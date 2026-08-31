#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "../../../CuAsm/CuAsmLogger.hpp"
#include "../../../CuAsm/CuAsmParser.hpp"
#include "../../../CuAsm/CubinFile.hpp"
#include "../../../CuAsm/common.hpp"
#include "test_bc_enum.hpp"

// baseline result for:
// -  grid=1600, block=128, NIter=256, 192 instruction per iteration
// -  No bank conflict
// -  MemFreq = 2505 MHz, SMFreq = 954 MHz
static constexpr double BASELINE = 16.74;

/**
 * @brief Result of parsing a benchmark run's captured stdout: average/stddev timing plus the
 *        reported result value and a formatted listing of every individual timing.
 */
struct ParsedResult {
    double tavg;
    double tstd;
    std::string tres;
    std::string sfull;
};

/**
 * @brief Formats a double the way Python's f'{v:{width}.{prec}f}' does (space-padded).
 * @param v Value to format.
 * @param width Minimum field width.
 * @param prec Number of digits after the decimal point.
 * @return The formatted string.
 */
static std::string fmtF(double v, int width, int prec) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%*.*f", width, prec, v);
    return buf;
}

/**
 * @brief Formats an int the way Python's f'{v:{width}d}' does (space-padded).
 * @param v Value to format.
 * @param width Minimum field width.
 * @return The formatted string.
 */
static std::string fmtD(int v, int width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%*d", width, v);
    return buf;
}

/**
 * @brief Formats an int the way Python's f'{v:0{width}d}' does (zero-padded).
 * @param v Value to format.
 * @param width Minimum field width.
 * @return The formatted string.
 */
static std::string fmtD0(int v, int width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*d", width, v);
    return buf;
}

/**
 * @brief Formats a string the way Python's f'{s:{width}s}' does (left-justified, space-padded).
 * @param s String to format.
 * @param width Minimum field width.
 * @return The formatted string.
 */
static std::string fmtS(const std::string& s, int width) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%-*s", width, s.c_str());
    return buf;
}

/**
 * @brief Repeats a string a given number of times, mirroring Python's `s * n`.
 * @param s String to repeat.
 * @param n Number of repetitions.
 * @return The concatenated result.
 */
static std::string repeat(const std::string& s, int n) {
    std::string result;
    result.reserve(s.size() * static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) result += s;
    return result;
}

/**
 * @brief Trims leading/trailing whitespace, mirroring Python's str.strip().
 * @param s String to trim.
 * @return The trimmed string.
 */
static std::string strip(const std::string& s) {
    std::size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    std::size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

/**
 * @brief Replaces all occurrences of a substring within a string.
 * @param s String to search within.
 * @param from Substring to replace.
 * @param to Replacement substring.
 * @return The resulting string, with every occurrence of from replaced by to.
 */
static std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

/**
 * @brief Tool class for generating a text file from a template annotated with
 *        @CUASM_INSERT_MARKER_POS.NAME marker lines. Unlike CuAsm::FileTemplate, the original
 *        marker line is always written back verbatim (never commented out).
 */
class CuAsmTemplate {
public:
    /**
     * @brief Constructs the template from a template file, splitting it into literal text parts
     *        and marker-defining lines.
     * @param templateName Path to the template file.
     */
    explicit CuAsmTemplate(const std::string& templateName) {
        static const std::regex markerPattern(R"(@CUASM_INSERT_MARKER_POS\.(\w+))");

        std::ifstream fin(templateName);
        std::string buf;
        int iline = 0;
        std::string line;
        while (std::getline(fin, line)) {
            ++iline;
            line += '\n';

            std::smatch res;
            if (std::regex_search(line, res, markerPattern)) {
                m_FileParts.emplace_back(buf);
                buf.clear();

                std::string marker = res[1].str();
                if (m_MarkerDict.find(marker) != m_MarkerDict.end()) {
                    std::cout << "  Duplicate marker \"" << marker << "\" in line " << iline << "\n";
                } else {
                    m_MarkerDict[marker] = std::nullopt;
                }

                m_FileParts.emplace_back(MarkerLine{marker, line});
            } else {
                buf += line;
            }
        }

        m_FileParts.emplace_back(buf);
    }

    /**
     * @brief Sets the replacement text for a marker.
     * @param marker Marker name to set.
     * @param s Replacement text to insert after the marker line.
     */
    void setMarker(const std::string& marker, const std::string& s) {
        m_MarkerDict[marker] = s;
    }

    /**
     * @brief Clears all marker replacement texts, reverting to the unset state.
     */
    void resetAllMarkers() {
        for (auto& [k, v] : m_MarkerDict) v = std::nullopt;
    }

    /**
     * @brief Generates an output file from the template, inserting marker replacement texts
     *        after each marker-defining line (which is always written back verbatim).
     * @param outfile Path of the file to write.
     */
    void generate(const std::string& outfile) const {
        std::ofstream fout(outfile);
        for (const auto& p : m_FileParts) {
            if (std::holds_alternative<std::string>(p)) {
                fout << std::get<std::string>(p);
            } else {
                const auto& [marker, origLine] = std::get<MarkerLine>(p);

                fout << origLine;

                const auto& value = m_MarkerDict.at(marker);
                if (value.has_value()) {
                    fout << *value;
                    fout << '\n'; // ensure a newline after the insertion
                }
            }
        }
    }

private:
    using MarkerLine = std::pair<std::string, std::string>;
    using FilePart = std::variant<std::string, MarkerLine>;

    std::vector<FilePart> m_FileParts;
    std::map<std::string, std::optional<std::string>> m_MarkerDict;
};

/**
 * @brief Disassembles a cubin file and saves the result as a .cuasm file.
 * @param binname Path of the input cubin file.
 * @param asmname Path of the output .cuasm file; if empty, derived from binname.
 */
static void cubin2cuasm(const std::string& binname, const std::string& asmname = "") {
    CuAsm::CubinFile cf(binname);

    std::string outname = asmname;
    if (outname.empty()) {
        if (binname.size() >= 6 && binname.compare(binname.size() - 6, 6, ".cubin") == 0) {
            outname = replaceAll(binname, ".cubin", ".cuasm");
        } else {
            outname = binname + ".cuasm";
        }
    }

    cf.saveAsCuAsm(outname);
}

/**
 * @brief Assembles a .cuasm source file and saves the result as a .cubin file.
 * @param asmname Path of the input .cuasm file.
 * @param binname Path of the output cubin file; if empty, derived from asmname.
 */
static void cuasm2cubin(const std::string& asmname, const std::string& binname = "") {
    CuAsm::CuAsmParser cap;
    cap.parse(asmname);

    std::string outname = binname;
    if (outname.empty()) {
        if (asmname.size() >= 6 && asmname.compare(asmname.size() - 6, 6, ".cuasm") == 0) {
            outname = replaceAll(asmname, ".cuasm", ".cubin");
        } else {
            outname = asmname + ".cubin";
        }
    }

    cap.saveAsCubin(outname);
}

/**
 * @brief Rebuilds the repeated-instruction cuasm into a cubin and, if not using the driver API,
 *        rebuilds the microbenchmark executable via make.
 * @param useDriverApi Whether the driver API is used to launch the kernel (skips the make step).
 */
static void build(bool useDriverApi = true) {
    cuasm2cubin(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.rep.sm_50.cuasm)");
    if (!useDriverApi) {
#ifdef _WIN32
        _putenv_s("PTXAS_HACK", R"(G:\Work\CuAssembler\TestData\microbench\RegBank\ptxhack.map)");
#else
        setenv("PTXAS_HACK", R"(G:\Work\CuAssembler\TestData\microbench\RegBank\ptxhack.map)", 1);
#endif
        CuAsm::checkOutput({"make", "clean"});
        CuAsm::checkOutput({"make"});
    }
}

/**
 * @brief Smoke-tests CuAsmTemplate by inserting placeholder text at the INIT/WORK_1 markers.
 */
static void template_test() {
    CuAsmTemplate cat(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.template.sm_50.cuasm)");

    cat.setMarker("INIT", "      // hehe init here!");
    cat.setMarker("WORK_1", "      // work1 here!");

    cat.generate("test.cuasm");
}

/**
 * @brief Parses a microbenchmark run's captured stdout, extracting per-iteration timings (ms)
 *        and the reported res[0] value.
 * @param res Captured stdout of the benchmark executable.
 * @return The parsed average/stddev timing, reported result, and formatted timing listing.
 */
static ParsedResult parseResult(const std::string& res) {
    static const std::regex testPattern(R"(Test(.*):(.*)ms)");
    static const std::regex resPattern(R"(res\[\s*0\]\s*:(.*))");

    int nt = 0;
    double tsum = 0.0;
    double tsq = 0.0;
    std::string tres;
    std::vector<double> tlist;

    std::istringstream iss(res);
    std::string line;
    while (std::getline(iss, line)) {
        std::smatch m;
        if (std::regex_search(line, m, testPattern)) {
            double t = std::stod(m[2].str());
            tlist.push_back(t);

            ++nt;
            tsum += t;
            tsq += t * t;
        }

        std::smatch m2;
        if (std::regex_search(line, m2, resPattern)) {
            tres = strip(m2[1].str());
        }
    }

    double tavg = tsum / nt;
    double tstd = std::sqrt(tsq / nt - tavg * tavg);

    std::string sfull = "[";
    for (std::size_t i = 0; i < tlist.size(); ++i) {
        if (i > 0) sfull += ", ";
        sfull += fmtF(tlist[i], 8, 3);
    }
    sfull += "]";

    return {tavg, tstd, tres, sfull};
}

/**
 * @brief Runs the compiled microbenchmark executable and captures its stdout, exiting the
 *        process with the same return code if the run fails.
 * @return Captured stdout of the benchmark executable.
 */
static std::string run_exe() {
    try {
        return CuAsm::checkOutput({"regbank_test.exe"});
    } catch (const CuAsm::CalledProcessError& e) {
        std::cout << "Error happened when running exe (return code=" << e.returnCode() << ")!\n";
        std::exit(e.returnCode());
    }
}

/**
 * @brief Builds and runs the no-bank-conflict microbenchmark for a given stall/yield/regnum config.
 * @param stall1 Stall count for the first FFMA in the work sequence.
 * @param stall2 Stall count for the remaining FFMAs in the work sequence.
 * @param yflag Yield flag ("-" or "Y") applied to every instruction.
 * @param regnum Register count reported in the generated cuasm's section info.
 * @return The parsed benchmark result.
 */
static ParsedResult test_NoBankConflict(int stall1, int stall2, const std::string& yflag, int regnum) {
    CuAsmTemplate cat(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.template.sm_50.cuasm)");

    std::string s_init = "[----:B------:R-:W-:-:S01]  MOV R4, RZ ; \n";
    for (int r = 8; r <= 16; ++r) {
        s_init += "[----:B------:R-:W-:-:S01]  MOV32I R" + std::to_string(r) + ", 0x3f800000 ; \n";
    }

    std::string s_work1 = "      [----:B------:R-:W-:" + yflag + ":S" + fmtD0(stall1, 2) + "]  FFMA R4, R9, R10, R4; \n";
    for (int i = 0; i < 5; ++i) {
        s_work1 += "      [----:B------:R-:W-:" + yflag + ":S" + fmtD0(stall2, 2) + "]  FFMA R" + std::to_string(i + 12) + ", R9, R10, R11; \n";
    }
    s_work1 = repeat(s_work1, 32);
    std::string s_work2 = s_work1;

    cat.setMarker("INIT", s_init);
    cat.setMarker("WORK_1", s_work1);
    cat.setMarker("WORK_2", s_work2);

    std::string s_regnum = "  \t.sectioninfo\t@\"SHI_REGISTERS=" + std::to_string(regnum) + "\"";
    cat.setMarker("REGNUM", s_regnum);

    cat.generate(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.rep.sm_50.cuasm)");

    build();

    return parseResult(run_exe());
}

/**
 * @brief Sweeps stall/yield configurations for the no-bank-conflict microbenchmark, writing
 *        results to res_NoBankConflict.txt.
 */
static void doTest_NoBankConflict() {
    std::ofstream fout("res_NoBankConflict.txt");
    int regnum = 32;
    double tratio = 0.0;

    for (int stall = 1; stall <= 15; ++stall) {
        for (const char* yflag : {"-", "Y"}) {
            auto [tavg, tstd, tres, sfull] = test_NoBankConflict(stall, stall, yflag, regnum);
            tratio = tavg * 6 / BASELINE;
            std::string s = std::string("[") + yflag + ":S" + fmtD0(stall, 2) + "]: " + fmtF(tavg, 8, 3) + " (" + fmtF(tratio, 8, 4) + "), " + fmtF(tstd, 8, 3) + ",  " + tres + "  " + sfull;
            std::cout << s << "\n";
            fout << s << "\n";
        }
    }

    std::string header = "\n######### S## + 5*S01 ############\n";
    std::cout << header;
    fout << header << "\n";

    for (int stall = 1; stall <= 15; ++stall) {
        for (const char* yflag : {"-", "Y"}) {
            auto [tavg, tstd, tres, sfull] = test_NoBankConflict(stall, 1, yflag, 32);
            // NOTE: tratio is not recomputed here in the original Python (it reuses the value
            // left over from the last iteration of the loop above) — mirrored verbatim.
            std::string s = std::string("[") + yflag + ":S" + fmtD0(stall, 2) + "]: " + fmtF(tavg, 8, 3) + " (" + fmtF(tratio, 8, 4) + "), " + fmtF(tstd, 8, 3) + ",  " + tres + "  " + sfull;
            std::cout << s << "\n";
            fout << s << "\n";
        }
    }
}

/**
 * @brief Builds and runs the bank-conflict-comb microbenchmark for a given instruction sequence.
 * @param stall Stall count applied to every instruction in the sequence.
 * @param ins_seq Sequence of FFMA instruction lines (as produced by genBankConflictInsSeq).
 * @return The parsed benchmark result.
 */
static ParsedResult test_BankConflictComb(int stall, const std::vector<std::string>& ins_seq) {
    CuAsmTemplate cat(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.template.sm_50.cuasm)");

    std::string s_init = "[----:B------:R-:W-:-:S01]  MOV32I R4, 0x7ff00000 ; \n"; // R4 should not be modified in ins_seq
    std::string s_work1;
    for (const auto& ins : ins_seq) {
        s_work1 += "      [----:B------:R-:W-:-:S" + fmtD0(stall, 2) + "] " + ins + " \n";
    }

    s_work1 = repeat(s_work1, 32);
    std::string s_work2 = s_work1;

    cat.setMarker("INIT", s_init);
    cat.setMarker("WORK_1", s_work1);
    cat.setMarker("WORK_2", s_work2);
    cat.generate(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.rep.sm_50.cuasm)");

    build();
    return parseResult(run_exe());
}

/**
 * @brief Runs the bank-conflict-comb microbenchmark across every combination enumerated by
 *        buildCombDict(), writing results to res_BankConflictComb.txt.
 */
static void doTest_BankConflictComb() {
    auto comb_dict = buildCombDict();

    std::ofstream fout("res_BankConflictComb.txt");

    // std::map keeps keys sorted ascending; iterating in reverse gives descending order,
    // equivalent to Python's `comb_keys.sort(reverse=True)`.
    int i = 0;
    for (auto it = comb_dict.rbegin(); it != comb_dict.rend(); ++it, ++i) {
        const auto& [i0, i1, i2] = it->second;
        std::string ks = getCombStr(i0, i1, i2);
        auto ins_seq = genBankConflictInsSeq(i0, i1, i2);

        std::array<int, 4> bsum = {i0[0] + i1[0] + i2[0], i0[1] + i1[1] + i2[1], i0[2] + i1[2] + i2[2], i0[3] + i1[3] + i2[3]};
        std::string bs;
        for (int x : bsum) bs += std::to_string(x);
        int besti = 2 * *std::max_element(bsum.begin(), bsum.end());

        auto [tavg, tstd, tres, sfull] = test_BankConflictComb(1, ins_seq);
        double tratio = tavg * 6 / BASELINE;
        std::string s = "Comb" + fmtD(i + 1, 4) + " : [" + ks + "] (" + bs + ":" + fmtD(besti, 2) + "): " + fmtF(tavg, 8, 3) + " (" + fmtF(tratio, 8, 4) + "), " + fmtF(tstd, 8, 3) + ",  " + tres + "  " + sfull;
        std::cout << s << "\n";
        fout << s << "\n";
    }
}

/**
 * @brief Builds and runs the register-reuse bank-conflict microbenchmark for a given register
 *        pair and reuse-flag pair.
 * @param r1 First source register index.
 * @param r2 Second source register index.
 * @param reuse1 Reuse flag ("-" or "R") for the first source operand.
 * @param reuse2 Reuse flag ("-" or "R") for the second source operand.
 * @return The parsed benchmark result.
 */
static ParsedResult test_ReuseBankConflict(int r1, int r2, const std::string& reuse1, const std::string& reuse2) {
    CuAsmTemplate cat(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.template.sm_50.cuasm)");

    std::string s_init = "[----:B------:R-:W-:-:S01]  MOV R4, RZ ; \n";
    for (int r = 8; r <= 16; ++r) {
        s_init += "[----:B------:R-:W-:-:S01]  MOV32I R" + std::to_string(r) + ", 0x3f800000 ; \n";
    }

    std::string reuse_s = reuse1 + reuse2 + "--";
    std::string s_work1 = "      [" + reuse_s + ":B------:R-:W-:-:S01]  FFMA R4, R" + std::to_string(r1) + ", R" + std::to_string(r2) + ", R4; \n";
    for (int i = 0; i < 5; ++i) {
        s_work1 += "      [" + reuse_s + ":B------:R-:W-:-:S01]  FFMA R" + std::to_string(i + 20) + ", R" + std::to_string(r1) + ", R" + std::to_string(r2) + ", R16; \n";
    }
    s_work1 = repeat(s_work1, 32);
    std::string s_work2 = s_work1;

    cat.setMarker("INIT", s_init);
    cat.setMarker("WORK_1", s_work1);
    cat.setMarker("WORK_2", s_work2);

    cat.generate(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.rep.sm_50.cuasm)");

    build();

    return parseResult(run_exe());
}

/**
 * @brief Sweeps register pairs and reuse-flag combinations for the register-reuse bank-conflict
 *        microbenchmark, writing results to res_ReuseBankConflict.txt.
 */
static void doTest_ReuseBankConflict() {
    std::ofstream fout("res_ReuseBankConflict.txt");
    for (int r1 = 8; r1 <= 15; ++r1) {
        std::string r1s = "R" + std::to_string(r1);
        for (int r2 = 8; r2 <= 15; ++r2) {
            std::string r2s = "R" + std::to_string(r2);
            for (const char* reuse1 : {"-", "R"}) {
                for (const char* reuse2 : {"-", "R"}) {
                    auto [tavg, tstd, tres, sfull] = test_ReuseBankConflict(r1, r2, reuse1, reuse2);
                    double tratio = tavg * 6 / BASELINE;
                    std::string s = "(" + fmtS(r1s, 3) + ", " + fmtS(r2s, 3) + ", \"" + reuse1 + reuse2 + "\"): " + fmtF(tavg, 8, 3) + " (" + fmtF(tratio, 8, 4) + "), " + fmtF(tstd, 8, 3) + ",  " + tres + "  " + sfull;
                    std::cout << s << "\n";
                    fout << s << "\n";
                }
            }
        }
    }
}

/**
 * @brief Builds and runs the reuse-cache-stall microbenchmark for a given register pair,
 *        stall count, reuse pattern, and register count.
 * @param r1 First source register index.
 * @param r2 Second source register index.
 * @param stall Stall count applied to every FFMA in the work sequence.
 * @param reuse_s Four-character reuse-flag string applied to every instruction.
 * @param regnum Register count reported in the generated cuasm's section info.
 * @return The parsed benchmark result.
 */
static ParsedResult test_ReuseStall(int r1, int r2, int stall, const std::string& reuse_s, int regnum) {
    CuAsmTemplate cat(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.template.sm_50.cuasm)");

    std::string s_init = "[----:B------:R-:W-:-:S01]  MOV R4, RZ ; \n";
    for (int r = 8; r <= 16; ++r) {
        s_init += "[----:B------:R-:W-:-:S01]  MOV32I R" + std::to_string(r) + ", 0x3f800000 ; \n";
    }

    std::string s_work1 = "      [" + reuse_s + ":B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R4, R" + std::to_string(r1) + ", R" + std::to_string(r2) + ", R4; \n";
    for (int i = 0; i < 5; ++i) {
        s_work1 += "      [" + reuse_s + ":B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R" + std::to_string(i + 20) + ", R" + std::to_string(r1) + ", R" + std::to_string(r2) + ", R16; \n";
    }
    s_work1 = repeat(s_work1, 32);
    std::string s_work2 = s_work1;

    cat.setMarker("INIT", s_init);
    cat.setMarker("WORK_1", s_work1);
    cat.setMarker("WORK_2", s_work2);

    std::string s_regnum = "  \t.sectioninfo\t@\"SHI_REGISTERS=" + std::to_string(regnum) + "\"";
    cat.setMarker("REGNUM", s_regnum);

    cat.generate(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.rep.sm_50.cuasm)");

    build();

    return parseResult(run_exe());
}

/**
 * @brief Sweeps stall/reuse/regnum configurations for the reuse-cache-stall microbenchmark on
 *        the (R8,R12) and (R8,R9) register pairs, writing results to res_ReuseStall.txt.
 */
static void doTest_ReuseStall() {
    std::ofstream fout("res_ReuseStall.txt");

    std::string header1 = "\n#### (R8, R12) ####\n";
    std::cout << header1;
    fout << header1 << "\n";

    for (int stall = 1; stall <= 15; ++stall) {
        for (const char* reuse_s : {"----", "RR--"}) {
            for (int regnum : {254, 160, 128, 96, 80, 64, 40, 32}) {
                int occu = std::min(16, 512 / regnum);
                auto [tavg, tstd, tres, sfull] = test_ReuseStall(8, 12, stall, reuse_s, regnum);
                double tratio = tavg * 6 / BASELINE;
                std::string s = std::string("[") + reuse_s + ":S" + fmtD0(stall, 2) + "] (RegNum=" + fmtD(regnum, 3) + ", Occu=" + fmtD(occu, 2) + "): " + fmtF(tavg, 8, 3) + " (" + fmtF(tratio, 8, 4) + "), " + fmtF(tstd, 8, 3) + ",  " + tres + "  " + sfull;
                std::cout << s << "\n";
                fout << s << "\n";
            }
        }
    }

    std::string header2 = "\n#### (R8, R9) ####\n";
    std::cout << header2;
    fout << header2 << "\n";

    for (int stall = 1; stall <= 15; ++stall) {
        for (const char* reuse_s : {"----", "R---"}) {
            for (int regnum : {254, 160, 128, 96, 80, 64, 40, 32}) {
                int occu = std::min(16, 512 / regnum);
                auto [tavg, tstd, tres, sfull] = test_ReuseStall(8, 9, stall, reuse_s, regnum);
                double tratio = tavg * 6 / BASELINE;
                std::string s = std::string("[") + reuse_s + ":S" + fmtD0(stall, 2) + "] (RegNum=" + fmtD(regnum, 3) + ", Occu=" + fmtD(occu, 2) + "): " + fmtF(tavg, 8, 3) + " (" + fmtF(tratio, 8, 4) + "), " + fmtF(tstd, 8, 3) + ",  " + tres + "  " + sfull;
                std::cout << s << "\n";
                fout << s << "\n";
            }
        }
    }
}

/**
 * @brief Builds and runs the reuse-cache-switch microbenchmark for a given stall count, register
 *        cycle length, and reuse clip position.
 * @param stall Stall count applied to every FFMA in the work sequence.
 * @param cycle Number of registers in the rotation cycle used by the work sequence.
 * @param clip Index at which reuse flags switch from "RR--" to "----" within the cycle.
 * @return The parsed benchmark result together with the (unrepeated) work sequence text.
 */
static std::pair<ParsedResult, std::string> test_ReuseSwitch(int stall, int cycle, int clip) {
    CuAsmTemplate cat(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.template.sm_50.cuasm)");

    std::string s_init = "[----:B------:R-:W-:-:S01]  MOV R4, RZ ; \n";
    for (int r = 8; r <= 16; ++r) {
        s_init += "[----:B------:R-:W-:-:S01]  MOV32I R" + std::to_string(r) + ", 0x3f800000 ; \n";
    }

    static const std::array<std::array<int, 3>, 4> RList = {{{8, 12, 16}, {9, 13, 5}, {10, 14, 6}, {11, 15, 7}}};

    std::string s_work1;
    if (clip == 0) {
        s_work1 = "      [----:B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R4, R8, R12, R4; \n";
    } else {
        s_work1 = "      [RR--:B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R4, R8, R12, R4; \n";
    }

    for (int i = 1; i <= 5; ++i) {
        int idx = i % cycle;
        const auto& [r1, r2, r3] = RList[idx];

        std::string reuse_s = (idx >= clip) ? "----" : "RR--";
        s_work1 += "      [" + reuse_s + ":B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R" + std::to_string(i + 17) + ", R" + std::to_string(r1) + ", R" + std::to_string(r2) + ", R" + std::to_string(r3) + "; \n";
    }

    std::string s1s = s_work1;
    s_work1 = repeat(s_work1, 32);
    std::string s_work2 = s_work1;

    cat.setMarker("INIT", s_init);
    cat.setMarker("WORK_1", s_work1);
    cat.setMarker("WORK_2", s_work2);

    cat.generate(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.rep.sm_50.cuasm)");

    build();

    return {parseResult(run_exe()), s1s};
}

/**
 * @brief Sweeps stall/cycle/clip configurations for the reuse-cache-switch microbenchmark,
 *        writing results to res_ReuseSwitch.txt.
 */
static void doTest_ReuseSwitch() {
    std::ofstream fout("res_ReuseSwitch.txt");
    for (int stall = 1; stall <= 6; ++stall) {
        for (int cycle : {1, 2, 3}) {
            for (int clip = 0; clip <= cycle; ++clip) {
                auto [result, s1s] = test_ReuseSwitch(stall, cycle, clip);
                auto [tavg, tstd, tres, sfull] = result;
                double tratio = tavg * 6 / BASELINE;
                std::string s = "[S" + fmtD0(stall, 2) + ", Cycle" + std::to_string(cycle) + ", Clip" + std::to_string(clip) + "]: " + fmtF(tavg, 8, 3) + " (" + fmtF(tratio, 8, 4) + "), " + fmtF(tstd, 8, 3) + ",  " + tres + "  " + sfull;

                std::cout << "---------------------------------\n\n";
                std::cout << s1s;
                std::cout << s << "\n";

                fout << "---------------------------------\n";
                fout << s1s;
                fout << s << "\n";
            }
        }
    }
}

/**
 * @brief Builds and runs a hand-written register-reuse microbenchmark and prints its raw output.
 */
static void test_Simple() {
    CuAsmTemplate cat(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.template.sm_50.cuasm)");

    std::string s_init = "[----:B------:R-:W-:-:S06]  MOV R4, RZ ; \n";
    for (int r = 8; r <= 16; ++r) {
        s_init += "[----:B------:R-:W-:-:S06]  MOV32I R" + std::to_string(r) + ", 0x3f800000 ; \n";
    }

    int stall = 1;
    std::string s_work1 = "      [----:B------:R-:W-:Y:S" + fmtD0(stall, 2) + "]  FFMA R4, R9, R10, R4; \n";
    s_work1 += "      [RR--:B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R18, R7, R11, R15;  \n";
    s_work1 += "      [RR--:B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R19, R12, R8, R16;  \n";
    s_work1 += "      [RR--:B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R20, R11, R7, R15; \n";
    s_work1 += "      [RR--:B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R21, R8, R12, R16; \n";
    s_work1 += "      [RR--:B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R22, R7, R11, R15;  \n";
    s_work1 += "      [RR--:B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R23, R12, R8, R16;  \n";
    s_work1 += "      [RR--:B------:R-:W-:-:S" + fmtD0(stall, 2) + "]  FFMA R24, R11, R7, R15;  \n";

    s_work1 = repeat(s_work1, 32);
    std::string s_work2 = s_work1;

    cat.setMarker("INIT", s_init);
    cat.setMarker("WORK_1", s_work1);
    cat.setMarker("WORK_2", s_work2);

    cat.generate(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.rep.sm_50.cuasm)");

    build();

    std::string res = run_exe();
    std::cout << res;
}

/**
 * @brief Builds and runs a second hand-written microbenchmark (no reuse flags) and prints its
 *        raw output.
 */
static void test_Simple2() {
    CuAsmTemplate cat(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.template.sm_50.cuasm)");

    std::string s_init = "[----:B------:R-:W-:-:S06]  MOV R4, RZ ; \n";
    for (int r = 8; r <= 16; ++r) {
        s_init += "[----:B------:R-:W-:-:S06]  MOV32I R" + std::to_string(r) + ", 0x3f800000 ; \n";
    }

    std::string s_work1 = "      [----:B------:R-:W-:-:S03]  FFMA R4, R9, R10, R4; \n";
    s_work1 += "      [----:B------:R-:W-:-:S03]  FFMA R18, R9, R10, R11; \n";
    s_work1 += "      [----:B------:R-:W-:-:S03]  FFMA R19, R9, R10, R11; \n";
    s_work1 += "      [----:B------:R-:W-:-:S03]  FFMA R20, R9, R10, R11; \n";
    s_work1 += "      [----:B------:R-:W-:-:S03]  FFMA R21, R9, R10, R11; \n";
    s_work1 += "      [----:B------:R-:W-:-:S03]  FFMA R22, R9, R10, R11; \n";

    s_work1 = repeat(s_work1, 32);
    std::string s_work2 = s_work1;

    cat.setMarker("INIT", s_init);
    cat.setMarker("WORK_1", s_work1);
    cat.setMarker("WORK_2", s_work2);

    cat.generate(R"(G:\Work\CuAssembler\TestData\microbench\RegBank\regbank_test.rep.sm_50.cuasm)");

    build();

    std::string res = run_exe();
    std::cout << res;
}

/**
 * @brief Entry point: disables logging and runs the bank-conflict-comb microbenchmark sweep,
 *        mirroring the original's `if __name__ == '__main__'` block.
 * @return Always zero.
 */
int main() {
    CuAsm::CuAsmLogger::disable();

    doTest_BankConflictComb();

    return 0;
}
