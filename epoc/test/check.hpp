// Minimal assertion helper shared by the test programs. Deliberately not a
// framework: these tests need to run anywhere the library builds, with no
// extra dependency to fetch.

#ifndef XAVIER_EPOC_TEST_CHECK_HPP
#define XAVIER_EPOC_TEST_CHECK_HPP

#include <cstdio>
#include <string>

namespace tst {

inline int& failures() {
    static int n = 0;
    return n;
}

inline void section(const char* name) {
    std::printf("%s\n", name);
}

inline void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures();
}

/// Report a numeric comparison, printing both values when it fails so a
/// regression is diagnosable from the log alone.
inline void check_near(double got, double want, double rel_tol, const std::string& what) {
    const double denom = (want == 0.0) ? 1.0 : (want < 0 ? -want : want);
    const double diff = (got > want ? got - want : want - got);
    const bool ok = (diff / denom) <= rel_tol;
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        std::printf("  [FAIL] %s (got %.4f, want %.4f, rel err %.4f > %.4f)\n",
                    what.c_str(), got, want, diff / denom, rel_tol);
        ++failures();
    }
}

inline int summary() {
    const int f = failures();
    if (f == 0) {
        std::printf("\nall tests passed\n");
        return 0;
    }
    std::printf("\n%d test(s) FAILED\n", f);
    return 1;
}

}  // namespace tst

#endif  // XAVIER_EPOC_TEST_CHECK_HPP
