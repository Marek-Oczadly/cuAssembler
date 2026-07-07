#include "common.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <cstdlib>
#define popen _popen
#define pclose _pclose
#else
#include <cstdlib>
#endif

namespace CuAsm {

namespace {

/**
 * @brief Formats a byte string similarly to python's repr() of a bytes object, e.g. b'ab\x00'.
 * @param s Byte string to format.
 * @return The formatted repr string.
 **/
std::string reprBytes(const std::string& s) {
    std::ostringstream out;
    out << "b'";
    for (unsigned char c : s) {
        if (c == '\\' || c == '\'') {
            out << '\\' << c;
        } else if (c == '\n') {
            out << "\\n";
        } else if (c == '\t') {
            out << "\\t";
        } else if (c == '\r') {
            out << "\\r";
        } else if (c >= 0x20 && c < 0x7f) {
            out << static_cast<char>(c);
        } else {
            out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << std::dec;
        }
    }
    out << '\'';
    return out.str();
}

// cpp style line comments, e.g. "// comment"
const std::regex p_cppcomment(R"(//.*$)");
// c style comments, e.g. "/* comment */"
const std::regex p_ccomment(R"(/\*.*?\*/)");
// notes for bra targets, e.g. (*"BRANCH_TARGETS .L_10"*)
const std::regex p_bracomment(R"(\(\*.*\*\))");

/**
 * @brief Quotes a single command-line argument for the platform's shell, so it can be safely
 *        embedded in the command string passed to popen().
 * @param arg Argument to quote.
 * @return The quoted argument.
 **/
std::string quoteArg(const std::string& arg) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
    return out;
#else
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
#endif
}

} // namespace

CalledProcessError::CalledProcessError(int returnCode, std::string output)
    : std::runtime_error("Command returned non-zero exit status " + std::to_string(returnCode)),
      m_returnCode(returnCode),
      m_output(std::move(output)) {}

int CalledProcessError::returnCode() const noexcept {
    return m_returnCode;
}

const std::string& CalledProcessError::output() const noexcept {
    return m_output;
}

std::string checkOutput(const std::vector<std::string>& args, bool mergeStderr) {
    std::string cmd;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            cmd += ' ';
        }
        cmd += quoteArg(args[i]);
    }
    if (mergeStderr) {
        cmd += " 2>&1";
    }

    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("Failed to launch command: " + cmd);
    }

    std::string output;
    std::array<char, 4096> buf{};
    std::size_t nread;
    while ((nread = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        output.append(buf.data(), nread);
    }

    int status = pclose(pipe);
#ifdef _WIN32
    int returnCode = status;
#else
    int returnCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

    if (returnCode != 0) {
        throw CalledProcessError(returnCode, output);
    }

    return output;
}

std::string getTempFileName(const std::string& name, const std::string& prefix, const std::string& suffix) {
    std::string fpath;
    const char* tmpdir = std::getenv("TMPDIR");
#ifdef _WIN32
    const char* winTemp = std::getenv("TEMP");
    if (winTemp == nullptr) {
        winTemp = std::getenv("TMP");
    }
    fpath = (winTemp != nullptr) ? winTemp : ".";
#else
    fpath = (tmpdir != nullptr) ? tmpdir : "/tmp";
#endif

    std::string pfx = prefix;
    if (!pfx.empty() && pfx.back() != '.') {
        pfx += '.';
    }

    std::string sfx = suffix;
    if (!sfx.empty() && sfx.front() != '.') {
        sfx = "." + sfx;
    }

    if (!name.empty()) {
        return fpath + "/" + pfx + name + sfx;
    }

    static std::mt19937 rng(std::random_device{}());
    static const std::string alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<std::size_t> dist(0, alphabet.size() - 1);

    while (true) {
        std::time_t now = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        std::ostringstream ttag;
        ttag << std::put_time(&tmv, "%m%d-%H%M%S");

        std::string randPart;
        for (int i = 0; i < 8; ++i) {
            randPart += alphabet[dist(rng)];
        }

        std::string fname = fpath + "/" + pfx + ttag.str() + randPart + sfx;
        std::ifstream test(fname);
        if (!test.good()) {
            return fname;
        }
    }
}

std::string binstr(const BigInt& v, int bitlen, int width, const std::string& sp) {
    std::string bv;
    if (v == 0) {
        bv = "0";
    } else {
        for (BigInt x = v; x > 0; x >>= 1) {
            bv += static_cast<char>('0' + static_cast<int>(x & 1));
        }
        std::reverse(bv.begin(), bv.end());
    }

    if (static_cast<int>(bv.size()) < bitlen) {
        bv = std::string(bitlen - bv.size(), '0') + bv;
    }

    std::string out;
    for (int i = 0; i < bitlen; i += width) {
        if (i > 0) {
            out += sp;
        }
        out += bv.substr(i, width);
    }
    return out;
}

std::string hexstr(const BigInt& v, int bitlen, int width, const std::string& sp) {
    std::ostringstream hexOs;
    hexOs << std::hex << v;
    std::string hv = hexOs.str();

    int lhex = bitlen / 4;
    if (static_cast<int>(hv.size()) < lhex) {
        hv = std::string(lhex - hv.size(), '0') + hv;
    }

    std::string out;
    for (int i = 0; i < lhex; i += width) {
        if (i > 0) {
            out += sp;
        }
        out += hv.substr(i, width);
    }
    return out;
}

std::pair<std::uint64_t, std::uint64_t> alignTo(std::uint64_t pos, std::uint64_t align) {
    if (align == 0 || align == 1) {
        return {pos, 0};
    }

    std::uint64_t npos = ((pos + align - 1) / align) * align;
    return {npos, npos - pos};
}

std::string intList2Str(const std::vector<std::int64_t>& vlist, int width) {
    std::ostringstream sio;
    sio << '[';
    for (std::size_t i = 0; i < vlist.size(); ++i) {
        if (i > 0) {
            sio << ", ";
        }

        std::ostringstream hexOs;
        hexOs << std::hex << vlist[i];
        std::string hv = hexOs.str();
        if (width > 0 && static_cast<int>(hv.size()) < width) {
            hv = std::string(width - hv.size(), '0') + hv;
        }
        sio << "0x" << hv;
    }
    sio << ']';
    return sio.str();
}

std::map<std::string, std::pair<int, int>> splitAsmSection(const std::vector<std::string>& lines) {
    static const std::regex m_secdirective(R"(^\s*\.section\s+([\.\w]+),)");

    std::vector<std::string> secnames;
    std::vector<int> markers = {0};

    for (std::size_t iline = 0; iline < lines.size(); ++iline) {
        std::smatch m;
        if (std::regex_search(lines[iline], m, m_secdirective)) {
            std::string secname = m[1].str();
            secnames.push_back(secname);

            bool hasPrevComment = false;
            if (iline > 0) {
                std::string prevLine = lines[iline - 1];
                // trim
                std::size_t b = prevLine.find_first_not_of(" \t\r\n");
                std::size_t e = prevLine.find_last_not_of(" \t\r\n");
                prevLine = (b == std::string::npos) ? "" : prevLine.substr(b, e - b + 1);
                if (prevLine.rfind("//", 0) == 0 && prevLine.find(secname) != std::string::npos) {
                    hasPrevComment = true;
                }
            }

            markers.push_back(hasPrevComment ? static_cast<int>(iline) - 1 : static_cast<int>(iline));
            markers.push_back(static_cast<int>(iline));
        }
    }

    markers.push_back(static_cast<int>(lines.size()));

    std::map<std::string, std::pair<int, int>> sectionMarkers;
    sectionMarkers["$FileHeader"] = {markers[0], markers[1]};

    for (std::size_t isec = 0; isec < secnames.size(); ++isec) {
        sectionMarkers[secnames[isec]] = {markers[2 * isec + 2], markers[2 * isec + 3]};
    }

    return sectionMarkers;
}

std::string stringBytes2Asm(const std::string& ss, const std::string& label, int width) {
    std::size_t p = 0;
    int counter = 0;

    std::ostringstream sio;
    while (true) {
        std::size_t pnext = ss.find('\0', p);
        if (pnext == std::string::npos) {
            break;
        }

        std::string s = ss.substr(p, pnext - p + 1);
        sio << "    // " << label << '[' << counter << "] = " << reprBytes(s) << " \n";

        std::size_t p0 = p;
        while (p0 < pnext + 1) {
            sio << "    /*" << std::hex << std::setw(4) << std::setfill('0') << p0 << std::dec << "*/ .byte ";
            for (std::size_t k = p0; k < std::min(p0 + width, pnext + 1); ++k) {
                if (k > p0) {
                    sio << ", ";
                }
                sio << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(s[k - p])) << std::dec;
            }
            sio << '\n';
            p0 += width;
        }

        sio << '\n';
        p = pnext + 1;
        ++counter;
    }

    return sio.str();
}

std::string bytes2Asm(const std::string& bs, int width, std::uint64_t addrOffset, const std::string& ident) {
    std::ostringstream sio;

    std::size_t p = 0;
    while (p < bs.size()) {
        sio << ident << "/*" << std::hex << std::setw(4) << std::setfill('0') << (p + addrOffset) << std::dec << "*/ .byte ";
        std::size_t end = std::min(p + static_cast<std::size_t>(width), bs.size());
        for (std::size_t k = p; k < end; ++k) {
            if (k > p) {
                sio << ", ";
            }
            sio << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(bs[k])) << std::dec;
        }
        sio << '\n';
        p += width;
    }

    return sio.str();
}

void bytesdump(const std::string& inname, const std::string& outname) {
    std::ifstream fin(inname, std::ios::binary);
    if (!fin) {
        throw std::runtime_error("Failed to open input file: " + inname);
    }
    std::string bs((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());

    std::string bstr = bytes2Asm(bs);

    std::ofstream fout(outname);
    if (!fout) {
        throw std::runtime_error("Failed to open output file: " + outname);
    }
    fout << bstr;
}

std::string stripComments(const std::string& s) {
    std::string out = std::regex_replace(s, p_cppcomment, " ");
    out = std::regex_replace(out, p_ccomment, " ");
    out = std::regex_replace(out, p_bracomment, " ");
    out = std::regex_replace(out, std::regex(R"(\s+)"), " ");

    std::size_t b = out.find_first_not_of(' ');
    std::size_t e = out.find_last_not_of(' ');
    if (b == std::string::npos) {
        return "";
    }
    return out.substr(b, e - b + 1);
}

} // namespace CuAsm
