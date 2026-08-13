#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>

namespace CuAsm::Test {

/**
 * @brief Tiny hand-rolled assertion/reporting helper shared by the CuAsm/utils/ unit tests,
 *        mirroring the PASS/FAIL-per-line convention already used elsewhere in this test suite
 *        (see Tests/CheckDisasm/CheckDisasmCommon.hpp, Tests/test_CuSMVersion.cpp) rather than
 *        pulling in a test framework dependency. Each test .cpp owns one TestReporter and folds
 *        many individual checks into that single executable's pass/fail exit code.
 **/
class TestReporter {
public:
    /** @brief Records a pass/fail line for a boolean condition. */
    void check(const std::string& label, bool condition) {
        if (condition) {
            std::cout << "[PASS] " << label << std::endl;
        } else {
            std::cerr << "[FAIL] " << label << std::endl;
            ++mFailures;
        }
    }

    /**
     * @brief Records a pass/fail line comparing two equality-comparable, stream-printable values.
     * @param label Human-readable description of what's being compared.
     * @param actual Value produced by the code under test.
     * @param expected Value it should equal.
     **/
    template <typename T, typename U>
    void checkEqual(const std::string& label, const T& actual, const U& expected) {
        if (actual == expected) {
            std::cout << "[PASS] " << label << std::endl;
        } else {
            std::ostringstream as;
            std::ostringstream es;
            as << actual;
            es << expected;
            std::cerr << "[FAIL] " << label << ": expected " << es.str() << ", got " << as.str() << std::endl;
            ++mFailures;
        }
    }

    /** @brief Runs fn and records a failure if it throws anything. */
    void checkNoThrow(const std::string& label, const std::function<void()>& fn) {
        try {
            fn();
            std::cout << "[PASS] " << label << " completed without throwing" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << label << " unexpectedly threw: " << e.what() << std::endl;
            ++mFailures;
        }
    }

    /**
     * @brief Runs fn and records a failure unless it throws exactly ExceptionT.
     * @tparam ExceptionT Expected exception type (must derive from std::exception).
     * @param label Human-readable description of the operation.
     * @param fn Operation expected to throw ExceptionT.
     **/
    template <typename ExceptionT>
    void checkThrows(const std::string& label, const std::function<void()>& fn) {
        try {
            fn();
        } catch (const ExceptionT& e) {
            std::cout << "[PASS] " << label << " threw as expected: " << e.what() << std::endl;
            return;
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << label << " threw the wrong exception type: " << e.what() << std::endl;
            ++mFailures;
            return;
        }
        std::cerr << "[FAIL] " << label << " did not throw" << std::endl;
        ++mFailures;
    }

    /** @brief Prints a summary line and returns the process exit code (0 if every check passed). */
    int finish(const std::string& suiteName) const {
        if (mFailures == 0) {
            std::cout << "==== " << suiteName << ": all checks passed ====" << std::endl;
        } else {
            std::cerr << "==== " << suiteName << ": " << mFailures << " check(s) failed ====" << std::endl;
        }
        return mFailures == 0 ? 0 : 1;
    }

private:
    int mFailures = 0;
};

} // namespace CuAsm::Test
