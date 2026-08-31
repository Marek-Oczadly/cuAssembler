#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "../../CuAsm/utils/FileTemplate.hpp"
#include "TestUtilsCommon.hpp"

namespace fs = std::filesystem;
using CuAsm::FileTemplate;

namespace {

/** @brief Writes a fixture template file, in binary mode so the '\n's below are written as-is. */
void writeFixture(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

/** @brief Reads an entire generated output file back in, for content checks. */
std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/** @brief Counts non-overlapping occurrences of needle in haystack. */
std::size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t p = haystack.find(needle); p != std::string::npos; p = haystack.find(needle, p + 1)) {
        ++count;
    }
    return count;
}

} // namespace

/**
 * @brief Exercises CuAsm::FileTemplate, the marker-substitution engine used to generate
 *        variant source files (e.g. RegBank's microbenchmark .cuasm templates) from a single
 *        template with `@FT_MARKER.NAME` placeholder lines. Covers: unset markers staying
 *        commented out, setMarker()/generate() substitution, resetAllMarkers(), a non-default
 *        comment string, lines that merely *contain* the marker text without being recognized as
 *        one (must pass through verbatim), duplicate marker declarations not corrupting state,
 *        and a missing template file failing cleanly.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    const std::string testDir = std::string(CUASM_TESTDATA_DIR) + "/CuTest";
    const std::string templatePath = testDir + "/tmp_test_filetemplate.txt";
    const std::string dupTemplatePath = testDir + "/tmp_test_filetemplate_dup.txt";
    const std::string outPath = testDir + "/tmp_test_filetemplate.out.txt";

    writeFixture(templatePath,
                 "Header line\n"
                 "@FT_MARKER.POS1\n"
                 "Middle line\n"
                 "// @FT_MARKER.IGNORED (doesn't start the line, so it's just text)\n"
                 "@FT_MARKER.POS2\n"
                 "Footer line\n");

    FileTemplate ft(templatePath);
    ft.generate(outPath);
    std::string out = readAll(outPath);
    t.check("with no marker set, both markers are commented out and non-marker text/lines that merely "
            "contain marker-like text pass through unmodified",
            out.find("// @FT_MARKER.POS1") != std::string::npos && out.find("// @FT_MARKER.POS2") != std::string::npos &&
                out.find("Header line") != std::string::npos &&
                out.find("// @FT_MARKER.IGNORED (doesn't start the line, so it's just text)") != std::string::npos);

    ft.setMarker("POS1", "REPLACED_POS1_TEXT");
    ft.generate(outPath);
    out = readAll(outPath);
    t.check("setMarker substitutes replacement text after the (still-present) marker comment line, "
            "leaving an unset marker alone",
            out.find("REPLACED_POS1_TEXT") != std::string::npos &&
                out.find("// @FT_MARKER.POS1") < out.find("REPLACED_POS1_TEXT") &&
                out.find("// @FT_MARKER.POS2") != std::string::npos);

    ft.resetAllMarkers();
    ft.generate(outPath);
    out = readAll(outPath);
    t.check("resetAllMarkers reverts to the unset (commented-out only) state",
            out.find("REPLACED_POS1_TEXT") == std::string::npos && out.find("// @FT_MARKER.POS1") != std::string::npos);

    FileTemplate ftHash(templatePath, "#");
    ftHash.generate(outPath);
    t.check("a non-default comment string is used to comment out unset markers",
            readAll(outPath).find("# @FT_MARKER.POS1") != std::string::npos);

    writeFixture(dupTemplatePath, "@FT_MARKER.DUP\nmiddle\n@FT_MARKER.DUP\n");
    FileTemplate ftDup(dupTemplatePath);
    ftDup.setMarker("DUP", "DUP_TEXT");
    ftDup.generate(outPath);
    t.checkEqual("a marker declared twice (logged as a duplicate, not rejected) is substituted at both occurrences",
                 countOccurrences(readAll(outPath), "DUP_TEXT"), std::size_t(2));

    t.checkThrows<std::runtime_error>("constructing a FileTemplate from a nonexistent file throws cleanly",
                                       [&] { (void)FileTemplate(testDir + "/tmp_this_file_does_not_exist.txt"); });

    std::error_code ec;
    fs::remove(templatePath, ec);
    fs::remove(dupTemplatePath, ec);
    fs::remove(outPath, ec);

    return t.finish("test_FileTemplate");
}
