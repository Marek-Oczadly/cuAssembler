#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <elfio/elfio.hpp>

#include "../CuAsm/CuNVInfo.hpp"
#include "../CuAsm/CuSMVersion.hpp"
#include "utils/TestUtilsCommon.hpp"

using CuAsm::CuNVInfo;
using CuAsm::CuNVInfoAttr;
using CuAsm::CuNVInfoValue;

namespace {

/** @brief Loads one ELF section's raw bytes by exact name from a cubin. Empty if not found. */
std::vector<std::uint8_t> loadSectionBytes(const ELFIO::elfio& ef, const std::string& name) {
    for (const auto& sec : ef.sections) {
        if (sec->get_name() == name) {
            const char* data = sec->get_data();
            return std::vector<std::uint8_t>(data, data + sec->get_size());
        }
    }
    return {};
}

/** @brief Finds the first ".nv.info.<kernel>" section name in a cubin, or "" if none exists. */
std::string findFirstPerKernelNVInfoSection(const ELFIO::elfio& ef) {
    for (const auto& sec : ef.sections) {
        const std::string& n = sec->get_name();
        if (n.starts_with(".nv.info.")) {
            return n;
        }
    }
    return "";
}

} // namespace

/**
 * @brief Exercises CuAsm::CuNVInfo, the .nv.info(.*) ELF section codec: decoding a kernel's
 *        resource-usage/attribute metadata (register counts, offsets needing labels, WMMA/
 *        cooperative-group usage flags, ...) and serializing it back. Runs against real .nv.info
 *        section bytes pulled straight out of the TestData/CuTest fixture cubins via ELFIO -
 *        real nvcc-produced data, but no nvcc/cuobjdump/nvdisasm subprocess needed to get it.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    const std::string cubinPath = std::string(CUASM_TESTDATA_DIR) + "/CuTest/cudatest.6.sm_61.cubin";
    ELFIO::elfio ef;
    if (!ef.load(cubinPath)) {
        std::cerr << "Failed to load " << cubinPath << "\n";
        return 1;
    }

    // ---- top-level ".nv.info": decode, and round-trip serialize() back to identical bytes ----

    const std::vector<std::uint8_t> topBytes = loadSectionBytes(ef, ".nv.info");
    t.check("the fixture cubin has a non-empty top-level .nv.info section to test against", !topBytes.empty());

    const CuNVInfo topInfo(topBytes, "sm_61");
    t.check("decoding a real .nv.info section yields a non-empty attribute list", !topInfo.m_AttrList.empty());

    const std::vector<std::uint8_t> reserialized = topInfo.serialize();
    t.check("serialize() round-trips decoded attributes back to byte-identical .nv.info data",
            reserialized == topBytes);

    // Every decoded attribute name is either a recognized EIATTR_* name or the documented
    // EIATTR_UNKNOWN_0x<hex> fallback form - never empty, never something else.
    bool allNamesWellFormed = true;
    for (const CuNVInfoAttr& attr : topInfo.m_AttrList) {
        if (attr.name.empty()) {
            allNamesWellFormed = false;
        }
    }
    t.check("every decoded attribute has a non-empty name", allNamesWellFormed);

    // getUnknownAttrList(): whatever it returns, every entry's name must be the
    // EIATTR_UNKNOWN_0x<hex> fallback form (never a recognized name misclassified as unknown).
    const std::vector<CuNVInfoAttr> unknown = topInfo.getUnknownAttrList();
    bool allUnknownAreFallbackNamed = true;
    for (const CuNVInfoAttr& attr : unknown) {
        if (!attr.name.starts_with("EIATTR_UNKNOWN_0x")) {
            allUnknownAreFallbackNamed = false;
        }
    }
    t.check("getUnknownAttrList() only ever returns EIATTR_UNKNOWN_0x<hex>-named entries", allUnknownAreFallbackNamed);

    // ---- getOffsetLabelDict: cross-checked against an independent scan of the same attribute list ----

    const std::string perKernelSecName = findFirstPerKernelNVInfoSection(ef);
    t.check("the fixture cubin has at least one per-kernel .nv.info.<kernel> section to test against",
            !perKernelSecName.empty());

    const std::vector<std::uint8_t> kernelBytes = loadSectionBytes(ef, perKernelSecName);
    const CuNVInfo kernelInfo(kernelBytes, "sm_61");
    const std::string label = "some_kernel_section";
    const std::map<std::uint64_t, std::string> offsetLabels = kernelInfo.getOffsetLabelDict(label);

    // Independently recompute which offsets *should* show up: every value in a "*OFFSETS"-suffixed
    // attribute that isn't in this arch's auto-generated set (matching getOffsetLabelDict's own
    // filter, but derived here from the public CuSMVersion API rather than by calling the function
    // under test on itself).
    const auto& autoGen = CuAsm::CuSMVersion("sm_61").getNVInfoAttrAutoGenSet();
    std::set<std::uint64_t> expectedOffsets;
    for (const CuNVInfoAttr& attr : kernelInfo.m_AttrList) {
        if (!attr.name.ends_with("OFFSETS") || autoGen.count(attr.name)) {
            continue;
        }
        if (const auto* offsets = std::get_if<std::vector<std::uint32_t>>(&attr.value)) {
            for (std::uint32_t off : *offsets) {
                expectedOffsets.insert(off);
            }
        }
    }

    bool offsetSetsMatch = offsetLabels.size() == expectedOffsets.size();
    bool everyLabelWellFormed = true;
    for (const auto& [offset, labelText] : offsetLabels) {
        if (!expectedOffsets.count(offset)) {
            offsetSetsMatch = false;
        }
        if (labelText.find(label) == std::string::npos || !labelText.ends_with("#")) {
            everyLabelWellFormed = false;
        }
    }
    t.check("getOffsetLabelDict()'s offsets exactly match an independent scan for non-autogen "
            "*OFFSETS attributes, with well-formed label text",
            offsetSetsMatch && everyLabelWellFormed);

    // ---- setRegCount: only touches EIATTR_REGCOUNT entries, leaves everything else alone ----

    const std::size_t attrCountBefore = topInfo.m_AttrList.size();
    std::vector<std::pair<int, int>> existingRegCounts;
    for (const CuNVInfoAttr& attr : topInfo.m_AttrList) {
        if (attr.name != "EIATTR_REGCOUNT") {
            continue;
        }
        if (const auto* v = std::get_if<std::vector<std::uint32_t>>(&attr.value); v && !v->empty()) {
            existingRegCounts.emplace_back(static_cast<int>((*v)[0]), static_cast<int>(v->size() > 1 ? (*v)[1] : 0));
        }
    }

    if (!existingRegCounts.empty()) {
        CuNVInfo mutableInfo(topBytes, "sm_61");
        std::map<int, int> newCounts;
        for (const auto& [symIdx, oldCount] : existingRegCounts) {
            newCounts[symIdx] = oldCount + 100; // a value guaranteed different from whatever's there
        }
        const bool allFound = mutableInfo.setRegCount(newCounts);

        bool everyRegCountUpdated = true;
        for (const CuNVInfoAttr& attr : mutableInfo.m_AttrList) {
            if (attr.name != "EIATTR_REGCOUNT") {
                continue;
            }
            const auto* v = std::get_if<std::vector<std::uint32_t>>(&attr.value);
            if (!v || v->size() != 2 || newCounts.at(static_cast<int>((*v)[0])) != static_cast<int>((*v)[1])) {
                everyRegCountUpdated = false;
            }
        }
        t.check("setRegCount() updates every EIATTR_REGCOUNT entry to the requested value and "
                "reports every symbol found, without changing the attribute count",
                allFound && everyRegCountUpdated && mutableInfo.m_AttrList.size() == attrCountBefore);

        // Omitting one symbol from the dict makes setRegCount() report not-all-found.
        std::map<int, int> partialCounts = newCounts;
        partialCounts.erase(partialCounts.begin());
        CuNVInfo partialInfo(topBytes, "sm_61");
        t.check("setRegCount() reports false when the dict is missing a symbol that needs updating",
                !partialInfo.setRegCount(partialCounts));
    } else {
        std::cout << "[INFO] fixture's top-level .nv.info has no EIATTR_REGCOUNT entries; "
                     "setRegCount() coverage limited to the (still-real) no-op case below\n";
        std::map<int, int> empty;
        CuNVInfo noopInfo(topBytes, "sm_61");
        t.check("setRegCount() with no REGCOUNT entries present is a no-op that reports all-found (vacuously)",
                noopInfo.setRegCount(empty));
    }

    return t.finish("test_CuNVInfo");
}
