#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace CuAsm {

class CuInsParser {
public:
    /**
     * @brief Constructs a parser bound to a particular arch.
     * @param arch Arch string (e.g. "sm_75"), valid as input to CuSMVersion.
     **/
    explicit CuInsParser(const std::string& arch = "sm_75");

    /**
     * @brief Parses an instruction asm string into its key, operand values, and modifiers.
     * @param s Instruction asm string to parse.
     * @param addr Address of the instruction, needed by branch-type instructions.
     * @param code Instruction code, recorded for later inspection via dumpInfo().
     * @return Tuple of (insKey, insVals, insModifiers).
     **/
    std::tuple<std::string, std::vector<std::uint64_t>, std::vector<std::string>>
    parse(const std::string& s, std::uint64_t addr = 0, std::uint64_t code = 0);

    /**
     * @brief Prints the parser's internal state for the most recently parsed instruction to stdout.
     **/
    void dumpInfo() const;
};

} // namespace CuAsm
