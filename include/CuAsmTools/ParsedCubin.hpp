#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../CuAsm/CuAsmParser.hpp"
#include "../../CuAsm/CuControlCode.hpp"
#include "../../CuAsm/CubinFile.hpp"
#include "../../CuAsm/common.hpp"
#include "CorrectCC.hpp"
#include "VerifyCC.hpp"

// Clean C++ entry point for editing a cubin's SASS instruction streams and re-encoding it,
// building on the existing cubin<->cuasm round trip (CubinFile's disassembly, CuAsmParser's
// assembly) that bin/cuasm.cpp/CuAsmTools/Cuasm.hpp already wrap. Unlike Cuasm.hpp's
// disassembleCubin()/assembleCuasm() (which hand the caller a whole .cuasm text file to read or
// write), this exposes only what a SASS-instruction-shuffling caller actually wants to touch --
// each kernel's instruction text, as an ordinary movable/reorderable std::vector<SassLine> -- and
// keeps every other cuasm/cubin detail (ELF file/section/segment headers, symbol/relocation
// tables, .nv.info, and per-instruction control codes) private, so it can't be desynchronized by
// editing.
//
// Design: parseCubin() disassembles the cubin exactly as CubinFile::saveAsCuAsm() would, but
// instead of writing that text out, keeps it in memory as ParsedCubin::m_SkeletonLines -- a
// template that ParsedCubin::saveAsCubin() reproduces verbatim except for each kernel's
// instruction range, which it regenerates from that kernel's (possibly edited/reordered)
// SassLine list. This means saveAsCubin() never needs to understand (or risk getting wrong) any
// of the surrounding ELF/table/.nv.info structure -- it only ever has to emit valid
// "[ctrlcode] instruction ;" text, exactly like the disassembly it came from, which
// CuAsmParser::parse() already knows how to consume.
//
// Each SassLine also carries (hidden) any label lines -- branch targets, or the
// ".CUASM_OFFSET_LABEL...:" markers nv.info's warp-wide-instruction-offset attributes are
// recovered from -- that immediately preceded it in the original disassembly, and re-emits them
// immediately before itself on save. Reordering a kernel's instructions therefore keeps any
// label that pointed at one of them attached to it (both stay in sync with the move), but doing
// so will generally still change what jumps to that label actually branch to, and moving a
// warp-wide instruction across an EIATTR_INT_WARP_WIDE_INSTR_OFFSETS label reassigns which
// attribute reports its (new) address -- this hides the *bookkeeping*, not the *semantics*, of
// reordering code with control-flow/warp-wide-attribute dependencies. Only per-kernel
// instruction edits are honored by saveAsCubin(): removing a kernel from ParsedCubin::m_Kernels
// (rather than clearing its SassLine list) leaves that kernel's original code in the assembled
// output, since its instruction range in m_SkeletonLines is only ever touched by substitution,
// never by deletion.
//
// Each SassLine's control code is carried forward from wherever it was disassembled from (or
// defaulted, for a freshly-inserted line) verbatim -- editing/reordering instructions can leave
// those stale (a moved instruction's original wait/set-scoreboard slots may no longer close the
// hazards its new neighbors now create). saveAsCubin() closes that gap itself: after assembling,
// it runs the existing hazard-based verify-cc/correct-cc machinery (VerifyCC.hpp/CorrectCC.hpp,
// wrapping ccCommon.hpp's verifyControlCodes()/correctControlCodes()) against the result, and if
// any kernel's control codes are Violated, replaces them with correctControlCodes()'s repaired
// ones before returning. This only covers Turing (sm_75) and newer, matching that machinery's own
// c_MinSupportedSMVersion -- an older-arch cubin's control codes are used exactly as carried
// forward/defaulted, unverified. It also pulls in ELFIO + Boost::graph transitively through
// ccCommon.hpp, which parseCubin()/SassLine/SassKernel alone would not need.

namespace CuAsm::Tools {

class ParsedCubin;
ParsedCubin parseCubin(const std::string& cubinPath);

/**
 * @brief One SASS instruction belonging to a parsed cubin's kernel, mirroring one
 *        "[ctrlcode] instruction ;" line of CubinFile's disassembly.
 *
 * SassLine is an ordinary copyable/movable value type -- a kernel's instruction list
 * (std::vector<SassLine>) can be freely reordered, spliced, inserted into, or erased from with
 * plain vector operations. Its control code and any labels that pointed at it travel invisibly
 * along with it (there is no accessor for either), so nothing manipulating the instruction
 * stream can desync them from the instruction they belong to.
 **/
class SassLine {
public:
    /**
     * @brief Constructs an instruction with a conservative default control code
     *        ("B------:R-:W-:-:S01": waits on/sets no scoreboard, one-cycle stall), for callers
     *        inserting brand-new instructions into a kernel.
     * @param text Instruction text (opcode + operands + trailing ';'), e.g. "IADD3 R0, R1, R2, RZ ;".
     **/
    explicit SassLine(std::string text = {}) : m_Text(std::move(text)) {}

    /// Instruction text (opcode + operands + trailing ';'), freely readable/writable.
    std::string m_Text;

private:
    friend ParsedCubin parseCubin(const std::string&);
    friend class ParsedCubin;

    /// Raw control code (waitbar/readbar/writebar/yield/stall); see CuControlCode. Hidden so
    /// editing/reordering SassLine::m_Text can never desync it from its instruction.
    std::uint32_t m_ControlCode = CuControlCode::mergeCode(0, 0, 0, 0, 1);
    /// Label lines (branch targets, ".CUASM_OFFSET_LABEL...:" markers) that immediately preceded
    /// this instruction in the disassembly; re-emitted immediately before it on save.
    std::vector<std::string> m_LeadingLabels;
};

/**
 * @brief One kernel (`.text.<name>` section) within a parsed cubin.
 **/
class SassKernel {
public:
    /// Kernel (function) name.
    std::string m_Name;
    /// The kernel's instructions, in execution order. Publicly manipulatable/movable: reorder,
    /// insert, erase, or edit SassLine::m_Text freely.
    std::vector<SassLine> m_Lines;

private:
    friend ParsedCubin parseCubin(const std::string&);
    friend class ParsedCubin;

    /// [start, end) line range within ParsedCubin::m_SkeletonLines that this kernel's original
    /// instructions occupied; saveAsCubin() replaces exactly this range with m_Lines' current
    /// (re-encoded) content. Hidden so it can't be invalidated by code outside this file.
    std::pair<int, int> m_CodeRegion{0, 0};
};

/**
 * @brief A cubin disassembled into a form that exposes only its SASS instructions for editing,
 *        keeping everything else (ELF file/section/segment headers, symbol/relocation tables,
 *        .nv.info, control codes) private so saveAsCubin() can re-encode it correctly. Construct
 *        via parseCubin().
 **/
class ParsedCubin {
public:
    /// This cubin's kernels, in on-disk order. See SassKernel::m_Lines for what editing them
    /// does and does not affect.
    std::vector<SassKernel> m_Kernels;

    /**
     * @brief Re-encodes the current (possibly edited/reordered) instruction streams, validates
     *        the result's control codes, transparently repairing them if needed, and writes the
     *        result out as a cubin.
     * @param outCubinPath Output .cubin path; overwritten if it already exists.
     * @throws std::exception on any IO failure, or any parse/assembly failure from the
     *         regenerated cuasm text (e.g. an edited instruction CuInsParser/CuInsAssembler
     *         can't encode, or a branch/label reference left dangling by a reorder) -- see
     *         CuAsmParser::parse() / saveAsCubin().
     * @throws std::runtime_error if the assembled cubin targets Turing (sm_75) or newer and a
     *         kernel's control codes are left invalid by an edit/reorder in a way
     *         correctControlCodes() cannot repair in place (CheckStatus::Unrepairable) -- an
     *         instruction reorder with no valid assignment of variable-latency producers to the
     *         6 physical scoreboard slots. outCubinPath is not written in that case.
     **/
    void saveAsCubin(const std::string& outCubinPath) const;

private:
    friend ParsedCubin parseCubin(const std::string&);

    /// The full disassembly (CubinFile::saveAsCuAsm() output), one entry per line, kept as a
    /// template: every line outside a kernel's m_CodeRegion is reproduced verbatim by
    /// saveAsCubin(), so ELF headers/segments/symbol tables/.nv.info/etc. never need to be
    /// understood or reconstructed here.
    std::vector<std::string> m_SkeletonLines;
};

inline void ParsedCubin::saveAsCubin(const std::string& outCubinPath) const {
    std::map<int, const SassKernel*> regionsByStart;
    for (const SassKernel& kernel : m_Kernels) {
        regionsByStart.emplace(kernel.m_CodeRegion.first, &kernel);
    }

    std::vector<std::string> outLines;
    outLines.reserve(m_SkeletonLines.size());

    for (std::size_t i = 0; i < m_SkeletonLines.size();) {
        auto it = regionsByStart.find(static_cast<int>(i));
        if (it == regionsByStart.end()) {
            outLines.push_back(m_SkeletonLines[i]);
            ++i;
            continue;
        }

        const SassKernel& kernel = *it->second;
        for (const SassLine& line : kernel.m_Lines) {
            for (const std::string& label : line.m_LeadingLabels) {
                outLines.push_back("  " + label + ":");
            }
            outLines.push_back("      [" + CuControlCode::decode(line.m_ControlCode) + "]\t" + line.m_Text);
        }

        const int regionEnd = kernel.m_CodeRegion.second;
        regionsByStart.erase(it);
        // A kernel with no original instructions has a zero-width region (start == end): its
        // generated lines (if any were added) are inserted here without consuming line i, which
        // is then reproduced normally as the next iteration's non-region line.
        if (regionEnd > static_cast<int>(i)) {
            i = static_cast<std::size_t>(regionEnd);
        }
    }

    const std::string tmpAsmPath = getTempFileName("", "cuasm", "cuasm");
    {
        std::ofstream fout(tmpAsmPath, std::ios::binary);
        if (!fout) {
            throw std::runtime_error("ParsedCubin::saveAsCubin: failed to open temp file \"" + tmpAsmPath + "\"");
        }
        for (const std::string& line : outLines) {
            fout << line << '\n';
        }
    }

    const std::string tmpCubinPath = getTempFileName("", "cuasm", "cubin");
    try {
        CuAsmParser parser;
        parser.parse(tmpAsmPath);
        parser.saveAsCubin(tmpCubinPath);
    } catch (...) {
        std::filesystem::remove(tmpAsmPath);
        std::filesystem::remove(tmpCubinPath);
        throw;
    }
    std::filesystem::remove(tmpAsmPath);

    try {
        ELFIO::elfio ef;
        const bool supportedArch = ef.load(tmpCubinPath) && detectArch(ef).has_value();

        if (!supportedArch) {
            // Pre-Turing (< sm_75): verifyCubinControlCodes()/correctCubinControlCodes() don't
            // support it, so the control codes carried forward/defaulted by each SassLine are
            // used exactly as they are, unverified.
            std::filesystem::copy_file(tmpCubinPath, outCubinPath, std::filesystem::copy_options::overwrite_existing);
        } else {
            const CubinVerificationReport vr = verifyCubinControlCodes(tmpCubinPath);
            const bool anyViolated = std::any_of(vr.kernels.begin(), vr.kernels.end(), [](const KernelVerificationReport& k) {
                return k.result.status == CheckStatus::Violated;
            });

            if (!anyViolated) {
                std::filesystem::copy_file(tmpCubinPath, outCubinPath, std::filesystem::copy_options::overwrite_existing);
            } else {
                // correctCubinControlCodes() re-derives and writes every kernel's control codes
                // (not just the Violated ones) -- recomputing an already-Verified kernel's is
                // harmless (still correct, just possibly different stall/wait choices), and it
                // only ever writes outCubinPath itself when every kernel came back repairable.
                const CubinCorrectionReport cr = correctCubinControlCodes(tmpCubinPath, outCubinPath);
                if (cr.anyUnrepairable) {
                    throw std::runtime_error(
                        "ParsedCubin::saveAsCubin: control codes left invalid by this edit could not be "
                        "automatically repaired (an instruction reorder left a kernel with no valid "
                        "scoreboard-slot assignment) -- see CuAsm::Tools::correctCubinControlCodes() for a "
                        "per-kernel report");
                }
            }
        }
    } catch (...) {
        std::filesystem::remove(tmpCubinPath);
        throw;
    }
    std::filesystem::remove(tmpCubinPath);
}

/**
 * @brief Parses a cubin file into a ParsedCubin, disassembling it exactly as CubinFile does but
 *        exposing only its SASS instruction text (per kernel) for editing.
 * @param cubinPath Path to the input cubin file.
 * @return The parsed cubin.
 * @throws std::exception on any parse/IO failure (see CubinFile's constructor), or
 *         std::runtime_error if a kernel's instruction range contains a line this couldn't
 *         classify as a label, directive, or "[ctrlcode] instruction" line.
 **/
inline ParsedCubin parseCubin(const std::string& cubinPath) {
    const std::string tmpAsmPath = getTempFileName("", "cuasm", "cuasm");
    CubinFile cf(cubinPath);
    cf.saveAsCuAsm(tmpAsmPath);

    std::vector<std::string> lines;
    {
        std::ifstream fin(tmpAsmPath, std::ios::binary);
        if (!fin) {
            std::filesystem::remove(tmpAsmPath);
            throw std::runtime_error("parseCubin: failed to open temp file \"" + tmpAsmPath + "\"");
        }
        std::string line;
        while (std::getline(fin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(std::move(line));
        }
    }
    std::filesystem::remove(tmpAsmPath);

    // Mirrors CuAsmParser::Impl's own pTextLine/mLabelPattern/mDirectivePattern (private there),
    // and matchAtStart's python-re.match-style "anchored at position 0" semantics.
    static const std::regex codeLinePattern(R"(\[([\w:-]+)\](.*))");
    static const std::regex labelPattern(R"(([a-zA-Z0-9._$@#]+?)\s*:\s*(.*))");
    static const std::regex directivePattern(R"((\.[a-zA-Z0-9_]+)\s*(.*))");
    auto matchAtStart = [](const std::string& s, const std::regex& re, std::smatch& m) {
        return std::regex_search(s, m, re, std::regex_constants::match_continuous);
    };

    ParsedCubin result;
    result.m_SkeletonLines = lines;

    const std::map<std::string, std::pair<int, int>> sectionMarkers = splitAsmSection(lines);
    std::map<int, std::string> textSectionsByStart;
    for (const auto& [secname, range] : sectionMarkers) {
        if (secname.starts_with(".text.")) {
            textSectionsByStart.emplace(range.first, secname);
        }
    }

    for (const auto& [rangeStart, secname] : textSectionsByStart) {
        const auto& range = sectionMarkers.at(secname);

        SassKernel kernel;
        kernel.m_Name = secname.substr(6);

        // First pass: locate the [first, last] instruction lines, so labels/directives before
        // the first (kernel entry point, .global/.type/.size/.other/...) and after the last
        // (e.g. the size-computation end label) can be left untouched in the skeleton.
        int firstInstr = -1;
        int lastInstr = -1;
        for (int i = range.first; i < range.second; ++i) {
            std::string nline = trim(CuAsmParser::stripComments(lines[i]));
            if (nline.empty()) {
                continue;
            }
            std::smatch m;
            if (matchAtStart(nline, labelPattern, m) || matchAtStart(nline, directivePattern, m)) {
                continue;
            }
            if (matchAtStart(nline, codeLinePattern, m) && matchesControlCodePattern(m[1].str())) {
                if (firstInstr < 0) {
                    firstInstr = i;
                }
                lastInstr = i;
            }
        }

        const int codeStart = (firstInstr >= 0) ? firstInstr : range.second;
        const int codeEnd = (lastInstr >= 0) ? lastInstr + 1 : range.second;

        // Second pass: build the SassLine list, attaching interspersed labels to the
        // instruction they precede.
        std::vector<std::string> pendingLabels;
        for (int i = codeStart; i < codeEnd; ++i) {
            std::string nline = trim(CuAsmParser::stripComments(lines[i]));
            if (nline.empty()) {
                continue;
            }
            std::smatch m;
            if (matchAtStart(nline, labelPattern, m)) {
                pendingLabels.push_back(m[1].str());
                continue;
            }
            if (matchAtStart(nline, directivePattern, m)) {
                continue;
            }
            if (!matchAtStart(nline, codeLinePattern, m)) {
                throw std::runtime_error("parseCubin: unrecognized line in kernel \"" + kernel.m_Name + "\": \"" + nline + "\"");
            }

            SassLine sline(trim(m[2].str()));
            sline.m_ControlCode = CuControlCode::encode(m[1].str());
            sline.m_LeadingLabels = std::move(pendingLabels);
            pendingLabels.clear();
            kernel.m_Lines.push_back(std::move(sline));
        }

        kernel.m_CodeRegion = {codeStart, codeEnd};
        result.m_Kernels.push_back(std::move(kernel));
    }

    return result;
}

} // namespace CuAsm::Tools
