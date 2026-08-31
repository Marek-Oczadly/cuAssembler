#include "CuKernelAssembler.hpp"

#include <algorithm>
#include <optional>

#include "CuAsmLogger.hpp"
#include "CuControlCode.hpp"

namespace CuAsm {

CuKernelAssembler::CuKernelAssembler(CuInsAssemblerRepos* insAsmRepos, const CuSMVersion& version)
    : m_InsAsmRepos(insAsmRepos), m_Arch(version) {
    reset();
}

void CuKernelAssembler::reset() {
    m_CCodeList.clear();
    m_ICodeList.clear();
    m_ExtraInfo.clear();
    m_InsIdx = 0;
    m_CodeBytes.clear();
}

void CuKernelAssembler::push(std::uint64_t addr, const std::string& icodeStr, const std::string& ccodeStr) {
    const std::uint32_t ccode = CuControlCode::encode(ccodeStr);
    const BigInt icode = m_InsAsmRepos->assemble(addr, icodeStr);

    // Generate some attributes for special set of opcodes
    const CuInsParser& insParser = m_InsAsmRepos->getInsParser();
    const auto& callbacks = autoAttrOpcodeCallbacks();
    const auto it = callbacks.find(insParser.m_InsOp);
    if (it != callbacks.end()) {
        // extra info will be updated in the function
        it->second(m_ExtraInfo, addr, insParser);
    }

    ++m_InsIdx;

    m_CCodeList.push_back(ccode);
    m_ICodeList.push_back(icode);
}

std::vector<std::byte> CuKernelAssembler::genCode() {
    m_CodeBytes = m_Arch.mergeCtrlCodes(m_ICodeList, m_CCodeList);
    return m_CodeBytes;
}

const std::vector<std::byte>& CuKernelAssembler::getCodeBytes() const {
    return m_CodeBytes;
}

void CuKernelAssembler::autoAttrExit(std::map<std::string, CuNVInfoValue>& info, std::uint64_t addr,
                                      const CuInsParser& /*insParser*/) {
    static const std::string attr = "EIATTR_EXIT_INSTR_OFFSETS";

    const auto it = info.find(attr);
    if (it == info.end()) {
        info[attr] = std::vector<std::uint32_t>{static_cast<std::uint32_t>(addr)};
    } else {
        std::get<std::vector<std::uint32_t>>(it->second).push_back(static_cast<std::uint32_t>(addr));
    }
}

void CuKernelAssembler::autoAttrS2R(std::map<std::string, CuNVInfoValue>& info, std::uint64_t addr,
                                     const CuInsParser& insParser) {
    if (insParser.m_InsKey != "S2R_R_L") {
        return;
    }

    std::optional<std::string> sreg;
    for (const std::string& modi : insParser.m_InsModifier) {
        if (modi.rfind("2_", 0) == 0) {
            sreg = modi.substr(2);
        }
    }

    if (!sreg) {
        CuAsmLogger::logWarning("Unknown SREG for S2R_R_L!!!");
        return;
    }

    if (sreg->rfind("SR_CTAID", 0) != 0) {
        return;
    }

    static const std::string attr = "EIATTR_S2RCTAID_INSTR_OFFSETS";
    const auto it = info.find(attr);
    if (it == info.end()) {
        info[attr] = std::vector<std::uint32_t>{static_cast<std::uint32_t>(addr)};
    } else {
        std::get<std::vector<std::uint32_t>>(it->second).push_back(static_cast<std::uint32_t>(addr));
    }

    if (*sreg == "SR_CTAID.Z") {
        static const std::string zattr = "EIATTR_CTAIDZ_USED";
        if (!info.count(zattr)) {
            info[zattr] = static_cast<std::uint32_t>(0);
        }
    }
}

void CuKernelAssembler::autoAttrBar(std::map<std::string, CuNVInfoValue>& info, std::uint64_t /*addr*/,
                                     const CuInsParser& insParser) {
    static const std::string attr = "BARNUM";
    const std::int64_t barIdx = insParser.m_InsVals[CuInsParser::OPERAND_VAL_IDX];
    const std::uint32_t candidate = static_cast<std::uint32_t>(barIdx + 1);

    const auto it = info.find(attr);
    if (it != info.end()) {
        std::uint32_t& val = std::get<std::uint32_t>(it->second);
        val = std::max(candidate, val);
    } else {
        info[attr] = candidate;
    }
}

void CuKernelAssembler::autoAttrShfl(std::map<std::string, CuNVInfoValue>& /*info*/, std::uint64_t /*addr*/,
                                      const CuInsParser& /*insParser*/) {
    // EIATTR_INT_WARP_WIDE_INSTR_OFFSETS
}

void CuKernelAssembler::autoAttrVote(std::map<std::string, CuNVInfoValue>& /*info*/, std::uint64_t /*addr*/,
                                      const CuInsParser& /*insParser*/) {
    // EIATTR_INT_WARP_WIDE_INSTR_OFFSETS
}

void CuKernelAssembler::autoAttrMma(std::map<std::string, CuNVInfoValue>& info, std::uint64_t /*addr*/,
                                     const CuInsParser& /*insParser*/) {
    static const std::string attr = "EIATTR_WMMA_USED";
    if (!info.count(attr)) {
        info[attr] = static_cast<std::uint32_t>(0);
    }
}

const std::map<std::string, CuKernelAssembler::AutoAttrCallback>& CuKernelAssembler::autoAttrOpcodeCallbacks() {
    static const std::map<std::string, AutoAttrCallback> callbacks = {
        {"EXIT", &CuKernelAssembler::autoAttrExit},
        {"S2R", &CuKernelAssembler::autoAttrS2R},
        {"BAR", &CuKernelAssembler::autoAttrBar},
        {"SHFL", &CuKernelAssembler::autoAttrShfl},
        {"VOTE", &CuKernelAssembler::autoAttrVote},
        {"DMMA", &CuKernelAssembler::autoAttrMma},
        {"HMMA", &CuKernelAssembler::autoAttrMma},
        {"IMMA", &CuKernelAssembler::autoAttrMma},
        {"BMMA", &CuKernelAssembler::autoAttrMma},
    };
    return callbacks;
}

} // namespace CuAsm
