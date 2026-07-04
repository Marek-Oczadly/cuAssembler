#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "test_bc_enum.hpp"

using Bank3 = std::array<int, 3>;
using CombKey = std::array<Bank3, 4>;

/**
 * @brief Possible bank (read) distribution of ONE instruction.
 */
static const std::vector<Bank4> insBankList = {
    {3, 0, 0, 0}, {0, 3, 0, 0}, {0, 0, 3, 0}, {0, 0, 0, 3},
    {2, 1, 0, 0}, {2, 0, 1, 0}, {2, 0, 0, 1}, {1, 2, 0, 0},
    {0, 2, 1, 0}, {0, 2, 0, 1}, {1, 0, 2, 0}, {0, 1, 2, 0},
    {0, 0, 2, 1}, {1, 0, 0, 2}, {0, 1, 0, 2}, {0, 0, 1, 2},
    {1, 1, 1, 0}, {1, 1, 0, 1}, {1, 0, 1, 1}, {0, 1, 1, 1},
    {2, 0, 0, 0}, {0, 2, 0, 0}, {0, 0, 2, 0}, {0, 0, 0, 2},
    {1, 1, 0, 0}, {1, 0, 1, 0}, {1, 0, 0, 1}, {0, 1, 1, 0}, {0, 1, 0, 1}, {0, 0, 1, 1},
    {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

/**
 * @brief Formats a 3-tuple the way Python's str() renders a tuple of three ints.
 * @param t Tuple to format.
 * @return String of the form "(a, b, c)".
 */
static std::string formatBank3(const Bank3& t) {
    std::ostringstream oss;
    oss << '(' << t[0] << ", " << t[1] << ", " << t[2] << ')';
    return oss.str();
}

/**
 * @brief Formats a combination key the way Python's str() renders a tuple of four 3-tuples.
 * @param key Combination key to format.
 * @return String of the form "((a, b, c), (d, e, f), (g, h, i), (j, k, l))".
 */
static std::string formatCombKey(const CombKey& key) {
    std::ostringstream oss;
    oss << '(';
    for (std::size_t i = 0; i < key.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << formatBank3(key[i]);
    }
    oss << ')';
    return oss.str();
}

/**
 * @brief Computes the canonical combination key for three instructions' bank-read distributions,
 *        invariant to cyclic rotation of the three instructions.
 * @param ib0 Bank-read distribution of the first instruction.
 * @param ib1 Bank-read distribution of the second instruction.
 * @param ib2 Bank-read distribution of the third instruction.
 * @return String representation of the canonical combination key.
 */
static std::string getCombKey(const Bank4& ib0, const Bank4& ib1, const Bank4& ib2) {
    std::array<Bank4, 3> ibs = {ib0, ib1, ib2};
    CombKey best{};
    bool haveBest = false;

    for (int i = 0; i < 3; ++i) {
        const Bank4& sib0 = ibs[i];
        const Bank4& sib1 = ibs[(i + 1) % 3];
        const Bank4& sib2 = ibs[(i + 2) % 3];

        CombKey bs = {{{sib0[0], sib1[0], sib2[0]},
                        {sib0[1], sib1[1], sib2[1]},
                        {sib0[2], sib1[2], sib2[2]},
                        {sib0[3], sib1[3], sib2[3]}}};
        std::sort(bs.begin(), bs.end(), std::greater<Bank3>());

        if (!haveBest || bs > best) {
            best = bs;
            haveBest = true;
        }
    }

    return formatCombKey(best);
}

/**
 * @brief Concatenates each bank-read distribution's digits and joins the three with commas.
 * @param ib0 Bank-read distribution of the first instruction.
 * @param ib1 Bank-read distribution of the second instruction.
 * @param ib2 Bank-read distribution of the third instruction.
 * @return Comma-separated digit strings, e.g. "3000,0300,0030".
 */
std::string getCombStr(const Bank4& ib0, const Bank4& ib1, const Bank4& ib2) {
    auto digits = [](const Bank4& b) {
        std::string s;
        for (int x : b) s += std::to_string(x);
        return s;
    };
    return digits(ib0) + ',' + digits(ib1) + ',' + digits(ib2);
}

/**
 * @brief Enumerates all unique canonical bank-conflict combinations across three instructions.
 * @return Map from combination key string to a representative triple of bank-read distributions.
 */
std::map<std::string, std::tuple<Bank4, Bank4, Bank4>> buildCombDict() {
    std::map<std::string, std::tuple<Bank4, Bank4, Bank4>> combDict;

    for (const auto& ib0 : insBankList) {
        for (const auto& ib1 : insBankList) {
            for (const auto& ib2 : insBankList) {
                std::string comb = getCombKey(ib0, ib1, ib2);
                combDict.try_emplace(comb, ib0, ib1, ib2);
            }
        }
    }
    return combDict;
}

/**
 * @brief Generates a sequence of FFMA instructions that reproduce a given bank-conflict pattern.
 * @param i0 Bank-read distribution of the first instruction.
 * @param i1 Bank-read distribution of the second instruction.
 * @param i2 Bank-read distribution of the third instruction.
 * @return Lines of generated assembly text, one per instruction.
 */
std::vector<std::string> genBankConflictInsSeq(const Bank4& i0, const Bank4& i1, const Bank4& i2) {
    static const std::array<std::array<int, 3>, 4> reglist = {{{8, 12, 16}, {9, 13, 17}, {10, 14, 18}, {11, 15, 19}}};

    std::array<Bank4, 6> ilist = {i0, i1, i2, i0, i1, i2};
    std::array<std::string, 6> regout;
    for (int x = 20; x < 26; ++x) regout[x - 20] = "R" + std::to_string(x);

    std::vector<std::string> s;
    for (std::size_t iIns = 0; iIns < ilist.size(); ++iIns) {
        const Bank4& insBank = ilist[iIns];
        std::vector<int> rs;
        for (int ibank = 0; ibank < 4; ++ibank) {
            const auto& rin = reglist[ibank];
            int count = insBank[ibank];
            for (int k = 0; k < count; ++k) rs.push_back(rin[k]);
        }
        while (rs.size() < 3) rs.push_back(rs.back());

        std::string regin;
        for (std::size_t k = 0; k < rs.size(); ++k) {
            if (k > 0) regin += ", ";
            regin += "R" + std::to_string(rs[k]);
        }

        s.push_back("FFMA " + regout[iIns] + ", " + regin + ";\n");
    }

    return s;
}
