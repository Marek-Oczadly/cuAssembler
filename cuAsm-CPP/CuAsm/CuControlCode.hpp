#pragma once

#include <cstdint>
#include <set>
#include <string>

namespace CuAsm {

/// Length of a formatted control-code string, e.g. "B--2---:R0:W1:-:S07".
constexpr int c_ControlStringLen = 19;

/**
 * @brief Checks whether a string matches the control-code text format
 *        "B(0|-)(1|-)(2|-)(3|-)(4|-)(5|-):R[0-5-]:W[0-5-]:(Y|-):S\d{2}", mirroring
 *        CuControlCode.c_ControlCodesPattern.
 * @param s String to check.
 * @return True if s matches the control-code format.
 **/
bool matchesControlCodePattern(const std::string& s);

class CuControlCode {
public:
    /**
     * @brief Constructs a control-code object by splitting a raw control code into its fields.
     * @param code Raw control code (waitbar/readbar/writebar/yield/stall packed into one integer).
     **/
    explicit CuControlCode(std::uint32_t code);

    /**
     * @brief Checks whether the yield flag is set.
     * @return True if the yield flag is set (code == 0), false otherwise.
     **/
    bool isYield() const;

    /**
     * @brief Gets the stall count.
     * @return The stall count.
     **/
    std::uint32_t getStallCount() const;

    /**
     * @brief Gets the read scoreboard id.
     * @return The read scoreboard id, or -1 if not set.
     **/
    int getReadSB() const;

    /**
     * @brief Gets the write scoreboard id.
     * @return The write scoreboard id, or -1 if not set.
     **/
    int getWriteSB() const;

    /**
     * @brief Gets the set of scoreboards this instruction waits on.
     * @return Set of waiting scoreboard indices; empty if waiting on none.
     **/
    std::set<int> getBarrierSet() const;

    /**
     * @brief Splits a raw control code into its (waitbar, readbar, writebar, yield, stall) parts.
     * @param code Raw control code to split.
     * @param waitbar Output wait-on-barrier bitmask.
     * @param readbar Output read dependency barrier.
     * @param writebar Output write dependency barrier.
     * @param yieldFlag Output yield flag.
     * @param stall Output stall count.
     **/
    static void splitCode(std::uint32_t code, std::uint32_t& waitbar, std::uint32_t& readbar,
                          std::uint32_t& writebar, std::uint32_t& yieldFlag, std::uint32_t& stall);

    /**
     * @brief Splits a raw control code into its (waitbar, readbar, writebar, ystall) parts, with
     *        yield/stall combined, mirroring the original splitCode2().
     * @param code Raw control code to split.
     * @param waitbar Output wait-on-barrier bitmask.
     * @param readbar Output read dependency barrier.
     * @param writebar Output write dependency barrier.
     * @param ystall Output combined yield/stall field.
     **/
    static void splitCode2(std::uint32_t code, std::uint32_t& waitbar, std::uint32_t& readbar,
                           std::uint32_t& writebar, std::uint32_t& ystall);

    /**
     * @brief Merges control-code fields back into a single raw control code.
     * @param waitbar Wait-on-barrier bitmask.
     * @param readbar Read dependency barrier.
     * @param writebar Write dependency barrier.
     * @param yieldFlag Yield flag.
     * @param stall Stall count.
     * @return The merged raw control code.
     **/
    static std::uint32_t mergeCode(std::uint32_t waitbar, std::uint32_t readbar, std::uint32_t writebar,
                                    std::uint32_t yieldFlag, std::uint32_t stall);

    /**
     * @brief Decodes a raw control code into its human-readable string form, e.g. "B--2---:R0:W1:-:S07".
     * @param code Raw control code to decode.
     * @return The decoded control-code string.
     **/
    static std::string decode(std::uint32_t code);

    /**
     * @brief Encodes a human-readable control-code string back into its raw form.
     * @param s Control-code string to encode, matching the format produced by decode().
     * @return The encoded raw control code.
     **/
    static std::uint32_t encode(const std::string& s);

private:
    std::uint32_t m_Barrier;
    std::uint32_t m_Read;
    std::uint32_t m_Write;
    std::uint32_t m_Yield;
    std::uint32_t m_Stall;
};

} // namespace CuAsm
