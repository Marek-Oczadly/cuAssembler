#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace CuAsm {

/// Value of a single .nv.info attribute: unset (EIFMT_NVAL), a scalar (EIFMT_BVAL/HVAL), or a
/// list of 32-bit words (EIFMT_SVAL).
using CuNVInfoValue = std::variant<std::monostate, std::uint32_t, std::vector<std::uint32_t>>;

/// A single decoded (attr name, value) entry of CuNVInfo::m_AttrList.
struct CuNVInfoAttr {
    std::string name;
    CuNVInfoValue value;
};

class CuNVInfo {
public:
    /**
     * @brief Decodes the raw bytes of an .nv.info(.*) section into its attribute list.
     * @param bytecodes Raw bytes of the section (e.g. from an .nv.info.<kernel> ELF section).
     * @param arch Arch string (e.g. "sm_75") the section belongs to.
     **/
    explicit CuNVInfo(const std::vector<std::uint8_t>& bytecodes, const std::string& arch = "sm_75");

    /**
     * @brief Serializes the attribute list back into its raw ".nv.info" byte encoding, mirroring
     *        CuNVInfo.serialize.
     * @return The encoded bytes.
     **/
    std::vector<std::uint8_t> serialize() const;

    /**
     * @brief Builds a map from byte offset to a generated label name, for OFFSETS attributes
     *        that cannot be recovered from disassembly text alone, mirroring
     *        CuNVInfo.getOffsetLabelDict.
     * @param secname Kernel/section name used to build the label text.
     * @return Map of offset -> label name.
     **/
    std::map<std::uint64_t, std::string> getOffsetLabelDict(const std::string& secname) const;

    /**
     * @brief Updates attribute values from a kernel's extra-info dict, keeping unrelated
     *        attributes as-is and dropping auto/manual-gen attributes no longer present,
     *        mirroring CuNVInfo.updateNVInfoFromDict. Should only be called for a
     *        ".nv.info.<kernel>" section, not the top-level ".nv.info".
     * @param nvinfoDict Map of attribute name to new value.
     **/
    void updateNVInfoFromDict(const std::map<std::string, CuNVInfoValue>& nvinfoDict);

    /**
     * @brief Updates EIATTR_REGCOUNT entries with per-symbol register counts, mirroring
     *        CuNVInfo.setRegCount. Should only be called for the top-level ".nv.info" section.
     * @param regCountDict Map of symbol index to register count.
     * @return True if all entries were found and updated.
     **/
    bool setRegCount(const std::map<int, int>& regCountDict);

    /**
     * @brief Gets every attribute whose name is not a recognized EIATTR (i.e. was decoded from
     *        an attr_key not in the EIATTR table), mirroring CuNVInfo.getUnknownAttrList.
     * @return The unknown attribute entries, in m_AttrList order.
     **/
    std::vector<CuNVInfoAttr> getUnknownAttrList() const;

    /// Decoded (attr, val) entries, in the order they appear in the section.
    std::vector<CuNVInfoAttr> m_AttrList;

private:
    /// Attributes the kernel assembler can auto-generate, mirroring __mEIATTR_AutoGen (a copy of
    /// CuSMVersion.getNVInfoAttrAutoGenSet() taken at construction time).
    std::set<std::string> m_EIATTRAutoGen;

    /// Attributes the kernel assembler should generate manually, mirroring __mEIATTR_MaunalGen
    /// (a copy of CuSMVersion.getNVInfoAttrManualGenSet() taken at construction time).
    std::set<std::string> m_EIATTRManualGen;
};

} // namespace CuAsm
