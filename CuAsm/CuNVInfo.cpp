#include "CuNVInfo.hpp"

#include <format>
#include <iostream>
#include <stdexcept>

#include "CuSMVersion.hpp"

namespace CuAsm {

namespace {

const std::map<std::uint16_t, std::string>& eiattrTable() {
    static const std::map<std::uint16_t, std::string> table = {
        {0x0401, "EIATTR_CTAIDZ_USED"},
        {0x0504, "EIATTR_MAX_THREADS"},
        {0x0a04, "EIATTR_PARAM_CBANK"},
        {0x0f04, "EIATTR_EXTERNS"},
        {0x1004, "EIATTR_REQNTID"},
        {0x1104, "EIATTR_FRAME_SIZE"},
        {0x1204, "EIATTR_MIN_STACK_SIZE"},
        {0x1502, "EIATTR_BINDLESS_TEXTURE_BANK"},
        {0x1602, "EIATTR_BINDLESS_SURFACE_BANK"},
        {0x1704, "EIATTR_KPARAM_INFO"},
        {0x1903, "EIATTR_CBANK_PARAM_SIZE"},
        {0x1b03, "EIATTR_MAXREG_COUNT"},
        {0x1c04, "EIATTR_EXIT_INSTR_OFFSETS"},
        {0x1d04, "EIATTR_S2RCTAID_INSTR_OFFSETS"},
        {0x1e04, "EIATTR_CRS_STACK_SIZE"},
        {0x1f01, "EIATTR_NEED_CNP_WRAPPER"},
        {0x2001, "EIATTR_NEED_CNP_PATCH"},
        {0x2101, "EIATTR_EXPLICIT_CACHING"},
        {0x2304, "EIATTR_MAX_STACK_SIZE"},
        {0x2504, "EIATTR_LD_CACHEMOD_INSTR_OFFSETS"},
        {0x2704, "EIATTR_ATOM_SYS_INSTR_OFFSETS"},
        {0x2804, "EIATTR_COOP_GROUP_INSTR_OFFSETS"},
        {0x2a01, "EIATTR_SW1850030_WAR"},
        {0x2b01, "EIATTR_WMMA_USED"},
        {0x2e04, "EIATTR_ATOM16_EMUL_INSTR_REG_MAP"},
        {0x2f04, "EIATTR_REGCOUNT"},
        {0x3001, "EIATTR_SW2393858_WAR"},
        {0x3104, "EIATTR_INT_WARP_WIDE_INSTR_OFFSETS"},
        {0x3404, "EIATTR_INDIRECT_BRANCH_TARGETS"},
        {0x3501, "EIATTR_SW2861232_WAR"},
        {0x3604, "EIATTR_SW_WAR"},
        {0x3704, "EIATTR_CUDA_API_VERSION"},
    };
    return table;
}

// EIATTRVAL is just the reciprocal mapping of EIATTR.
const std::map<std::string, std::uint16_t>& eiattrValTable() {
    static const std::map<std::string, std::uint16_t> table = [] {
        std::map<std::string, std::uint16_t> t;
        for (const auto& [key, name] : eiattrTable()) {
            t[name] = key;
        }
        return t;
    }();
    return table;
}

std::uint16_t getAttrKey(const std::string& attrName) {
    const auto& table = eiattrValTable();
    const auto it = table.find(attrName);
    if (it != table.end()) {
        return it->second;
    }

    // EIATTR_UNKNOWN_0x????
    const std::string hexPart = attrName.substr(attrName.size() - 6);
    return static_cast<std::uint16_t>(std::stoul(hexPart, nullptr, 16));
}

std::string getAttrName(std::uint16_t attrKey) {
    const auto& table = eiattrTable();
    const auto it = table.find(attrKey);
    if (it != table.end()) {
        return it->second;
    }

    return std::format("EIATTR_UNKNOWN_0x{:04x}", attrKey);
}

std::vector<CuNVInfoAttr> decodeAttrList(const std::vector<std::uint8_t>& bytecodes) {
    std::vector<CuNVInfoAttr> attrList;

    std::size_t pos = 0;
    const std::size_t n = bytecodes.size();

    const auto readLE = [&](int nbytes) -> std::uint32_t {
        std::uint32_t val = 0;
        for (int i = 0; i < nbytes; ++i) {
            val |= static_cast<std::uint32_t>(bytecodes.at(pos + i)) << (8 * i);
        }
        pos += static_cast<std::size_t>(nbytes);
        return val;
    };

    while (pos < n) {
        const std::uint8_t vfmt = bytecodes.at(pos++);
        const std::uint8_t attrType = bytecodes.at(pos++);
        const std::uint16_t attrKey = static_cast<std::uint16_t>((attrType << 8) + vfmt);

        CuNVInfoValue val;
        if (vfmt == 1) { // EIFMT_NVAL
            pos += 2; // usually zeros, thus skipped
        } else if (vfmt == 2 || vfmt == 3) { // EIFMT_BVAL / EIFMT_HVAL
            val = readLE(2);
        } else if (vfmt == 4) { // EIFMT_SVAL, length should be multiple of 4B
            const std::uint32_t nelem = readLE(2) >> 2;
            std::vector<std::uint32_t> elems;
            elems.reserve(nelem);
            for (std::uint32_t i = 0; i < nelem; ++i) {
                elems.push_back(readLE(4));
            }
            val = std::move(elems);
        } else {
            throw std::runtime_error("Unknown EIFMT(" + std::to_string(vfmt) + ") in nv.info!");
        }

        const std::string attrName = getAttrName(attrKey);
        if (!eiattrTable().count(attrKey)) {
            std::cout << std::format("WARNING!!! Unknown EIATTR 0x{:04x}! Some offsets may not work properly!", attrKey) << std::endl;
        }

        attrList.push_back({attrName, val});
    }

    return attrList;
}

std::vector<std::uint8_t> packValue(std::uint8_t vfmt, const CuNVInfoValue& val) {
    std::vector<std::uint8_t> bval;

    if (vfmt == 1) { // EIFMT_NVAL
        bval = {0x00, 0x00};
    } else if (vfmt == 2 || vfmt == 3) { // EIFMT_BVAL / EIFMT_HVAL
        const std::uint32_t v = std::get<std::uint32_t>(val);
        bval = {static_cast<std::uint8_t>(v & 0xff), static_cast<std::uint8_t>((v >> 8) & 0xff)};
    } else if (vfmt == 4) { // EIFMT_SVAL
        const std::vector<std::uint32_t>& elems = std::get<std::vector<std::uint32_t>>(val);
        const std::uint32_t length = static_cast<std::uint32_t>(elems.size() * 4);
        bval.push_back(static_cast<std::uint8_t>(length & 0xff));
        bval.push_back(static_cast<std::uint8_t>((length >> 8) & 0xff));
        for (std::uint32_t v : elems) {
            bval.push_back(static_cast<std::uint8_t>(v & 0xff));
            bval.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
            bval.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
            bval.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
        }
    } else {
        throw std::runtime_error("Unknown EIFMT(" + std::to_string(vfmt) + ") in nv.info!");
    }

    return bval;
}

std::vector<std::uint8_t> encodeAttrList(const std::vector<CuNVInfoAttr>& attrList) {
    std::vector<std::uint8_t> out;

    for (const CuNVInfoAttr& attr : attrList) {
        const std::uint16_t attrKey = getAttrKey(attr.name);
        const std::uint8_t vfmt = static_cast<std::uint8_t>(attrKey & 0xff);

        out.push_back(static_cast<std::uint8_t>(attrKey & 0xff));
        out.push_back(static_cast<std::uint8_t>((attrKey >> 8) & 0xff));

        const std::vector<std::uint8_t> bval = packValue(vfmt, attr.value);
        out.insert(out.end(), bval.begin(), bval.end());
    }

    return out;
}

} // namespace

CuNVInfo::CuNVInfo(const std::vector<std::uint8_t>& bytecodes, const std::string& arch)
    : m_AttrList(decodeAttrList(bytecodes)),
      m_EIATTRAutoGen(CuSMVersion(arch).getNVInfoAttrAutoGenSet()),
      m_EIATTRManualGen(CuSMVersion(arch).getNVInfoAttrManualGenSet()) {
}

std::vector<std::uint8_t> CuNVInfo::serialize() const {
    return encodeAttrList(m_AttrList);
}

std::map<std::uint64_t, std::string> CuNVInfo::getOffsetLabelDict(const std::string& secname) const {
    std::map<std::uint64_t, std::string> labelDict;

    for (const CuNVInfoAttr& attr : m_AttrList) {
        if (attr.name.ends_with("OFFSETS") && !m_EIATTRAutoGen.count(attr.name)) {
            // CHECK: Could there be two nvinfo attributes with same offset?
            // TODO: Some OFFSETS attributes seem have special structure?
            //       May add some assembler directive in kernel to handle them?
            const auto* offsets = std::get_if<std::vector<std::uint32_t>>(&attr.value);
            if (!offsets) {
                continue;
            }
            for (std::uint32_t offset : *offsets) {
                labelDict[offset] = ".CUASM_OFFSET_LABEL." + secname + "." + attr.name + ".#";
            }
        }
    }

    return labelDict;
}

void CuNVInfo::updateNVInfoFromDict(const std::map<std::string, CuNVInfoValue>& nvinfoDict) {
    std::map<std::string, CuNVInfoValue> d = nvinfoDict;

    std::vector<CuNVInfoAttr> newAttrList;
    for (const CuNVInfoAttr& attr : m_AttrList) {
        if (!m_EIATTRAutoGen.count(attr.name) && !m_EIATTRManualGen.count(attr.name)) { // kept as is
            newAttrList.push_back(attr);
        } else if (const auto it = d.find(attr.name); it != d.end()) {
            // autogen/manual gen attributes, updated and remove from appended list
            newAttrList.push_back(attr);
            d.erase(it);
        }
        // else: attr in AutoGen but not in d, which means it's deleted
    }

    // Append new autogen attributes not previously defined
    for (const auto& [k, v] : d) {
        // Some item in d may be not valid attributes, skipped
        if (m_EIATTRAutoGen.count(k) || m_EIATTRManualGen.count(k)) {
            newAttrList.push_back({k, v});
        }
    }

    m_AttrList = std::move(newAttrList);
}

bool CuNVInfo::setRegCount(const std::map<int, int>& regCountDict) {
    bool flag = true;

    for (CuNVInfoAttr& attr : m_AttrList) {
        if (attr.name != "EIATTR_REGCOUNT") {
            continue;
        }

        auto* val = std::get_if<std::vector<std::uint32_t>>(&attr.value);
        if (!val || val->empty()) {
            continue;
        }

        const int symIdx = static_cast<int>((*val)[0]);
        const auto it = regCountDict.find(symIdx);
        if (it != regCountDict.end()) {
            *val = {static_cast<std::uint32_t>(symIdx), static_cast<std::uint32_t>(it->second)};
        } else {
            flag = false;
        }
    }

    return flag;
}

std::vector<CuNVInfoAttr> CuNVInfo::getUnknownAttrList() const {
    std::vector<CuNVInfoAttr> result;

    for (const CuNVInfoAttr& attr : m_AttrList) {
        if (!eiattrValTable().count(attr.name)) {
            result.push_back(attr);
        }
    }

    return result;
}

} // namespace CuAsm
