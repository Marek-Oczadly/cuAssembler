#pragma once

#include <array>
#include <map>
#include <string>
#include <tuple>
#include <vector>

using Bank4 = std::array<int, 4>;

/**
 * @brief Enumerates all unique canonical bank-conflict combinations across three instructions.
 * @return Map from combination key string to a representative triple of bank-read distributions.
 */
std::map<std::string, std::tuple<Bank4, Bank4, Bank4>> buildCombDict();

/**
 * @brief Concatenates each bank-read distribution's digits and joins the three with commas.
 * @param ib0 Bank-read distribution of the first instruction.
 * @param ib1 Bank-read distribution of the second instruction.
 * @param ib2 Bank-read distribution of the third instruction.
 * @return Comma-separated digit strings, e.g. "3000,0300,0030".
 */
std::string getCombStr(const Bank4& ib0, const Bank4& ib1, const Bank4& ib2);

/**
 * @brief Generates a sequence of FFMA instructions that reproduce a given bank-conflict pattern.
 * @param i0 Bank-read distribution of the first instruction.
 * @param i1 Bank-read distribution of the second instruction.
 * @param i2 Bank-read distribution of the third instruction.
 * @return Lines of generated assembly text, one per instruction.
 */
std::vector<std::string> genBankConflictInsSeq(const Bank4& i0, const Bank4& i1, const Bank4& i2);
