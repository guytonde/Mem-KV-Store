#include <cstdio>

#include "test_framework.hpp"

int main() {
    std::printf("running %zu tests\n", testing::registry().size());
    return testing::run_all();
}
