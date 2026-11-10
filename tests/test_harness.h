// Minimal self-registering test harness (no third-party dependency).
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mt {

struct Case {
    const char *name;
    std::function<void()> body;
};

inline std::vector<Case> &registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int &failure_count() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char *name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

inline int run_all() {
    int failed_cases = 0;
    for (const Case &test : registry()) {
        const int before = failure_count();
        test.body();
        const bool ok = failure_count() == before;
        std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", test.name);
        failed_cases += ok ? 0 : 1;
    }
    std::printf("%zu tests, %d failed\n", registry().size(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace mt

#define MT_CONCAT_INNER(a, b) a##b
#define MT_CONCAT(a, b) MT_CONCAT_INNER(a, b)
#define MT_TEST(name)                                                                       \
    static void MT_CONCAT(mt_fn_, __LINE__)();                                              \
    static ::mt::Registrar MT_CONCAT(mt_reg_, __LINE__)(name, MT_CONCAT(mt_fn_, __LINE__)); \
    static void MT_CONCAT(mt_fn_, __LINE__)()

#define MT_CHECK(cond)                                                                       \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            ++::mt::failure_count();                                                         \
            std::fprintf(stderr, "  check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                                    \
    } while (false)

#define MT_CHECK_NEAR(a, b, tol)                                                                     \
    do {                                                                                             \
        const double mt_a = static_cast<double>(a);                                                  \
        const double mt_b = static_cast<double>(b);                                                  \
        if (!(mt_a - mt_b <= (tol) && mt_b - mt_a <= (tol))) {                                       \
            ++::mt::failure_count();                                                                 \
            std::fprintf(stderr, "  check failed: %s ~ %s (%g vs %g) (%s:%d)\n", #a, #b, mt_a, mt_b, \
                         __FILE__, __LINE__);                                                        \
        }                                                                                            \
    } while (false)

#define MT_CHECK_THROWS(expr, exception_type)                                                         \
    do {                                                                                              \
        bool mt_threw = false;                                                                        \
        try {                                                                                         \
            (void)(expr);                                                                             \
        } catch (const exception_type &) {                                                            \
            mt_threw = true;                                                                          \
        }                                                                                             \
        if (!mt_threw) {                                                                              \
            ++::mt::failure_count();                                                                  \
            std::fprintf(stderr, "  expected %s from %s (%s:%d)\n", #exception_type, #expr, __FILE__, \
                         __LINE__);                                                                   \
        }                                                                                             \
    } while (false)
