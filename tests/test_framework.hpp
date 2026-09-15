#pragma once
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

// tiny test harness, register cases, run them, report failures
namespace testing {

struct TestCase {
    std::string name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct Failure : std::exception {
    std::string message;
    explicit Failure(std::string m) : message(std::move(m)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

inline int run_all() {
    int failed = 0;
    for (const auto& test : registry()) {
        try {
            test.fn();
            std::printf("  [ ok ] %s\n", test.name.c_str());
        } catch (const std::exception& error) {
            std::printf("  [FAIL] %s\n         %s\n", test.name.c_str(), error.what());
            ++failed;
        }
    }
    std::printf("\n%zu tests, %d failed\n", testing::registry().size(), failed);
    return failed == 0 ? 0 : 1;
}

} // namespace testing

#define TEST(name)                                                     \
    static void name();                                                \
    static ::testing::Registrar registrar_##name(#name, &name);        \
    static void name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            throw ::testing::Failure(std::string(__FILE__ ":") + std::to_string(__LINE__) + \
                                     " CHECK failed: " #cond);                         \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        auto lhs__ = (a);                                                              \
        auto rhs__ = (b);                                                              \
        if (!(lhs__ == rhs__)) {                                                       \
            throw ::testing::Failure(std::string(__FILE__ ":") + std::to_string(__LINE__) + \
                                     " CHECK_EQ failed: " #a " == " #b);               \
        }                                                                              \
    } while (0)
