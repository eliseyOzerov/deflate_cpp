// A small test runner: `TEST(name) { ... }` registers a test, `EXPECT(condition)` records a
// failure without stopping the test, and `EXPECT_PROBLEM(statement, words)` checks that the
// statement throws a problem whose sentence contains `words`. `make test` runs them all.
// The same runner as math_cpp's, without its math comparisons.
#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace editor::testing {

struct Test {
    std::string name;
    std::function<void()> body;
};

inline std::vector<Test>& tests() {
    static std::vector<Test> all;
    return all;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registration {
    Registration(std::string name, std::function<void()> body) { tests().push_back({std::move(name), std::move(body)}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    failures() += 1;
    std::cerr << file << ":" << line << ": " << what << "\n";
}

/// Whether two floats are within `tolerance` of each other.
inline bool isApproximatelyEqual(float first, float second, float tolerance = 1e-5f) {
    return std::fabs(first - second) <= tolerance;
}

} // namespace editor::testing

#define EDITOR_JOIN_INNER(first, second) first##second
#define EDITOR_JOIN(first, second) EDITOR_JOIN_INNER(first, second)

#define TEST(name)                                                                                         \
    static void name();                                                                                    \
    static ::editor::testing::Registration EDITOR_JOIN(registration_, name)(#name, name);                 \
    static void name()

#define EXPECT(condition)                                                                                  \
    do {                                                                                                   \
        if (!(condition)) ::editor::testing::fail(__FILE__, __LINE__, "expected " #condition);            \
    } while (false)

#define EXPECT_EQUAL(actual, expected)                                                                     \
    do {                                                                                                   \
        auto actualValue = (actual);                                                                       \
        auto expectedValue = (expected);                                                                   \
        if (!(actualValue == expectedValue)) {                                                             \
            std::ostringstream message;                                                                    \
            message << "expected " #actual " == " #expected ", got " << actualValue;                     \
            ::editor::testing::fail(__FILE__, __LINE__, message.str());                                    \
        }                                                                                                  \
    } while (false)

#define EXPECT_PROBLEM(statement, words)                                                                   \
    do {                                                                                                   \
        try {                                                                                              \
            statement;                                                                                     \
            ::editor::testing::fail(__FILE__, __LINE__, "expected a problem from " #statement);           \
        } catch (const std::exception& problem) {                                                          \
            if (std::string(problem.what()).find(words) == std::string::npos) {                            \
                ::editor::testing::fail(                                                                   \
                    __FILE__, __LINE__, std::string("expected a problem with \"") + (words) + "\", got \"" \
                                            + problem.what() + "\"");                                      \
            }                                                                                              \
        }                                                                                                  \
    } while (false)
