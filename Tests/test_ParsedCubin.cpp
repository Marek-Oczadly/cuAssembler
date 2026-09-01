#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>

#include "../include/CuAsmTools/ParsedCubin.hpp"
#include "utils/TestUtilsCommon.hpp"

namespace fs = std::filesystem;
using CuAsm::Tools::CheckStatus;
using CuAsm::Tools::CubinVerificationReport;
using CuAsm::Tools::ParsedCubin;
using CuAsm::Tools::SassKernel;
using CuAsm::Tools::parseCubin;
using CuAsm::Tools::verifyCubinControlCodes;

namespace {

const SassKernel* findKernel(const ParsedCubin& pc, const std::string& name) {
    for (const auto& k : pc.m_Kernels) {
        if (k.m_Name == name) {
            return &k;
        }
    }
    return nullptr;
}

} // namespace

/**
 * @brief Exercises CuAsm::Tools::parseCubin()/ParsedCubin against the same cudatest.6.sm_61.cubin
 *        fixture test_CubinFile.cpp uses, checking that: the exposed SASS text is clean (no
 *        control-code/address-comment leakage), an unedited round trip re-assembles cleanly, an
 *        edited instruction's new text actually lands in the re-assembled cubin (proving
 *        SassLine::m_Text is genuinely load-bearing, not decorative), and swapping two
 *        SassLine entries (proving movability) still re-assembles cleanly.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    const std::string cubinPath = std::string(CUASM_TESTDATA_DIR) + "/CuTest/cudatest.6.sm_61.cubin";
    const std::string outCubinPath = std::string(CUASM_TESTDATA_DIR) + "/CuTest/tmp_test_parsedcubin_out.cubin";

    ParsedCubin pc = parseCubin(cubinPath);

    t.check("parsed cubin exposes both known kernels from the fixture", findKernel(pc, "_Z7argtestPiS_S_") != nullptr &&
                                                                              findKernel(pc, "_Z10local_testiiPi") != nullptr);

    const SassKernel* localTest = findKernel(pc, "_Z10local_testiiPi");
    t.check("local_test kernel has a non-trivial instruction count", localTest != nullptr && localTest->m_Lines.size() > 20);

    if (localTest != nullptr) {
        bool anyLeaksControlCodeOrComment = false;
        for (const auto& line : localTest->m_Lines) {
            if (line.m_Text.find("B------") != std::string::npos || line.m_Text.find("/*") != std::string::npos) {
                anyLeaksControlCodeOrComment = true;
            }
        }
        t.check("no SassLine::m_Text leaks a control code or address comment", !anyLeaksControlCodeOrComment);

        bool foundTargetInstruction = false;
        std::size_t targetIdx = 0;
        for (std::size_t i = 0; i < localTest->m_Lines.size(); ++i) {
            if (localTest->m_Lines[i].m_Text == "IADD32I R0, R4.reuse, 0x1 ;") {
                foundTargetInstruction = true;
                targetIdx = i;
                break;
            }
        }
        t.check("found the known 'IADD32I R0, R4.reuse, 0x1 ;' instruction to edit", foundTargetInstruction);

        if (foundTargetInstruction) {
            // Edit: change the immediate. Purely a text edit -- no control-code/label plumbing touched.
            localTest = nullptr; // drop the const* before mutating
            for (auto& k : pc.m_Kernels) {
                if (k.m_Name == "_Z10local_testiiPi") {
                    k.m_Lines[targetIdx].m_Text = "IADD32I R0, R4.reuse, 0x2 ;";

                    // Movability: swap two adjacent standalone (non-dual-issue-paired) instructions.
                    std::swap(k.m_Lines[5], k.m_Lines[6]);
                    break;
                }
            }
        }
    }

    t.checkNoThrow("saveAsCubin() re-encodes the edited/reordered instruction stream without throwing",
                    [&]() { pc.saveAsCubin(outCubinPath); });

    t.check("saveAsCubin() writes a non-trivially-sized cubin", fs::exists(outCubinPath) && fs::file_size(outCubinPath) > 1000);

    if (fs::exists(outCubinPath)) {
        t.checkNoThrow("the re-assembled cubin can itself be parsed back", [&]() {
            ParsedCubin pc2 = parseCubin(outCubinPath);
            const SassKernel* localTest2 = findKernel(pc2, "_Z10local_testiiPi");
            if (localTest2 == nullptr) {
                throw std::runtime_error("_Z10local_testiiPi kernel missing from re-assembled cubin");
            }

            bool sawEditedImmediate = false;
            bool sawOldImmediate = false;
            for (const auto& line : localTest2->m_Lines) {
                if (line.m_Text == "IADD32I R0, R4.reuse, 0x2 ;") sawEditedImmediate = true;
                if (line.m_Text == "IADD32I R0, R4.reuse, 0x1 ;") sawOldImmediate = true;
            }
            if (!sawEditedImmediate || sawOldImmediate) {
                throw std::runtime_error("edited immediate did not round-trip into the re-assembled cubin");
            }
        });
    }

    std::error_code ec;
    fs::remove(outCubinPath, ec);

    // --- sm_75+ scenario: saveAsCubin()'s auto validate/correct-control-codes step (only wired
    // up for Turing/sm_75+, see ParsedCubin.hpp's file-level doc) ---
    const std::string sm75CubinPath = std::string(CUASM_TESTDATA_DIR) + "/CheckDisasm/AtomicOps/AtomicOps.sm_75.orig.cubin";
    const std::string sm75OutPath = std::string(CUASM_TESTDATA_DIR) + "/CheckDisasm/AtomicOps/tmp_test_parsedcubin_sm75_out.cubin";

    ParsedCubin pc75 = parseCubin(sm75CubinPath);

    // Reverses the whole "SHF.R.S32.HI R3, RZ, 0x1f, R0 ;" .. "IMAD.WIDE R4, R5, R12, c[0x0][0x160] ;"
    // instruction run (18 instructions, several genuine RAW chains: SHF.R->LEA.HI->LOP3.LUT->
    // IMAD.IADD->IMAD.WIDE->ATOMG on R3, ULDC.64 producing UR4 that RED.E.MAX later reads, ...).
    // This is not a legal topological reorder, so the (now stale) control codes each instruction
    // carries along with it are provably wrong for its new neighbors -- confirmed independently:
    // reassembling this exact reversal via cuasm/verify-cc without correction reports 2 real
    // unresolved hazards (a WAR on PRED0 and one on UGPR4). saveAsCubin()'s auto-correct step
    // exists precisely to close hazards like this one.
    std::optional<std::size_t> revStart;
    std::optional<std::size_t> revEnd;
    for (auto& k : pc75.m_Kernels) {
        if (k.m_Name != "AtomicOps") {
            continue;
        }
        for (std::size_t i = 0; i < k.m_Lines.size(); ++i) {
            if (k.m_Lines[i].m_Text == "SHF.R.S32.HI R3, RZ, 0x1f, R0 ;") {
                revStart = i;
            }
            if (k.m_Lines[i].m_Text == "IMAD.WIDE R4, R5, R12, c[0x0][0x160] ;") {
                revEnd = i;
            }
        }
        if (revStart && revEnd && *revStart < *revEnd) {
            std::reverse(k.m_Lines.begin() + static_cast<std::ptrdiff_t>(*revStart),
                         k.m_Lines.begin() + static_cast<std::ptrdiff_t>(*revEnd) + 1);
        }
        break;
    }
    t.check("found and reversed the target instruction run in AtomicOps", revStart.has_value() && revEnd.has_value() && revStart < revEnd);

    t.checkNoThrow("saveAsCubin() repairs control codes left invalid by a real dependency-breaking reorder",
                    [&]() { pc75.saveAsCubin(sm75OutPath); });

    if (fs::exists(sm75OutPath)) {
        t.checkNoThrow("the auto-corrected sm_75 output independently re-verifies clean", [&]() {
            const CubinVerificationReport vr = verifyCubinControlCodes(sm75OutPath);
            for (const auto& kr : vr.kernels) {
                if (kr.result.status != CheckStatus::Verified) {
                    throw std::runtime_error("kernel \"" + kr.kcc.kernelName +
                                              "\" is not Verified after saveAsCubin()'s auto-correct (status=" +
                                              std::to_string(static_cast<int>(kr.result.status)) + ")");
                }
            }
        });
    }
    fs::remove(sm75OutPath, ec);

    return t.finish("test_ParsedCubin");
}
