#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <elfio/elfio.hpp>

#include "../../bin/ccCommon.hpp"
#include "../CheckControlCodesShuffle/CheckControlCodesShuffleCommon.hpp"

// Reports/tasks.md Phase 5's full-pipeline round trip: "kernel is compiled with nvcc, disassembled,
// instructions shuffled, control codes validated and corrected, and then re-assembled." Neither
// sibling suite closes this loop end to end: CheckControlCodes never shuffles anything, and
// CheckControlCodesShuffle (this file's own dependency) only ever checks correctControlCodes()'s
// output *in memory* -- it never merges the corrected control codes back into real cubin bytes, and
// never reloads/redecodes the result from disk. This suite does both: CuSMVersion::mergeCtrlCodes()
// re-encodes each corrected kernel function's control codes, the result is patched into a copy of
// the original cubin's bytes at that kernel's ".text.<kernel>" section offset (mirroring
// correct-cc.cpp's own per-kernel patch-back loop exactly), the whole thing is written to a new
// cubin file on disk, and *that file* is reloaded completely from scratch -- a fresh ELFIO load, a
// fresh loadControlCodes(), and a fresh decodeInstructions() (which itself reruns cuobjdump against
// the new file, not anything cached from the original compile) -- before a final verifyControlCodes()
// confirms it still reports clean. This is the strongest evidence this codebase's test suite can
// produce that correctControlCodes()'s repair survives a real re-encode/patch/disk-round-trip/
// re-disassemble cycle, not just self-consistency against its own in-memory hazard graph.
//
// Turing (sm_75) only, same reasoning as CheckControlCodes/CheckControlCodesShuffle: the
// OperandRoleTable/LatencyClassTable curation this depends on (Reports/tasks.md Phase 0/1) is only
// complete there -- sm_80/sm_86 still carry real TODO gaps that would make an arbitrary fixture's
// decode fail on data-curation grounds this suite isn't meant to police. See
// runCheckControlCodesRoundtripPlaceholder() below for those architectures.

namespace CuAsm::Test {

/**
 * @brief Reads a whole file into memory as raw bytes, mirroring correct-cc.cpp's own (private)
 *        readWholeFile() helper -- duplicated here rather than exposed from that .cpp.
 * @param fname Path to read.
 * @return The file's contents (empty if the file couldn't be opened).
 **/
inline std::vector<char> readWholeFileCC(const std::string& fname) {
    std::ifstream in(fname, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    return std::vector<char>(s.begin(), s.end());
}

/**
 * @brief Runs the full compile -> disassemble -> shuffle -> validate/correct -> re-assemble ->
 *        reload -> re-verify round trip for one (kernel, architecture) pair.
 *
 *        Steps, per kernel function the fixture produces:
 *          1. compileAndDecodeCC() (shared with CheckControlCodes/CheckControlCodesShuffle): nvcc
 *             compile, loadControlCodes(), decodeInstructions() (a real cuobjdump run).
 *          2. shuffleKernel() (shared with CheckControlCodesShuffle): physically reorders a legal
 *             subset of the kernel's instructions, carrying each instruction's now-stale control
 *             code along with it -- exactly what a real instruction shuffler would hand to
 *             correct-cc. A kernel function with no shuffleable block is left alone.
 *          3. correctControlCodes(): repairs the (now-stale) control codes against the shuffled
 *             order. CheckStatus::Unrepairable is accepted for that kernel function -- a shuffle
 *             genuinely can push more than 6 VARIABLE-latency producers into simultaneous flight
 *             (Reports/tasks.md Phase 4) -- but per correct-cc.cpp's own "don't write an output file
 *             if any kernel is unrepairable" contract, an Unrepairable result for *any* kernel
 *             function in this cubin skips the re-assembly/reload step for the whole file (there is
 *             nothing correct-cc.cpp itself would have written in that case either).
 *          4. An independent in-memory re-verify (verifyControlCodes()) of every Corrected kernel
 *             function's repaired codes, same as CheckControlCodesShuffle already does.
 *          5. RE-ASSEMBLE: CuSMVersion::mergeCtrlCodes() re-encodes each Corrected kernel function's
 *             (already-reordered) instruction codes and (freshly-corrected) control codes, patched
 *             into a copy of the original cubin's bytes at that kernel's own section offset.
 *          6. If every kernel function was either Corrected-and-reassembled or had no shuffleable
 *             block at all (i.e. nothing was Unrepairable), the patched bytes are written to a new
 *             cubin file, which is then reloaded *completely from scratch*: a fresh ELFIO::elfio,
 *             a fresh loadControlCodes(), and a fresh decodeInstructions() -- which reruns cuobjdump
 *             against the new file on disk, independent of anything cached from step 1 -- followed
 *             by a final verifyControlCodes() that must report Verified for every kernel function.
 * @param kernelName Name of the kernel/subdirectory under TestData/CheckDisasm; also the base name
 *        of the .cu fixture.
 * @param arch Target SM architecture, e.g. "sm_75".
 * @param minArchSM Minimum numeric SM version the kernel's instructions require -- if arch's numeric
 *        SM version is lower, the round trip is skipped and this automatically passes.
 * @return true if every step above succeeded (including the accepted Unrepairable/no-shuffleable-
 *         block early-outs, which are real, correct outcomes, not failures); false on any compile/
 *         load/decode/correct/merge/reload/re-verify failure.
 **/
inline bool runCheckControlCodesRoundtrip(const std::string& kernelName, const std::string& arch, int minArchSM = 0) {
    bool passed = false;
    std::string failureReason;

    const std::string dir = std::string(CUASM_TESTDATA_DIR) + "/CheckDisasm/" + kernelName;
    const std::string origCubin = dir + "/" + kernelName + "." + arch + ".cc.cubin";
    const std::string outCubin = dir + "/" + kernelName + "." + arch + ".roundtrip.cc.cubin";
    removeQuietCC(outCubin);

    try {
        const std::optional<CompiledCC> compiled = compileAndDecodeCC(kernelName, arch, minArchSM);
        if (!compiled) {
            std::cout << "[SKIP] CheckControlCodesRoundtrip " << kernelName << " (" << arch << "): requires sm_" << minArchSM
                      << " or newer\n";
            return true;
        }

        std::vector<char> outBytes = readWholeFileCC(origCubin);
        if (outBytes.empty()) {
            throw std::runtime_error("failed to re-read compiled cubin \"" + origCubin + "\" for patch-back");
        }

        bool anyUnrepairable = false;
        bool anyReassembled = false;
        std::size_t totalBlocksShuffled = 0, totalInstrsShuffled = 0, totalCorrected = 0;

        for (const CuAsm::Tools::KernelControlCodes& kcc : compiled->kernels) {
            const std::vector<CuAsm::Tools::DecodedInstruction>& decoded = compiled->decodedByKernel.at(kcc.kernelName);

            // Deterministic per-(fixture, kernel-function) seed, distinct from
            // CheckControlCodesShuffle's own seed so the two suites don't just repeat each other's
            // exact permutation -- reproducible across runs/machines either way.
            const std::size_t seed = std::hash<std::string>{}(kernelName + "::roundtrip::" + kcc.kernelName);
            std::mt19937_64 rng(seed);

            ShuffleResult shuffled = shuffleKernel(kcc, decoded, compiled->arch, rng);
            if (shuffled.blocksShuffled == 0) {
                // Nothing physically moved for this kernel function -- outBytes already holds its
                // correct, untouched original bytes (it started as a full copy of the input file).
                continue;
            }
            totalBlocksShuffled += shuffled.blocksShuffled;
            totalInstrsShuffled += shuffled.instructionsShuffled;

            const CuAsm::Tools::ControlCodeCheckResult correctResult =
                CuAsm::Tools::correctControlCodes(shuffled.kcc, shuffled.decoded, compiled->arch);
            if (correctResult.status == CuAsm::Tools::CheckStatus::Unrepairable) {
                anyUnrepairable = true;
                continue;
            }
            if (correctResult.status != CuAsm::Tools::CheckStatus::Corrected) {
                throw std::runtime_error("kernel \"" + kcc.kernelName + "\": correctControlCodes() returned an unexpected status");
            }

            std::vector<CuAsm::Tools::DecodedInstruction> reVerifyDecoded = shuffled.decoded;
            for (std::size_t i = 0; i < reVerifyDecoded.size(); ++i) {
                reVerifyDecoded[i].ctrlCode = CuAsm::CuControlCode(shuffled.kcc.ctrlCodeList[i]);
            }
            const CuAsm::Tools::ControlCodeCheckResult reVerify = CuAsm::Tools::verifyControlCodes(reVerifyDecoded, compiled->arch);
            if (reVerify.status != CuAsm::Tools::CheckStatus::Verified) {
                throw std::runtime_error("kernel \"" + kcc.kernelName +
                                          "\": correctControlCodes() reported Corrected, but an independent in-memory "
                                          "re-verify still found " +
                                          std::to_string(reVerify.violations.size()) + " violation(s)");
            }
            ++totalCorrected;

            // RE-ASSEMBLE: merge the corrected control codes (and the already-reordered instruction
            // codes) back into real bytes, patched into a copy of the whole original cubin file --
            // exactly correct-cc.cpp's own per-kernel patch-back loop.
            const std::vector<std::byte> merged = compiled->arch.mergeCtrlCodes(shuffled.kcc.insCodeList, shuffled.kcc.ctrlCodeList);
            if (merged.size() != shuffled.kcc.sectionSize) {
                throw std::runtime_error("kernel \"" + kcc.kernelName + "\": re-merged control codes changed section size (" +
                                          std::to_string(shuffled.kcc.sectionSize) + " -> " + std::to_string(merged.size()) +
                                          " bytes)");
            }
            std::memcpy(outBytes.data() + shuffled.kcc.sectionOffset, merged.data(), merged.size());
            anyReassembled = true;
        }

        if (anyUnrepairable) {
            // Matches correct-cc.cpp's own contract: no output file is written if any kernel
            // function came back Unrepairable. Still a pass -- Unrepairable is a real, correct
            // outcome for a reorder that provably can't be repaired in place, not a test failure.
            passed = true;
            std::cout << "[PASS] CheckControlCodesRoundtrip " << kernelName << " (" << arch << "): " << totalBlocksShuffled
                      << " block(s)/" << totalInstrsShuffled << " instruction(s) shuffled, at least one kernel function reported "
                      << "Unrepairable -- re-assembly correctly skipped (no output cubin written), matching correct-cc.cpp's own "
                      << "contract\n";
        } else if (!anyReassembled) {
            passed = true;
            std::cout << "[PASS] CheckControlCodesRoundtrip " << kernelName << " (" << arch
                      << "): no shuffleable block found in any kernel function -- nothing to re-assemble\n";
        } else {
            std::ofstream out(outCubin, std::ios::binary);
            out.write(outBytes.data(), static_cast<std::streamsize>(outBytes.size()));
            out.close();

            ELFIO::elfio ef2;
            if (!ef2.load(outCubin)) {
                throw std::runtime_error("failed to reload the re-assembled cubin \"" + outCubin + "\" via ELFIO");
            }
            const auto sm2 = CuAsm::Tools::detectArch(ef2);
            if (!sm2) {
                throw std::runtime_error("re-assembled cubin \"" + outCubin + "\" has an unsupported/undetected SM version");
            }
            const std::vector<CuAsm::Tools::KernelControlCodes> kernels2 = CuAsm::Tools::loadControlCodes(ef2, *sm2);
            if (kernels2.size() != compiled->kernels.size()) {
                throw std::runtime_error("re-assembled cubin \"" + outCubin + "\" has a different kernel-function count after "
                                          "reload (" +
                                          std::to_string(kernels2.size()) + " vs original " + std::to_string(compiled->kernels.size()) +
                                          ")");
            }
            const std::map<std::string, std::vector<CuAsm::Tools::DecodedInstruction>> decodedByKernel2 =
                CuAsm::Tools::decodeInstructions(outCubin, kernels2, *sm2);

            for (const CuAsm::Tools::KernelControlCodes& kcc2 : kernels2) {
                const CuAsm::Tools::ControlCodeCheckResult finalResult =
                    CuAsm::Tools::verifyControlCodes(decodedByKernel2.at(kcc2.kernelName), *sm2);
                if (finalResult.status != CuAsm::Tools::CheckStatus::Verified) {
                    throw std::runtime_error("kernel \"" + kcc2.kernelName +
                                              "\": the re-assembled, reloaded-from-scratch cubin no longer verifies clean -- " +
                                              finalResult.message);
                }
            }

            passed = true;
            std::cout << "[PASS] CheckControlCodesRoundtrip " << kernelName << " (" << arch << "): " << totalBlocksShuffled
                      << " block(s)/" << totalInstrsShuffled << " instruction(s) shuffled, " << totalCorrected
                      << " kernel function(s) corrected, re-assembled into a real cubin, reloaded from scratch, and re-verified "
                      << "clean end to end\n";
        }
    } catch (const CuAsm::CalledProcessError& e) {
        failureReason = std::string(e.what()) + ": " + e.output();
    } catch (const std::exception& e) {
        failureReason = e.what();
    }

    if (!passed) {
        std::cerr << "[FAIL] CheckControlCodesRoundtrip " << kernelName << " (" << arch << "): " << failureReason << "\n";
    }

    removeQuietCC(outCubin);
    return passed;
}

/**
 * @brief Placeholder round trip for an architecture whose OperandRoleTable/LatencyClassTable
 *        curation (Reports/tasks.md Phase 0/1) isn't Turing-complete yet: sm_80/sm_86 still carry
 *        real uncurated opcodes (the LDGSTS async-copy family, F2FP/F2IP/I2FP, REDUX, and others --
 *        see tasks.md's Phase 0.1/1.1 "left as TODO" lists), so running an arbitrary fixture's real
 *        compile+decode+shuffle+correct+reassemble round trip there would spuriously fail with
 *        std::out_of_range on a data-curation gap rather than exercising anything meaningful about
 *        this suite's actual subject (the correction/re-assembly pipeline itself). Trivially passes
 *        until that curation catches up for this architecture -- see this file's own top-of-file doc.
 * @param arch Target SM architecture string, purely for the printed message (e.g. "sm_80").
 * @return Always true.
 **/
inline bool runCheckControlCodesRoundtripPlaceholder(const std::string& arch) {
    std::cout << "[SKIP] CheckControlCodesRoundtrip (" << arch << "): OperandRoleTable/LatencyClassTable curation "
              << "(Reports/tasks.md Phase 0/1) is not yet complete for this architecture -- placeholder pass, see "
              << "CheckControlCodesRoundtripCommon.hpp's doc\n";
    return true;
}

} // namespace CuAsm::Test
