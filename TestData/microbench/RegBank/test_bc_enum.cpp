#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <tuple>

#include "test_bc_enum.hpp"

/**
 * @brief Writes all discovered combinations to a file, sorted by combination key descending.
 * @param combDict Map from combination key string to the three bank-read distributions.
 * @param fname Output file name.
 */
static void writeComb2File(const std::map<std::string, std::tuple<Bank4, Bank4, Bank4>>& combDict, const std::string& fname = "comb.txt") {
    std::ofstream fout(fname);
    int i = 0;
    for (auto it = combDict.rbegin(); it != combDict.rend(); ++it, ++i) {
        const auto& [i0, i1, i2] = it->second;
        std::string s = getCombStr(i0, i1, i2);
        fout << "Comb " << std::setw(4) << (i + 1) << ": " << it->first << ": " << s << "\n";
    }
}

/**
 * @brief Entry point: builds all bank-conflict combinations, writes them to comb.txt, and prints
 *        the generated instruction sequences for the first 102 combinations.
 * @return Always zero.
 */
int main() {
    auto combDict = buildCombDict();
    std::cout << combDict.size() << " comb keys found!\n";
    writeComb2File(combDict);

    int i = 0;
    for (auto it = combDict.rbegin(); it != combDict.rend(); ++it, ++i) {
        const auto& [i0, i1, i2] = it->second;
        std::string ks = getCombStr(i0, i1, i2);
        auto s = genBankConflictInsSeq(i0, i1, i2);

        std::cout << "#### " << ks << "\n";
        for (const auto& line : s) std::cout << line;
        std::cout << "\n";

        if (i > 100) break;
    }

    return 0;
}
