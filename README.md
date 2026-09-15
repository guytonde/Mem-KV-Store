# Mem KV Store

An in memory key value store in C++17. Header only, safe to use from many threads, and it can hand out a consistent snapshot of the whole store in constant time no matter how many keys it holds. No dependencies past the standard library. The mutex and reader writer lock it runs on are written here on top of `std::atomic` and the Linux futex syscall, there is no `std::mutex` anywhere.

DESIGN.md has details on the why. This file is the what and the how.

## Building and running

```
make            build tests, demo and benchmark into build/release
make test       run the 23 tests
make demo       run a short walkthrough
make bench      run the throughput and snapshot cost benchmark
make tsan       rebuild the tests under ThreadSanitizer and run them
make asan       rebuild under AddressSanitizer plus UBSan and run them
make clean      delete build/
```

To use it in your own program put `inc` on the include path and compile as C++17 with pthreads. Linux is the target for the futex syscall; other platforms compile but waiters fall back to a yielding spin.

## API

Everything is in the `kvstore` namespace. Include `kv_store.hpp` for the store and snapshots, and `gc_thread.hpp` if you want background reclamation.

```
KVStore<Key, Value, Hash = std::hash<Key>, KeyEqual = std::equal_to<Key>>
```

Same conventions as `std::unordered_map`. Value must be copyable (reads return copies) and default constructible (a delete is stored as a tombstone carrying an empty Value). `compare_and_put` also needs `operator==` on Value. The store cannot be copied or moved.

```
KVStore(shard_count = default_shard_count())
```

shard_count is the number of independent lock stripes. Default is twice the hardware thread count with a floor of 4 (8 if the count is unknown). 0 is treated as 1. More shards means less contention between writers to different keys and a slightly more expensive snapshot, since a snapshot scans one word per shard.

Live store operations:

```
get(key)                            optional<Value>, newest value or nullopt if absent or deleted
contains(key)                       bool
put(key, value)                     version the write committed at
erase(key)                          true if the key was live, false if it was already gone
compare_and_put(key, expected, desired)
                                    replace only if the current value equals expected,
                                    atomic on that key, false if missing, deleted or different
put_all(entries)                    vector or initializer list of pairs, all installed under
                                    one version, returns that version
clear()                             delete every live key under one version
size(), empty()                     live key count, O(keys), not atomic against writers
```

Live reads are consistent per key but not across keys. Two `get` calls can see a state that never existed as a whole. For a consistent multi key view take a snapshot.

```
snapshot()                          Snapshot pinned at the current commit watermark
```

Taking one copies nothing and never blocks writers. The cost is the same at a thousand keys or a million. The view never changes for as long as the snapshot exists. Copying a snapshot adds another pin on the same version, moving transfers it, and a snapshot stays valid after the store that made it is destroyed.

```
Snapshot::get(key)                  optional<Value> as of the pinned version
Snapshot::contains(key)
Snapshot::size(), empty()           walks the whole store
Snapshot::for_each(fn)              fn(key, value) over every live key, one shard read locked at a time
Snapshot::items()                   vector of pairs, the whole view materialized
Snapshot::version()                 the pinned version
```

Two things to know. The callback passed to `for_each` must not call back into the snapshot or the store; the locks are not reentrant and a nested read on the same shard deadlocks once a writer queues behind it. And consistent is not the same as fresh: a `put` that has already returned can be missing from a snapshot taken right after it, until a slower writer that drew a lower version on another shard lands. If you need read your writes, compare `snapshot().version()` against what `put` returned and retry.

Bookkeeping and reclamation:

```
version()                           current commit watermark
shard_count()
stats()                             Stats { shards, keys, live_keys, versions, longest_chain,
                                            live_snapshots, version }
gc()                                drop every entry no live snapshot can reach, returns the
                                    count removed
```

`gc` is never called from the write path. Run a long write workload without it and every superseded value is kept forever.

```
GcThread<Store>(store, interval = 50ms)
    reclaimed()                     total entries removed so far
    sweeps()                        number of gc calls so far
```

Calls `store.gc()` on a timer from its own thread. The destructor stops and joins it. Destroy it before the store it points at.

A short example:

```
store = KVStore<string, string>()
gc = GcThread(store)

store.put("user:1", "alice")
store.put("user:2", "bob")

snap = store.snapshot()
store.put("user:1", "alice2")
store.erase("user:2")

snap.get("user:1")      "alice"
snap.get("user:2")      "bob"
store.get("user:1")     "alice2"
store.get("user:2")     nullopt

store.put_all({{"a", "1"}, {"b", "2"}})
store.compare_and_put("a", "1", "3")
```

`make demo` runs a longer version of this and prints the stats struct as it goes.

## Layout

```
inc/futex.hpp       futex_wait, futex_wake, cpu_relax, spin count
inc/mutex.hpp       three state futex mutex, LockGuard
inc/rwmutex.hpp     writer preferring reader writer lock, SharedLock, ExclusiveLock
inc/core.hpp        shards, version chains, begin_write, end_write, watermark, SnapshotRegistry
inc/snapshot.hpp    Snapshot
inc/kv_store.hpp    KVStore, Stats
inc/gc_thread.hpp   GcThread
src/demo.cpp        what make demo runs
tests/              harness plus test_kv_store.cpp and test_sync.cpp
bench/bench.cpp     what make bench runs
.github/workflows   CI
```

## Performance

From `bench/bench.cpp` on this machine:

```
2x Intel Xeon Silver 4214 at 2.20 GHz, 12 cores each, hyperthreading on
48 hardware threads across 2 NUMA nodes, 62 GB RAM
Ubuntu 24.04, GCC 13.3, O3
```

The box was shared and not idle, so read the trends more than the absolute numbers. Keyspace of 100k keys, 200k ops per thread, 64 shards, GcThread sweeping every 20 ms.

Throughput in millions of ops per second by thread count and read mix:

```
threads    95% read    50% read    100% write
      1        2.33        0.82          0.52
      2        3.07        1.22          0.69
      4        5.82        1.71          1.17
      8        8.98        2.55          1.76
     16       12.69        3.24          2.07
     32       17.28        3.71          1.99
```

Shard count at 8 threads, 50% read:

```
     1 shard      0.66 Mops/s
   256 shards     5.48 Mops/s
```

Snapshot cost against store size:

```
      1000 keys    180 ns per snapshot
    100000 keys    179 ns per snapshot
   1000000 keys    180 ns per snapshot
```

Four scanner threads each taking a snapshot and walking all 10k keys through it sustain about 14.6k full scans per second while a writer updates the store the whole time.

Reads scale with shards since the read path is one shard read lock plus a hash lookup. Writes flatten out around 16 threads because every write does one atomic increment on the same global version counter and that cache line bouncing between cores is the cost past that point.

## License

GPL version 2, see LICENSE.
