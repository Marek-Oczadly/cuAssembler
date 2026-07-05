#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "CuInsAssemblerRepos.hpp"
#include "CuNVInfo.hpp"
#include "CuSMVersion.hpp"

namespace CuAsm {

/**
 * @brief Assembles a single kernel's instruction stream into its final .text section bytes,
 *        mirroring CuAsm.CuKernelAssembler.CuKernelAssembler. Only the subset of behavior needed
 *        by CuAsmParser is declared here.
 **/
class CuKernelAssembler {
public:
    /**
     * @brief Constructs an assembler for one kernel.
     * @param insAsmRepos Instruction-assembler repository to encode instructions with; may be
     *        null, mirroring the python default of ins_asm_repos=None.
     * @param version Target SM architecture.
     **/
    explicit CuKernelAssembler(CuInsAssemblerRepos* insAsmRepos, const CuSMVersion& version);

    /**
     * @brief Pushes one instruction (with its control code) onto the kernel, mirroring
     *        CuKernelAssembler.push.
     * @param addr Byte offset of the instruction within the kernel's text section.
     * @param icodeStr Instruction text (operation + operands), with fixups already substituted.
     * @param ccodeStr Control code text, e.g. "B------:R-:W-:-:S01".
     **/
    void push(std::uint64_t addr, const std::string& icodeStr, const std::string& ccodeStr);

    /**
     * @brief Assembles all pushed instructions into their final byte encoding, mirroring
     *        CuKernelAssembler.genCode.
     * @return The kernel's assembled .text section bytes.
     **/
    std::string genCode();

    /// Auto-detected extra attributes (e.g. offsets of EXIT/S2R.CTAID instructions) gathered
    /// while pushing instructions, keyed by NVInfo attribute name, mirroring m_ExtraInfo.
    std::map<std::string, CuNVInfoValue> m_ExtraInfo;
};

} // namespace CuAsm
