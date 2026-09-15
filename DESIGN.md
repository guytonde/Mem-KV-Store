# Mem KV Store design notes

This is the long version of the README. The README tells you what the store does and how to call it. This file is about why it is built the way it is, what each piece is responsible for, and the places where I got it wrong once and had to rethink. If you are going to change anything in `core.hpp` read the watermark section first, that is where the correctness lives.

## What it is

An in memory key value store, header only, C++17, templated on `Key`, `Value`, `Hash` and `KeyEqual` the same way `std::unordered_map` is. Many threads can read and write at once. The thing it does that a map behind a lock does not is `snapshot()`: you get a consistent view of the whole store, the cost is a scan of one word per shard, and writers never stop while you hold it.

The other thing worth saying up front is that the locks are hand written. There is no `std::mutex` or `std::shared_mutex` anywhere. The mutex and the reader writer lock are built from `std::atomic` and the Linux futex syscall in `inc/`. That was the point of the project more than the store itself.

Things it deliberately does not do: persistence, cross process use (the futexes are private to one process), transactions beyond a single atomic batch, and lock free reads. Reads take a shard read lock. Why that is the stopping point is covered near the end.

## Layout

```
futex.hpp      wait and wake on an address, the only kernel boundary
mutex.hpp      three state futex mutex, guards the snapshot registry
rwmutex.hpp    writer preferring rwlock in one 32 bit word, one per shard
core.hpp       shards, version chains, the clock, the pin registry
snapshot.hpp   a pinned version plus a shared_ptr to the core
kv_store.hpp   the public API, multi shard locking, gc()
gc_thread.hpp  a thread that calls gc() on a timer
```

Nothing above `core.hpp` knows what a futex is and nothing below it knows what a version is. The tests split the same way, `test_sync.cpp` for the locks and `test_kv_store.cpp` for the store.

## The locks

futex.hpp is two wrappers over the syscall. `futex_wait(addr, expected)` sleeps only if `*addr` still equals `expected` when the kernel looks, otherwise it returns straight away. `futex_wake(addr, n)` wakes up to n sleepers on that address. That conditional check inside the kernel is the whole reason you can build a lock without a second lock protecting its own wait queue. The classic lost wakeup is: waiter reads the state, decides to sleep, and the releaser flips the state and wakes an empty queue in the gap. With a futex the waiter passes in the state it saw and the kernel refuses to park it if that has changed.

A few choices in this file matter later. It uses the `_PRIVATE` opcodes, which skip the shared mapping lookup and are faster, at the price that these locks only work inside one process. Put a store in shared memory and it will not fail loudly, it will just hang. The return value of the syscall is dropped, which is safe because every caller sits in a loop rechecking its own predicate, but it means nothing built on top can have a timeout without changing the signature. There is no `FUTEX_CMP_REQUEUE`, which is what would fix the thundering herd mentioned below. On non Linux platforms there is no wake path at all so `futex_wait` becomes a yield if the value still matches; correct because of the recheck loops, but it burns a core under contention. Linux is the target.

`cpu_relax()` is `pause` on x86 and `yield` on ARM, and `kSpinCount` is 64. Every primitive spins that many times before parking so a briefly held lock never touches the kernel.

mutex.hpp is the "mutex 2" from Drepper's Futexes Are Tricky. One word, three states: 0 unlocked, 1 locked with nobody parked, 2 locked and somebody might be parked.

```
lock:
    if cas(0 to 1) succeeds: return
    spin a few cas attempts
    prev = exchange(2)
    while prev != 0:
        futex_wait(&state, 2)
        prev = exchange(2)

unlock:
    if fetch_sub(1) == 1: return       # was 1, nobody parked, now 0
    store 0
    futex_wake(&state, 1)
```

The exchange to 2 rather than a cas is because a thread that just woke up has no idea whether other waiters remain, so it has to leave the word at 2 to force the next unlock into the kernel. Worst case is one spurious wake after a contended period. Uncontended lock and unlock is one cas and one fetch_sub, no syscall. This lock only guards the snapshot registry so its fast path is less important than the rwlock's.

rwmutex.hpp keeps all its state in one 32 bit word: bit 0 is writer active, bits 1 to 15 are the active reader count, bits 16 to 30 are the count of waiting writers. Both counters cap at 32767 and saturate instead of carrying into the neighbouring field. Every state change is a single compare exchange on that word, so there is never a window where the reader count and the writer bit disagree.

It prefers writers. A reader that arrives while `waiting writers > 0` parks instead of joining the current batch of readers. Without that a steady read load keeps the reader count above zero forever and no writer ever gets in; and `gc()` is a writer, so the store would leak. The writer bumps the waiting count before it starts spinning, not after it gives up and parks, so the preference kicks in right away. `rwmutex_does_not_starve_writers` hangs instead of failing if this regresses, which is hard to miss in CI.

Waiting is the part that took a while to get right. Waiters cannot park on the state word because it changes every time a reader enters or leaves and the waiter wants to sleep through those. So each side has its own sequence counter that only bumps on a release that matters to that side:

```
seq = counter.load()
if state now allows entry: retry without sleeping
futex_wait(&counter, seq)

release:
    counter.fetch_add(1)
    futex_wake_all(&counter)
```

If the increment lands between the waiter's load and its `futex_wait`, the kernel sees the mismatch and returns immediately. If it lands after, the wake finds the waiter. Either way nothing is lost. Last reader out wakes writers if any are waiting. A writer's unlock hands off to another writer if one is queued and otherwise releases the readers.

Wakes are broadcasts. Everyone parked on that counter wakes, one wins, the rest recheck and park again. Under heavy contention that is a thundering herd and the proper fix is requeueing or a ticket scheme. It has not shown up as a problem at the contention levels the benchmark reaches and broadcast is far simpler to reason about.

## Data model

```
Core
    shards[]      each is { RWMutex, atomic inflight, unordered_map<Key, Chain> }, 64 byte aligned
    clock         atomic u64, the global version counter, starts at 0
    snapshots     SnapshotRegistry: Mutex + map<version, pin count>

Chain = vector<Entry>, sorted ascending by version
Entry = { version, tombstone flag, value }
```

A key goes to `hash(key) % shard_count`. Each shard has its own lock and its own map, so writes to keys in different shards never share a lock or a cache line. The default shard count is twice the hardware thread count with a floor of 4. The `alignas(64)` on `Shard` keeps neighbouring shards off the same line; the struct is bigger than one line so it takes two, but the rwlock and `inflight` sit together in the first one which is the hot one.

Nothing is ever overwritten. A write appends `(version, value)` to the key's chain and a delete appends a tombstone. Reading at version V means walking the chain back from the end to the newest entry with `version <= V`. That is the entire MVCC mechanism. A snapshot is just a number, and reads through it are "find the right entry".

Chains are vectors, not linked lists, because gc keeps them short and a short vector walked from the back is a couple of cache lines. `find_at` does a linear scan from the back rather than a binary search for the same reason.

Two properties of chains that other code relies on. First, versions within a chain are strictly increasing, because a key lives in one shard, the shard lock is held exclusively for the whole write, and the version is drawn while holding it. `install()` has an out of order insertion branch but under current locking the only time it runs is when one `put_all` has the same key twice, in which case both entries carry the same version and the later one wins. Second, `KVStore::get` ignores versions entirely. It returns `chain.back()` under a read lock. That is always a fully completed write because the writer holds the shard exclusively until the entry is in, including across a whole multi shard `put_all`. Only snapshots pay for cross key consistency.

Version 0 is never handed out. The clock starts at 0 and the first write gets 1, which frees 0 up as a sentinel below.

## The write path

```
put(key, value):
    shard = hash(key) % n
    lock shard exclusive
        v = begin_write(shard)
        install(chain[key], {v, false, value})
        end_write(shard)
    unlock

put_all(entries):
    indices = sorted unique shard indices of every key
    lock them in ascending order
        v = begin_write(indices)         one version for the batch
        install every entry at v
        end_write(indices)
    unlock in reverse order
```

Ascending lock order across all multi shard operations is what makes deadlock impossible: two overlapping batches always fight over their lowest shared index first, and whoever wins holds a prefix of the other's order. `clear()` is `put_all` over every shard writing tombstones, and because it holds all the locks at once nobody can observe a half cleared store.

`compare_and_put` and `erase` check their precondition before calling `begin_write`, so a failed compare does not burn a version or touch the inflight marker.

## Versions and the watermark

This is the part that was wrong once and the part to understand fully before touching `core.hpp`.

Versions come from one global counter and writes finish out of order. Thread A draws 6, thread B draws 7, B finishes installing before A. If a snapshot just read the counter it would get 7, see B's write and not A's, and that is a gap in the history, which is exactly what a consistent snapshot must never show. So a snapshot needs the watermark: the highest W such that every write with `version <= W` is fully installed.

The first version of this had a second shared counter that writers advanced in order after installing. It was correct and it was terrible. Every commit serialized on one cache line and a slow writer held up every writer behind it, so adding threads made writes slower. The comment in `core.hpp` still records that. What it taught me is that the write path can afford exactly one shared atomic, the version draw, and nothing else.

The current design has each shard publish what it is doing in its own `inflight` field. Writers only ever touch their own shard. The watermark is derived by scanning all of them.

```
writer, holding its shard lock:
    inflight = kReserving        (1)  about to draw a version
    v = ++clock                  (2)
    inflight = v                 (3)  installing v
    ... install ...
    inflight = kNoWrite          (4)  done

watermark():
    safe = clock.load()                 read the clock FIRST
    for each shard:
        p = inflight.load()
        while p == kReserving: spin and reload
        if p != kNoWrite: safe = min(safe, p minus 1)
    return safe
```

`kNoWrite` is the max u64 and `kReserving` is 0. `begin_write` aborts if the clock ever reaches `kNoWrite`, which is 2^64 writes away, so that is a guard rather than a plan.

Why it is sound: take any write w with version v at or below the clock value C the scanner read. Step 2 of w happened before the scanner's clock read, that is what `v <= C` means. Step 1 came before step 2. So when the scanner reaches w's shard it sees either `kReserving` (spins until it becomes v, then excludes v), or v itself (excludes it), or something later in that field's history, which means step 4 already ran and w is fully installed. A later write cannot have started on that shard without w releasing the lock first. So every write at or below C is either excluded or done. A write that drew its version after the clock read has `v > C`, so v minus 1 is at least C and it cannot lower the result. This is why the clock is read before the shard fields and not after.

Two details here are not optional and both were learned by getting them wrong. The shard is marked `kReserving` before the version is drawn. Without that a writer draws 7, a scanner reads `clock = 7`, looks at the shard, sees `kNoWrite` because the publish has not landed, and returns 7 while write 7 is still installing. With the marker in place any scanner that could be affected sees something and waits a few instructions for the real number. And the exact version is published, not a lower bound. If a shard said "at least 5" instead of "7", one scan could return 6 and a later one 4, and the watermark would have gone backwards. Monotonicity is what the gc argument below rests on.

Monotonicity itself: scan A returns W_A having seen every write at or below W_A done. Scan B starts after A returns, those writes are still done, every marker B can see belongs to a write above W_A, and B's clock read is at least A's. So `W_B >= W_A`. `watermark_is_monotonic_under_concurrent_writes` checks this directly against four writers.

A batch publishes the same v on every shard it touches and clears them one at a time in `end_write`. Until the last clear lands at least one shard still shows v and holds the watermark below it, so a batch is either entirely at or below the watermark or entirely above it. The ledger test is the stress test for this.

Cost is O(shards), one cache line per shard that is nearly always in shared state. 64 shards is a couple hundred nanoseconds and it is the same at a thousand keys or a million. It is constant in data size, not constant, and a huge shard count trades snapshot cost for write concurrency, which is the right knob to have.

The one hazard is the spin on `kReserving`. `snapshot()` runs the scan with the registry mutex held, so a writer that gets preempted between steps 1 and 3 stalls every concurrent `snapshot()` and `gc()` until it is rescheduled. Bounded and rare, and the only place in the code where a lock is held while waiting on another thread. Publishing a lower bound instead would remove the spin but bring back the backwards watermark, so it stays.

## Snapshots

A `Snapshot` is a `shared_ptr<Core>` and a version. That is all it holds.

`KVStore::snapshot()` calls `SnapshotRegistry::pin_current`, which reads the watermark and increments the pin count for that version inside the same critical section on the registry mutex. The `Snapshot` constructor that takes over that pin is private with `KVStore` as a friend, because a snapshot built on any other version could sit above the install frontier or below the gc bound. Reading the watermark inside the lock is the entire safety story between snapshots and gc: `gc_bound()` takes the same lock, so a snapshot racing with gc is either already counted when gc looks or has not read the watermark yet and will read one at or above what gc used.

Copying a snapshot acquires another pin, moving steals it, the destructor releases. Because it holds a `shared_ptr` to the core the snapshot keeps the whole store alive and keeps working after the `KVStore` object is gone. `snapshot_outlives_the_store` covers that.

`Snapshot::get` takes the shard read lock, finds the chain, and walks back to the newest entry at or below the pinned version. A tombstone there means deleted as of this snapshot. The read lock is still needed even though the version already says which entry to return, because the chain is a vector and writers `push_back` (which can reallocate) and gc erases. The version answers which entry, the lock answers whether it is safe to touch the container. Dropping that lock means deferring reclamation of the container itself, which is the RCU or hazard pointer step and where this design stops.

Consistent is not the same as fresh. The pinned version is the watermark, and a slower writer that drew a lower version on another shard holds the watermark below it. So a `put` that has already returned can be missing from a snapshot taken right after, until the slower write lands. That is the price of not serializing commits. If you need read your writes, compare `snapshot().version()` against the version `put` returned and retry.

`for_each` walks one shard at a time under a read lock so writers on other shards keep going during a full scan. The locks are not reentrant, so the callback must not call back into the snapshot or the store. A nested read on the same shard deadlocks the moment a writer queues behind it.

## Reclamation

Chains grow forever if nothing trims them. `gc()`:

```
bound = oldest pinned version, or the current watermark if nothing is pinned
for each shard, under exclusive lock:
    for each chain:
        keep the newest entry with version <= bound and everything after it
        drop everything before
        if what is left is one tombstone at or below bound, drop the key
```

Every live snapshot has `version >= bound`, either because it is pinned and so at or above the oldest pin, or because it has not pinned yet and will pin at or above the current watermark. For any such version the entry `find_at` would return is either above bound (kept) or the newest at or below bound (kept). Nothing reachable is dropped. Dropping a tombstoned key changes "walk lands on a tombstone, return nothing" into "key not in map, return nothing", same answer.

Reclamation is off the write path on purpose. A writer should not pay for another thread's garbage. `GcThread` calls `gc()` on an interval from a background thread and sleeps in 5 ms slices so shutdown does not wait out a whole interval. The benchmark runs one at 20 ms; without it the numbers measure chain growth instead of the store. `gc()` sweeps every shard on every call and takes each lock exclusively, which briefly holds readers off that shard. Incremental sweeping would suit a very large store better.

A note on memory ordering while we are down here. The locks use acquire on acquisition and release on release, which is the standard pattern and the one ThreadSanitizer understands. The watermark protocol is `seq_cst` throughout: the three `inflight` stores, the clock increment, the scanner's loads. That is stronger than needed. The argument above only needs the scanner's clock load to synchronize with every earlier increment, which acquire on the load and release on the increment give through the release sequence, plus a release on the `kNoWrite` store so installs are visible before it. On x86 a `seq_cst` store is an `xchg` where a release store is a plain `mov`, so that is three locked instructions per write that could be plain stores. It is left at `seq_cst` because the correctness comment is written in those terms and the write bottleneck is the clock increment anyway. Relaxing it is a measured change for later, not a free one.

## Where the time goes

The README table has the numbers. Reads are shard local read locks and scale with shards. Writes plateau around 16 threads and stay there no matter how many you add.

The write plateau is `clock.fetch_add`. Every write in the process does one increment on one cache line, and that line bounces between every core running a writer. There is no way to remove it without giving up a single global order over versions, and the global order is what makes two snapshots comparable and what makes "a prefix of history" mean anything. It stays.

Two things that could be tightened without touching the design. `clock` and the registry mutex sit next to each other in `Core`, so every `snapshot()` locking the mutex bounces the same line every writer increments; `alignas(64)` on `clock` would separate them. And the `seq_cst` stores above.

## Tests

23 cases. The single threaded ones nail down semantics. The interesting ones are the invariant stress tests, because that is where a design like this actually breaks.

The ledger test moves money between accounts with `put_all` while four threads audit snapshots nonstop, and every audit must see the same total. A torn batch or a gap in the watermark fails it within milliseconds. The half installed write test uses a `Value` whose copy constructor blocks on a flag when it sees a sentinel payload. `put` copies the value after `begin_write`, so the write sits at `inflight = v` while stuck, and the test asserts `store.version() < v` and that a snapshot taken during the stall never sees the key even after the write finishes. The monotonicity test takes snapshots in a loop against four writers and checks the version never goes backwards and never exceeds `store.version()`. The starvation test has eight readers in a tight loop while one writer takes the lock a thousand times; if preference regresses, `join()` never returns. The gc under traffic test runs reclamation next to writers and scanners and looks for any read of freed or garbage data.

## Roads not taken

One global lock is the simplest thing and fails the "unrelated keys shouldn't contend" requirement on its face. Copy on snapshot is O(keys) and stops writers for the copy. A persistent functional map gives O(log n) snapshots through structural sharing but every write allocates a path of nodes, reads chase pointers, and making it concurrent needs either a global cas on the root or a much more complicated tree. MVCC over a flat hash map keeps the hot path a hash lookup.

Per shard version counters would remove the global increment and let writes scale linearly, but then version 6 on shard A and version 6 on shard B are unrelated, a snapshot is a vector of counters, and `put_all` atomicity across shards gets much harder. The global counter is the price of consistent snapshots.

Lock free reads via RCU or hazard pointers would drop the read lock and let reads scale further, but reclamation would then have to defer freeing chain storage until every reader has left, which is a grace period mechanism on top of the version refcounting that already exists. Two reclamation systems is more than this project wants to carry.

`std::shared_mutex` would have worked and saved the rwlock. I kinda just wanted to build it.

## Rough edges

A snapshot can miss a `put` that has already returned, see the snapshots section. Documented rather than fixed because fixing it inside `snapshot()` means waiting on the slower writer. `snapshot()` spins on `kReserving` with the registry mutex held. `KVStore` deletes copy and declares no move, so it is not movable even though it is a single `shared_ptr`. `futex_wait` has no timeout so nothing built on it can either. The non Linux fallback burns a core under contention. `gc()` sweeps every shard every call. Values are copied on read, so large values want to be wrapped in a `shared_ptr` by the caller. And the reader and waiting writer counts saturate at 32767; the 32768th just waits for one to leave, which is fine, but worth knowing if you ever see a reader stall with no writer in sight.
