#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "CuInsAssemblerRepos.hpp"
#include "CuInsParser.hpp"
#include "CuNVInfo.hpp"
#include "CuSMVersion.hpp"
#include "utils/BigNum.hpp"

namespace CuAsm {

/**
 * @brief Assembles a single kernel's instruction stream into its final .text section bytes,
 *        mirroring CuAsm.CuKernelAssembler.CuKernelAssembler.
 *
 * Unlike the python original (whose ins_asm_repos constructor argument may be None, a repos
 * filename string, or an already-built CuInsAssemblerRepos), this only takes a possibly-null,
 * non-owning CuInsAssemblerRepos*, since every caller (CuAsmParser) already owns one.
 **/
class CuKernelAssembler {
public:
    /**
     * @brief Constructs an assembler for one kernel.
     * @param insAsmRepos Instruction-assembler repository to encode instructions with; may be
     *        null, mirroring the python default of ins_asm_repos=None.
     * @param version Target SM architecture.
     **/
    explicit CuKernelAssembler(CuInsAssemblerRepos* insAsmRepos = nullptr,
                               const CuSMVersion& version = CuSMVersion("sm_75"));

    /**
     * @brief Clears all pushed instructions/attributes, restoring the state built by the
     *        constructor, mirroring CuKernelAssembler.reset.
     **/
    void reset();

    /**
     * @brief Pushes in a new instruction (with its control code), mirroring
     *        CuKernelAssembler.push. Fixups (including relocations) must already be filled in.
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
    std::vector<std::byte> genCode();

    /**
     * @brief Gets the bytes produced by the last genCode() call, mirroring
     *        CuKernelAssembler.getCodeBytes.
     * @return The kernel's assembled .text section bytes.
     **/
    const std::vector<std::byte>& getCodeBytes() const;

    /// Auto-detected extra attributes (e.g. offsets of EXIT/S2R.CTAID instructions, barrier
    /// count) gathered while pushing instructions, keyed by NVInfo attribute name, mirroring
    /// m_ExtraInfo.
    std::map<std::string, CuNVInfoValue> m_ExtraInfo;

private:
    /// Signature shared by every __AutoAttr_* callback, mirroring their (info, addr, ins_parser)
    /// python signature.
    using AutoAttrCallback = void (*)(std::map<std::string, CuNVInfoValue>& info,
                                       std::uint64_t addr,
                                       const CuInsParser& insParser);

    /** @brief Records EIATTR_EXIT_INSTR_OFFSETS, mirroring __AutoAttr_EXIT. */
    static void autoAttrExit(std::map<std::string, CuNVInfoValue>& info, std::uint64_t addr,
                              const CuInsParser& insParser);

    /** @brief Records EIATTR_S2RCTAID_INSTR_OFFSETS / EIATTR_CTAIDZ_USED, mirroring __AutoAttr_S2R. */
    static void autoAttrS2R(std::map<std::string, CuNVInfoValue>& info, std::uint64_t addr,
                             const CuInsParser& insParser);

    /** @brief Updates BARNUM (max barrier index + 1 seen so far), mirroring __AutoAttr_BAR. */
    static void autoAttrBar(std::map<std::string, CuNVInfoValue>& info, std::uint64_t addr,
                             const CuInsParser& insParser);

    /** @brief Placeholder for EIATTR_INT_WARP_WIDE_INSTR_OFFSETS, mirroring __AutoAttr_SHFL. */
    static void autoAttrShfl(std::map<std::string, CuNVInfoValue>& info, std::uint64_t addr,
                              const CuInsParser& insParser);

    /** @brief Placeholder for EIATTR_INT_WARP_WIDE_INSTR_OFFSETS, mirroring __AutoAttr_VOTE. */
    static void autoAttrVote(std::map<std::string, CuNVInfoValue>& info, std::uint64_t addr,
                              const CuInsParser& insParser);

    /** @brief Records EIATTR_WMMA_USED, mirroring __AutoAttr_MMA (shared by DMMA/HMMA/IMMA/BMMA). */
    static void autoAttrMma(std::map<std::string, CuNVInfoValue>& info, std::uint64_t addr,
                             const CuInsParser& insParser);

    /**
     * @brief Gets the (opcode -> auto-attribute callback) dispatch table, mirroring
     *        CuKernelAssembler.AutoAttrOpcodeCallback.
     * @return The dispatch table, keyed by plain opcode (CuInsParser::m_InsOp).
     **/
    static const std::map<std::string, AutoAttrCallback>& autoAttrOpcodeCallbacks();

    CuInsAssemblerRepos* m_InsAsmRepos;
    CuSMVersion m_Arch;

    std::vector<std::uint32_t> m_CCodeList;
    std::vector<BigInt> m_ICodeList;

    int m_InsIdx = 0;
    std::vector<std::byte> m_CodeBytes;
};

} // namespace CuAsm
