#pragma once

#include <cstdint>
#include <map>
#include <string>

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

private:
    int m_Version;
    int m_Major;
    int m_Minor;
};

} // namespace CuAsm
