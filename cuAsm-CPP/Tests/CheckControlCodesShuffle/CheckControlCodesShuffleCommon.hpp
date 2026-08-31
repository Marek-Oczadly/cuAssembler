#pragma once

#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/graph/adjacency_list.hpp>

#include "../../bin/ccCommon.hpp"
#include "../CheckControlCodes/CheckControlCodesCommon.hpp"

// Reports/tasks.md Phase 5's "shuffling and moving instructions" test coverage: CLAUDE.md's whole
// premise for this project is a SASS instruction *shuffler* that reorders existing instructions
// within a kernel but never inserts/removes any -- correct-cc's actual job is repairing control
// codes for exactly that kind of reorder. CheckControlCodes (the sibling suite) only ever checks a
// kernel's *original*, ptxas-scheduled instruction order; it never actually reorders anything, so
// it never exercises correctControlCodes() at all against real (not hand-built-synthetic) code.
// This suite closes that gap: reuses the same 23 real, nvcc-compiled fixture kernels (same broad
// opcode-family spread CheckControlCodes already gets real-compiler evidence from), physically
// permutes a legal subset of each kernel's instructions into a genuinely different order, and
// checks that correctControlCodes() repairs the (now-stale) control codes into a fresh set that
// independently re-verifies clean.
//
// WHICH INSTRUCTIONS ARE SAFE TO MOVE: physically relocating an instruction to a different address
// slot is only sound if (a) nothing in its own encoding depends on its address (no branch/call/
// convergence-stack instruction -- BRA/JMP/BRX*/JMX*/CALL/CAL/EXIT/RET/BSSY/SSY/PBK/PRET all embed
// or interpret an address-relative/absolute operand, per CuInsParser::specialTreatment()'s
// addrFuncs handling -- excluded by opcode, not just by control-flow shape, since e.g. CALL always
// looks like a plain fallthrough-only instruction from computeControlFlowSuccessors()'s CFG
// perspective on purpose, but a CALL.REL's *encoding* is still address-relative), and (b) nothing
// outside the moved region jumps into the middle of it (computeJumpTargets() below) -- otherwise a
// real entry point could land on a semantically-different instruction than the one originally
// scheduled there. A maximal run of instructions satisfying both is this file's "shuffleable
// block" (findShuffleableBlocks()); within one, any topological order of the *true* dependency
// edges (buildHazardGraph(), already reused unchanged from ccCommon.hpp -- the same graph a real
// shuffler would need to respect to stay semantically equivalent) is a legal reordering, and this
// file picks a random one (deterministically seeded per kernel function, so runs are reproducible)
// via randomTopoOrder()'s randomized Kahn's-algorithm walk.

namespace CuAsm::Test {

/**
 * @brief Whether opcode is one of the control-flow/address-taking instruction families that must
 *        never be physically relocated by shuffleKernel() -- deliberately kept as an independent,
 *        opcode-name-based list (mirroring, but not calling, ccCommon.hpp's
 *        computeControlFlowSuccessors() opcode groups) rather than inferring "safe to move" purely
 *        from that function's successor *shape*: computeControlFlowSuccessors() deliberately
 *        reports CALL/CAL as fallthrough-only (its target is irrelevant to *this kernel's* CFG,
 *        since the callee lives in a different section entirely), which would otherwise make a
 *        CALL indistinguishable from an ordinary instruction by successor shape alone -- but a
 *        `CALL.REL` instruction's own *encoding* is still address-relative, so moving it would
 *        silently corrupt which function it calls. This list is the safety net for exactly that
 *        gap, checked in addition to (not instead of) the successor-shape check in isPlainInstr().
 * @param opcode Instruction's base opcode (insKey up to the first '_').
 * @return True if opcode must stay at its original address.
 **/
inline bool isAddressSensitiveOpcode(const std::string& opcode) {
    static const std::set<std::string> s{"CALL", "CAL",  "BSSY", "SSY", "PBK", "PRET",
                                          "EXIT", "RET",  "BRA",  "JMP", "BRX", "BRXU", "JMX", "JMXU"};
    return s.count(opcode) > 0;
}

/**
 * @brief Finds every instruction index that some *other* instruction can reach via a real jump
 *        (i.e. a computeControlFlowSuccessors() edge that isn't simply "the next slot") -- these
 *        are illegal interior positions for a shuffleable block, since a block only stays
 *        semantically valid if every real entry point lands on its first instruction (see this
 *        file's own top-of-file doc).
 * @param instrs Kernel's decoded instructions, in program order.
 * @return isTarget[i] is true iff some edge (p, i) exists with p+1 != i.
 **/
inline std::vector<bool> computeJumpTargets(const std::vector<CuAsm::Tools::DecodedInstruction>& instrs) {
    const std::vector<std::vector<std::size_t>> succ = CuAsm::Tools::computeControlFlowSuccessors(instrs);
    std::vector<bool> isTarget(instrs.size(), false);
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        for (const std::size_t s : succ[i]) {
            if (s < instrs.size() && s != i + 1) {
                isTarget[s] = true;
            }
        }
    }
    return isTarget;
}

/// One contiguous, safely-reorderable run of instruction indices [lo, hi) -- see this file's
/// top-of-file doc for what makes a run safe.
using ShuffleBlock = std::pair<std::size_t, std::size_t>;

/**
 * @brief Splits a kernel's decoded instructions into maximal shuffleable blocks (see this file's
 *        top-of-file doc): a block is a contiguous run where every instruction is a "plain"
 *        (non-address-sensitive, fallthrough-only) instruction, and no instruction after the first
 *        is a jump target of anything else. A block's own first instruction is deliberately allowed
 *        to be a jump target (a loop header or reconvergence point is still a perfectly valid
 *        shuffle-region entry) -- only interior positions are excluded.
 * @param instrs Kernel's decoded instructions, in program order.
 * @param minBlockSize Minimum instruction count for a block to be worth reporting (a shorter run
 *        has too little independence to usually produce a genuine reorder).
 * @return Every maximal qualifying block, in ascending, non-overlapping index order.
 **/
inline std::vector<ShuffleBlock> findShuffleableBlocks(const std::vector<CuAsm::Tools::DecodedInstruction>& instrs,
                                                        std::size_t minBlockSize = 2) {
    const std::size_t n = instrs.size();
    const std::vector<std::vector<std::size_t>> succ = CuAsm::Tools::computeControlFlowSuccessors(instrs);
    const std::vector<bool> isJumpTarget = computeJumpTargets(instrs);

    std::vector<bool> isPlain(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        const std::string opcode = instrs[i].insKey.substr(0, instrs[i].insKey.find('_'));
        isPlain[i] = !isAddressSensitiveOpcode(opcode) && succ[i].size() == 1 && succ[i][0] == i + 1;
    }

    std::vector<ShuffleBlock> blocks;
    std::size_t i = 0;
    while (i < n) {
        if (!isPlain[i]) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && isPlain[j] && (j == i || !isJumpTarget[j])) {
            ++j;
        }
        if (j - i >= minBlockSize) {
            blocks.emplace_back(i, j);
        }
        i = j;
    }
    return blocks;
}

/**
 * @brief Picks a uniformly-random topological order of one shuffleable block's *local* true
 *        dependency subgraph (buildHazardGraph()'s edges restricted to [lo, hi)) via a randomized
 *        Kahn's-algorithm walk: repeatedly pick a uniformly-random currently-ready (in-degree-0)
 *        instruction, emit it, and decrement its local successors' in-degrees. Every dependency
 *        edge inside the block is still respected -- this is exactly the "any topological order of
 *        the true dependency graph is a semantically legal reorder" property a real instruction
 *        shuffler relies on.
 * @param lo First index of the block (inclusive).
 * @param hi One past the last index of the block (exclusive).
 * @param graph Whole-kernel hazard graph, as returned by buildHazardGraph() on the *original*
 *        (pre-shuffle) instruction list -- must be index-aligned with it.
 * @param rng Seeded RNG driving the random tie-breaking; consumed (advanced) by this call.
 * @return A permutation of [lo, hi): result[k] is the *original* instruction index that should now
 *         occupy slot lo+k.
 **/
inline std::vector<std::size_t> randomTopoOrder(std::size_t lo, std::size_t hi, const CuAsm::Tools::HazardGraph& graph,
                                                 std::mt19937_64& rng) {
    const std::size_t n = hi - lo;
    std::vector<int> indegree(n, 0);
    std::vector<std::vector<std::size_t>> localSucc(n);
    for (std::size_t idx = lo; idx < hi; ++idx) {
        const auto [oi, oend] = boost::out_edges(idx, graph);
        for (auto e = oi; e != oend; ++e) {
            const std::size_t target = boost::target(*e, graph);
            if (target >= lo && target < hi) {
                localSucc[idx - lo].push_back(target - lo);
                ++indegree[target - lo];
            }
        }
    }

    std::vector<std::size_t> ready;
    for (std::size_t k = 0; k < n; ++k) {
        if (indegree[k] == 0) {
            ready.push_back(k);
        }
    }

    std::vector<std::size_t> order;
    order.reserve(n);
    while (!ready.empty()) {
        std::uniform_int_distribution<std::size_t> dist(0, ready.size() - 1);
        const std::size_t pick = dist(rng);
        const std::size_t node = ready[pick];
        ready[pick] = ready.back();
        ready.pop_back();
        order.push_back(node);
        for (const std::size_t s : localSucc[node]) {
            if (--indegree[s] == 0) {
                ready.push_back(s);
            }
        }
    }

    std::vector<std::size_t> globalOrder;
    globalOrder.reserve(n);
    for (const std::size_t localIdx : order) {
        globalOrder.push_back(lo + localIdx);
    }
    return globalOrder;
}

/// Result of shuffleKernel(): a physically-reordered copy of a kernel's instructions/control codes,
/// plus bookkeeping about how much of it actually changed.
struct ShuffleResult {
    CuAsm::Tools::KernelControlCodes kcc;
    std::vector<CuAsm::Tools::DecodedInstruction> decoded;
    std::size_t blocksShuffled = 0;
    std::size_t blocksSkippedIdentity = 0;
    std::size_t instructionsShuffled = 0;
};

/**
 * @brief Simulates what a real instruction shuffler hands to correct-cc: every shuffleable block
 *        (findShuffleableBlocks()) in the kernel is independently permuted into a random-but-
 *        dependency-respecting order (randomTopoOrder()), and both insCodeList (the physical
 *        instruction encodings) and the decoded/role/latency-annotated instructions are physically
 *        relocated to match -- each instruction *keeps the control code it had at its old slot*
 *        (moved along with it, now stale for its new neighbors), exactly like a shuffler that
 *        reorders lines without updating their control-code annotations. A block whose random
 *        topological order happens to equal the identity (no real dependency freedom, or an
 *        unlucky RNG draw) is left alone and counted separately, not miscounted as "shuffled".
 * @param origKcc Kernel's original control-code/instruction-code lists.
 * @param origDecoded Kernel's original decoded instructions, index-aligned with origKcc.
 * @param arch Kernel's architecture (used only to recompute each new slot's address).
 * @param rng Seeded RNG driving every block's random topological order; consumed by this call.
 * @return The reordered kernel, ready for correctControlCodes().
 **/
inline ShuffleResult shuffleKernel(const CuAsm::Tools::KernelControlCodes& origKcc,
                                    const std::vector<CuAsm::Tools::DecodedInstruction>& origDecoded, const CuAsm::CuSMVersion& arch,
                                    std::mt19937_64& rng) {
    const std::size_t n = origDecoded.size();

    ShuffleResult result;
    result.kcc = origKcc;
    result.decoded = origDecoded;
    if (n < 2) {
        return result;
    }

    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) {
        order[i] = i;
    }

    const CuAsm::Tools::HazardGraph graph = CuAsm::Tools::buildHazardGraph(origDecoded);
    const std::vector<ShuffleBlock> blocks = findShuffleableBlocks(origDecoded);

    for (const auto& [lo, hi] : blocks) {
        const std::vector<std::size_t> perm = randomTopoOrder(lo, hi, graph, rng);
        bool isIdentity = true;
        for (std::size_t k = 0; k < perm.size(); ++k) {
            if (perm[k] != lo + k) {
                isIdentity = false;
                break;
            }
        }
        if (isIdentity) {
            ++result.blocksSkippedIdentity;
            continue;
        }
        for (std::size_t k = 0; k < perm.size(); ++k) {
            order[lo + k] = perm[k];
        }
        ++result.blocksShuffled;
        result.instructionsShuffled += (hi - lo);
    }

    if (result.blocksShuffled == 0) {
        return result;
    }

    std::vector<BigInt> newInsCodes(n);
    std::vector<std::uint32_t> newCtrlCodes(n);
    std::vector<CuAsm::Tools::DecodedInstruction> newDecoded = origDecoded;
    for (std::size_t slot = 0; slot < n; ++slot) {
        const std::size_t origIdx = order[slot];
        newInsCodes[slot] = origKcc.insCodeList[origIdx];
        newCtrlCodes[slot] = origKcc.ctrlCodeList[origIdx];
        newDecoded[slot] = origDecoded[origIdx];
        newDecoded[slot].address = arch.getInsOffsetFromIndex(static_cast<int>(slot));
    }

    result.kcc.insCodeList = std::move(newInsCodes);
    result.kcc.ctrlCodeList = std::move(newCtrlCodes);
    result.decoded = std::move(newDecoded);
    return result;
}

/**
 * @brief Runs the CheckControlCodesShuffle round trip for one (kernel, architecture) pair:
 *        compileAndDecodeCC() (shared with CheckControlCodes), then for every kernel function
 *        found, shuffleKernel() followed by correctControlCodes() and an independent re-verify of
 *        the result via verifyControlCodes() -- i.e. this is the first test suite to actually
 *        exercise correctControlCodes() against real, non-hand-built instruction sequences.
 *        CheckStatus::Unrepairable is accepted (not a failure): a shuffle genuinely can push more
 *        than 6 VARIABLE-latency producers into simultaneous flight, and correctly reporting that
 *        as unrepairable-in-place is itself part of what Phase 4 is supposed to do.
 * @param kernelName Name of the kernel/subdirectory under TestData/CheckDisasm; also the base
 *        name of the .cu fixture.
 * @param arch Target SM architecture, e.g. "sm_75".
 * @param minArchSM Minimum numeric SM version the kernel's instructions require -- if arch's
 *        numeric SM version is lower, the round trip is skipped and this automatically passes.
 * @return true if every kernel function's shuffle either had no shuffleable block, was correctly
 *         reported Unrepairable, or was Corrected and independently re-verified clean; false on any
 *         compile/load/decode error or an unexpected status/residual violation.
 **/
inline bool runCheckControlCodesShuffle(const std::string& kernelName, const std::string& arch, int minArchSM = 0) {
    bool passed = false;
    std::string failureReason;

    try {
        const std::optional<CompiledCC> compiled = compileAndDecodeCC(kernelName, arch, minArchSM);
        if (!compiled) {
            std::cout << "[SKIP] CheckControlCodesShuffle " << kernelName << " (" << arch << "): requires sm_" << minArchSM
                      << " or newer\n";
            return true;
        }

        std::size_t blocksShuffled = 0, blocksIdentity = 0, instrsShuffled = 0, corrected = 0, unrepairable = 0;
        for (const CuAsm::Tools::KernelControlCodes& kcc : compiled->kernels) {
            const std::vector<CuAsm::Tools::DecodedInstruction>& decoded = compiled->decodedByKernel.at(kcc.kernelName);

            // Deterministic per-(fixture, kernel-function) seed: reproducible across runs/machines,
            // but varies from one kernel function to the next rather than reusing one fixed order.
            const std::size_t seed = std::hash<std::string>{}(kernelName + "::" + kcc.kernelName);
            std::mt19937_64 rng(seed);

            ShuffleResult shuffled = shuffleKernel(kcc, decoded, compiled->arch, rng);
            blocksIdentity += shuffled.blocksSkippedIdentity;
            if (shuffled.blocksShuffled == 0) {
                continue;
            }
            blocksShuffled += shuffled.blocksShuffled;
            instrsShuffled += shuffled.instructionsShuffled;

            const CuAsm::Tools::ControlCodeCheckResult correctResult =
                CuAsm::Tools::correctControlCodes(shuffled.kcc, shuffled.decoded, compiled->arch);
            if (correctResult.status == CuAsm::Tools::CheckStatus::Unrepairable) {
                ++unrepairable;
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
                std::ostringstream oss;
                oss << "kernel \"" << kcc.kernelName << "\": correctControlCodes() reported Corrected, but an "
                    << "independent re-verify still found " << reVerify.violations.size() << " violation(s)";
                for (const CuAsm::Tools::HazardViolation& v : reVerify.violations) {
                    oss << "\n    instruction " << v.consumerIndex << " has an unresolved " << CuAsm::Tools::toString(v.type)
                        << " hazard from instruction " << v.producerIndex << " on " << CuAsm::Tools::toString(v.regSpace)
                        << v.regNumber << ": " << v.reason;
                }
                throw std::runtime_error(oss.str());
            }
            ++corrected;
        }

        passed = true;
        std::cout << "[PASS] CheckControlCodesShuffle " << kernelName << " (" << arch << "): " << blocksShuffled << " block(s)/"
                  << instrsShuffled << " instruction(s) shuffled across " << compiled->kernels.size() << " kernel(s), " << corrected
                  << " corrected+reverified clean, " << unrepairable << " correctly reported unrepairable, " << blocksIdentity
                  << " block(s) landed on the identity order\n";
    } catch (const CuAsm::CalledProcessError& e) {
        failureReason = std::string(e.what()) + ": " + e.output();
    } catch (const std::exception& e) {
        failureReason = e.what();
    }

    if (!passed) {
        std::cerr << "[FAIL] CheckControlCodesShuffle " << kernelName << " (" << arch << "): " << failureReason << "\n";
    }

    return passed;
}

} // namespace CuAsm::Test
