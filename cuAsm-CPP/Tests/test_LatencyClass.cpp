#include <stdexcept>
#include <string>

#include "../CuAsm/LatencyClass.hpp"
#include "utils/TestUtilsCommon.hpp"

using CuAsm::BarrierType;
using CuAsm::LatencyClassTable;
using CuAsm::LatencyKind;

/**
 * @brief Exercises CuAsm::LatencyClassTable, the curated latency/pipe classification table
 *        Reports/tasks.md Phase 1 defines -- its LatencyClass text-format grammar (FIXED,
 *        VARIABLE:READ, VARIABLE:WRITE, comments), its "lookup miss is a hard error" contract,
 *        and the default/static table loaders against the real shipped LatencyClass.sm_75.txt.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    // ---- toString/parse round trips for every enumerator ----

    t.check("toString(LatencyKind) matches the LatencyClass tokens",
            CuAsm::toString(LatencyKind::FIXED) == "FIXED" && CuAsm::toString(LatencyKind::VARIABLE) == "VARIABLE");
    t.check("parseLatencyKind round-trips both LatencyKind enumerators",
            CuAsm::parseLatencyKind("FIXED") == LatencyKind::FIXED && CuAsm::parseLatencyKind("VARIABLE") == LatencyKind::VARIABLE);
    t.checkThrows<std::invalid_argument>("parseLatencyKind rejects an unknown token", [] { (void)CuAsm::parseLatencyKind("BOGUS"); });

    t.check("toString(BarrierType) matches the LatencyClass tokens",
            CuAsm::toString(BarrierType::READ) == "READ" && CuAsm::toString(BarrierType::WRITE) == "WRITE");
    t.check("parseBarrierType round-trips both BarrierType enumerators",
            CuAsm::parseBarrierType("READ") == BarrierType::READ && CuAsm::parseBarrierType("WRITE") == BarrierType::WRITE);
    t.checkThrows<std::invalid_argument>("parseBarrierType rejects an unknown token", [] { (void)CuAsm::parseBarrierType("BOGUS"); });

    // ---- parse(): FIXED, VARIABLE:READ, VARIABLE:WRITE, comments/blank lines ----

    LatencyClassTable table;
    t.checkNoThrow("parse() accepts a well-formed LatencyClass document", [&] {
        table.parse(R"(
# a whole-line comment, and a blank line above/below should both be ignored

FFMA_R_R_R_R: FIXED   # trailing comment on a FIXED line

LDG_R_R: VARIABLE:WRITE
STG_R_R: VARIABLE:READ
)");
    });
    t.checkEqual("size() counts distinct InsKey entries", table.size(), std::size_t{3});
    t.check("contains() is true for a curated key and false for an uncurated one", table.contains("FFMA_R_R_R_R") && !table.contains("IADD3_R_R_R_R"));

    const auto& ffma = table.lookup("FFMA_R_R_R_R");
    t.check("lookup() returns FIXED with no barrier for an ALU-pipe InsKey",
            ffma.kind == LatencyKind::FIXED && !ffma.barrier.has_value());

    const auto& ldg = table.lookup("LDG_R_R");
    t.check("lookup() returns VARIABLE:WRITE for a load-shaped InsKey",
            ldg.kind == LatencyKind::VARIABLE && ldg.barrier.has_value() && *ldg.barrier == BarrierType::WRITE);

    const auto& stg = table.lookup("STG_R_R");
    t.check("lookup() returns VARIABLE:READ for a store-shaped InsKey",
            stg.kind == LatencyKind::VARIABLE && stg.barrier.has_value() && *stg.barrier == BarrierType::READ);

    // ---- lookup() miss is a hard error, matching OperandRoleTable's reject-rather-than-guess posture ----

    t.checkThrows<std::out_of_range>("lookup() on an uncurated InsKey throws std::out_of_range rather than guessing",
                                      [&] { (void)table.lookup("NOT_A_REAL_KEY"); });

    // ---- malformed documents ----

    t.checkThrows<std::runtime_error>("parse() rejects an InsKey line with no ':'",
                                       [] { LatencyClassTable bad; bad.parse("FFMA_R_R_R_R FIXED"); });
    t.checkThrows<std::runtime_error>("parse() rejects an unknown classification token",
                                       [] { LatencyClassTable bad; bad.parse("MOV_R_R: BOGUS"); });
    t.checkThrows<std::runtime_error>("parse() rejects VARIABLE with an unknown barrier token",
                                       [] { LatencyClassTable bad; bad.parse("LDG_R_R: VARIABLE:BOGUS"); });
    t.checkThrows<std::runtime_error>("parse() rejects VARIABLE with no barrier suffix at all",
                                       [] { LatencyClassTable bad; bad.parse("LDG_R_R: VARIABLE"); });

    // ---- default/static table loading against the real shipped LatencyClass.sm_75.txt ----

    const LatencyClassTable defaultTable = LatencyClassTable::getDefaultTable(75);
    t.check("getDefaultTable(75) loads the shipped curated draft (non-empty)", defaultTable.size() > 0);
    t.checkNoThrow("a known arithmetic/logic InsKey curated by the Phase 1 draft resolves without throwing",
                    [&] { (void)defaultTable.lookup("FFMA_R_R_R_R"); });
    t.check("the shipped draft classifies a known arithmetic/logic InsKey as FIXED",
            defaultTable.lookup("FFMA_R_R_R_R").kind == LatencyKind::FIXED);

    const LatencyClassTable emptyTable = LatencyClassTable::getDefaultTable(999999);
    t.check("getDefaultTable() on a version with no curated (or aliased) file falls back to an empty table instead of throwing",
            emptyTable.size() == 0);

    t.check("getStaticTable() returns the same shared instance across repeated calls",
            &LatencyClassTable::getStaticTable(75) == &LatencyClassTable::getStaticTable(75));

    return t.finish("test_LatencyClass");
}
