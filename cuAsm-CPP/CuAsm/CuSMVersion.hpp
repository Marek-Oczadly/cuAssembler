#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "utils/BigNum.hpp"

namespace CuAsm {

class CuNVInfo;

/**
 * @brief Immutable description of a target SM architecture (e.g. sm_75), mirroring
 *        CuAsm.CuSMVersion.CuSMVersion. Only the subset of behavior needed by CuAsmParser is
 *        declared here; instances are cheap value types (no instance-repository/caching like
 *        the python class's __new__ override).
 **/
class CuSMVersion {
public:
    /**
     * @brief Constructs from a numeric SM version, e.g. 75 for sm_75.
     * @param version Numeric SM version.
     **/
    explicit CuSMVersion(int version);

    /**
     * @brief Constructs from a version string, e.g. "sm_75" or "75".
     * @param version Version string.
     **/
    explicit CuSMVersion(const std::string& version);

    /** @brief Gets the major version digit(s), e.g. 7 for sm_75. @return The major version. */
    int getMajor() const;

    /** @brief Gets the minor version digit, e.g. 5 for sm_75. @return The minor version. */
    int getMinor() const;

    /** @brief Gets the full numeric SM version, e.g. 75 for sm_75. @return The SM version number. */
    int getVersionNumber() const;

    /** @brief Gets the display string, e.g. "SM_75". @return The version string. */
    std::string getVersionString() const;

    /**
     * @brief Gets the fixed instruction length in bytes for this arch (8 for sm_5x/6x, 16 for
     *        sm_7x/8x+), mirroring getInstructionLength.
     * @return The instruction length in bytes.
     **/
    int getInstructionLength() const;

    /**
     * @brief Gets the byte offset of an instruction from its index, mirroring
     *        getInsOffsetFromIndex.
     * @param idx Instruction index (0-based).
     * @return The byte offset within a text section.
     **/
    std::uint64_t getInsOffsetFromIndex(int idx) const;

    /**
     * @brief Gets the padding bytes used to fill unused instruction slots for this arch,
     *        mirroring getPadBytes.
     * @return The padding byte sequence.
     **/
    std::string getPadBytes() const;

    /**
     * @brief Gets the relocation type name for an in-instruction fixup key, mirroring
     *        getInsRelocationType. Available keys: "32@hi", "32@lo", "target".
     * @param key Fixup key.
     * @return The corresponding relocation type name (e.g. "R_CUDA_32").
     **/
    std::string getInsRelocationType(const std::string& key) const;

    /**
     * @brief Updates the EIATTR_REGCOUNT entries of a ".nv.info" CuNVInfo with per-symbol
     *        register counts, mirroring setRegCountInNVInfo.
     * @param nvinfo NVInfo to update.
     * @param regCountDict Map of symbol index to register count.
     * @return True if all entries were found and updated.
     **/
    bool setRegCountInNVInfo(CuNVInfo& nvinfo, const std::map<int, int>& regCountDict) const;

    /**
     * @brief Checks whether cubins for this arch need the desc[UR#] cache-policy hack applied
     *        before disassembly, mirroring CuSMVersion.needsDescHack.
     * @return True for sm_80 and later.
     **/
    bool needsDescHack() const;

    /**
     * @brief Rewrites a disassembled instruction line to replace a QNAN operand with its exact
     *        float bit pattern, mirroring CuSMVersion.hackDisassembly.
     * @param code Raw instruction code containing the QNAN immediate.
     * @param asmLine Disassembled instruction text containing "+QNAN"/"-QNAN".
     * @return The rewritten instruction text.
     **/
    std::string hackDisassembly(std::uint64_t code, const std::string& asmLine) const;

    /**
     * @brief Splits packed instruction bytes into parallel control-code and instruction-code
     *        lists, mirroring CuSMVersion.splitCtrlCodeFromBytes(_5x_6x/_7x_8x).
     * @param codebytes Raw bytes of a .text.* section.
     * @return Pair of (control code list, instruction code list), one entry per instruction.
     **/
    std::pair<std::vector<std::uint32_t>, std::vector<std::uint64_t>>
    splitCtrlCodeFromBytes(const std::string& codebytes) const;

    /**
     * @brief Splits a flat list of raw sm5x/6x instruction-pack ints into parallel control-code
     *        and instruction-code lists, mirroring CuSMVersion.splitCtrlCodeFromIntList_5x_6x.
     *        Every group of 4 ints (1 control-code word + 3 instruction words) becomes 3 (ctrl,
     *        code) pairs; reuse-cache bits are folded into each instruction code above bit 64.
     * @param intList Raw ints, length a multiple of 4.
     * @return Pair of (control code list, instruction code list), 3/4 the length of intList.
     **/
    static std::pair<std::vector<std::uint32_t>, std::vector<std::uint64_t>>
    splitCtrlCodeFromIntList_5x_6x(const std::vector<BigInt>& intList);

    /**
     * @brief Splits a flat list of raw sm7x/8x/9x 128-bit instruction ints into parallel
     *        control-code and instruction-code lists, mirroring
     *        CuSMVersion.splitCtrlCodeFromIntList_7x_8x.
     * @param intList Raw (up to 128-bit) instruction ints, one per instruction.
     * @return Pair of (control code list, instruction code list), same length as intList.
     **/
    static std::pair<std::vector<std::uint32_t>, std::vector<std::uint64_t>>
    splitCtrlCodeFromIntList_7x_8x(const std::vector<BigInt>& intList);

    /**
     * @brief Gets the instruction index for a byte offset within a .text.* section, mirroring
     *        getInsIndexFromOffset.
     * @param offset Byte offset of the instruction.
     * @return The instruction's 0-based index, or -1 if offset addresses a control-code word.
     **/
    int getInsIndexFromOffset(std::uint64_t offset) const;

    /**
     * @brief Formats an instruction code as a fixed-width hex string, mirroring
     *        CuSMVersion.formatCode.
     * @param code Instruction code to format.
     * @return "0x" followed by 22 (sm_5x/6x) or 32 (sm_7x/8x+) zero-padded hex digits.
     **/
    std::string formatCode(const BigInt& code) const;

    /**
     * @brief Compares by numeric SM version.
     * @param other Version to compare against.
     * @return True if the versions are equal.
     **/
    bool operator==(const CuSMVersion& other) const;

    /**
     * @brief Compares by numeric SM version.
     * @param other Version to compare against.
     * @return True if the versions differ.
     **/
    bool operator!=(const CuSMVersion& other) const;

    /**
     * @brief Generates a guard-predicated variant of an instruction that was recorded without one,
     *        mirroring CuSMVersion.genPredCode. Takes/returns the same (addr, code, asmText) shape
     *        as CuAsm::InsInfo, spelled out as a tuple here to avoid CuInsAssembler.hpp's reverse
     *        include of this header.
     * @param addr Address of the unpredicated instruction.
     * @param code Code of the unpredicated instruction.
     * @param asmText Asm text of the unpredicated instruction.
     * @return The predicated variant's (addr, code, asmText), or std::nullopt if asmText already
     *         has a predicate (or is an UNDEF pseudo-instruction).
     **/
    std::optional<std::tuple<std::uint64_t, BigInt, std::string>>
    genPredCode(std::uint64_t addr, const BigInt& code, const std::string& asmText) const;

    /// Some versions do not have a pre-gathered InsAsmRepos, but since the encoding may be almost
    /// identical to another version, its InsAsmRepos may be copied instead, mirroring
    /// CuSMVersion.InsAsmReposAliasDict. Maps a version number to the version number to alias.
    static const std::map<int, int> InsAsmReposAliasDict;

private:
    int m_Version;
    int m_Major;
    int m_Minor;
};

} // namespace CuAsm
