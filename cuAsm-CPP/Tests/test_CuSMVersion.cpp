#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../CuAsm/CuSMVersion.hpp"

namespace {

int failures = 0;

/**
 * @brief Runs fn and records a failure if it throws anything.
 * @param label Human-readable description of what fn does, used in the pass/fail log line.
 * @param fn Operation expected to complete without throwing.
 **/
void expectNoThrow(const std::string& label, const std::function<void()>& fn) {
    try {
        fn();
        std::cout << "[PASS] " << label << " constructed cleanly\n";
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << label << " unexpectedly threw: " << e.what() << "\n";
        ++failures;
    }
}

/**
 * @brief Runs fn and records a failure if it does not throw exactly std::invalid_argument. This
 *        is the "cleanly fail" contract for an unsupported architecture: a catchable, well-typed
 *        exception with a diagnostic message, not a crash, hang, or silently-wrong CuSMVersion.
 * @param label Human-readable description of what fn does, used in the pass/fail log line.
 * @param fn Operation expected to throw std::invalid_argument.
 **/
void expectInvalidArgument(const std::string& label, const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::invalid_argument& e) {
        std::cout << "[PASS] " << label << " threw std::invalid_argument: " << e.what() << "\n";
        return;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << label << " threw the wrong exception type: " << e.what() << "\n";
        ++failures;
        return;
    }
    std::cerr << "[FAIL] " << label << " did not throw\n";
    ++failures;
}

} // namespace

/**
 * @brief Verifies CuSMVersion's construction-time validation: every architecture actually backed
 *        by data (either a shipped DefaultInsAsmRepos, or an InsAsmReposAliasDict entry pointing
 *        at one) must construct cleanly via both constructors and a few string spellings; every
 *        other version - including ones CuSMVersion used to accept before validSMVersions() was
 *        trimmed down to just those 9 - must fail cleanly with std::invalid_argument instead of
 *        silently producing a non-functional instance.
 * @return 0 if every case behaved as expected, 1 otherwise.
 **/
int main() {
    // Directly supported (has a shipped DefaultInsAsmRepos) or aliased to one via
    // CuSMVersion::InsAsmReposAliasDict (62->61, 72->75, 87->86), so fully functional.
    const std::vector<int> supported = {60, 61, 62, 70, 72, 75, 80, 86, 87};
    for (int v : supported) {
        const std::string vs = std::to_string(v);
        expectNoThrow("CuSMVersion(" + vs + ")", [v] { CuAsm::CuSMVersion cv(v); });
        expectNoThrow("CuSMVersion(\"" + vs + "\")", [vs] { CuAsm::CuSMVersion cv(vs); });
        expectNoThrow("CuSMVersion(\"sm_" + vs + "\")", [vs] { CuAsm::CuSMVersion cv("sm_" + vs); });
        expectNoThrow("CuSMVersion(\"SM_" + vs + "\")", [vs] { CuAsm::CuSMVersion cv("SM_" + vs); });
    }

    // Previously accepted by validSMVersions() but with neither a repos file nor an alias entry -
    // constructing these used to succeed and only fail later (and silently, falling back to an
    // empty repos) deep inside CuInsAssemblerRepos::setToDefaultInsAsmDict. They must now be
    // rejected immediately, at construction.
    const std::vector<int> noLongerAccepted = {35, 37, 50, 52, 53, 89, 90};
    for (int v : noLongerAccepted) {
        const std::string vs = std::to_string(v);
        expectInvalidArgument("CuSMVersion(" + vs + ")", [v] { CuAsm::CuSMVersion cv(v); });
        expectInvalidArgument("CuSMVersion(\"" + vs + "\")", [vs] { CuAsm::CuSMVersion cv(vs); });
        expectInvalidArgument("CuSMVersion(\"sm_" + vs + "\")", [vs] { CuAsm::CuSMVersion cv("sm_" + vs); });
    }

    // Never a real architecture at all.
    expectInvalidArgument("CuSMVersion(0)", [] { CuAsm::CuSMVersion cv(0); });
    expectInvalidArgument("CuSMVersion(-1)", [] { CuAsm::CuSMVersion cv(-1); });
    expectInvalidArgument("CuSMVersion(999)", [] { CuAsm::CuSMVersion cv(999); });
    expectInvalidArgument("CuSMVersion(\"sm_999\")", [] { CuAsm::CuSMVersion cv("sm_999"); });
    expectInvalidArgument("CuSMVersion(\"not_an_arch\")", [] { CuAsm::CuSMVersion cv("not_an_arch"); });
    expectInvalidArgument("CuSMVersion(\"\")", [] { CuAsm::CuSMVersion cv(""); });

    return failures == 0 ? 0 : 1;
}
