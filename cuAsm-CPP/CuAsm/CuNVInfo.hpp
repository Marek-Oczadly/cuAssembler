#pragma once

#include <cstdint>
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

    /// Decoded (attr, val) entries, in the order they appear in the section.
    std::vector<CuNVInfoAttr> m_AttrList;
};

} // namespace CuAsm
