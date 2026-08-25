#include <stdexcept>
#include <string>
#include <vector>

#include "../CuAsm/OperandRole.hpp"
#include "utils/TestUtilsCommon.hpp"

using CuAsm::AccessMode;
using CuAsm::OperandKind;
using CuAsm::OperandRoleTable;

/**
 * @brief Exercises CuAsm::OperandRoleTable, the curated operand read/write role table Reports/
 *        tasks.md Phase 0 defines -- its IOInfo text-format grammar (base roles, comments,
 *        modifier-keyed overrides), its "lookup miss is a hard error" contract, and the
 *        default/static table loaders against the real shipped IOInfo.sm_75.txt.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    // ---- toString/parse round trips for a representative sample of each enum ----

    t.checkEqual("toString(AccessMode::READ_WRITE) matches the IOInfo token", CuAsm::toString(AccessMode::READ_WRITE),
                 std::string("READ_WRITE"));
    t.check("parseAccessMode round-trips every AccessMode enumerator",
            CuAsm::parseAccessMode("READ") == AccessMode::READ && CuAsm::parseAccessMode("WRITE") == AccessMode::WRITE &&
                CuAsm::parseAccessMode("READ_WRITE") == AccessMode::READ_WRITE);
    t.checkThrows<std::invalid_argument>("parseAccessMode rejects an unknown token", [] { (void)CuAsm::parseAccessMode("BOGUS"); });

    t.checkEqual("toString(OperandKind::R_ADDR) matches CuInsParser's own \"R.Addr\" tag string",
                 CuAsm::toString(OperandKind::R_ADDR), std::string("R.Addr"));
    t.check("parseOperandKind round-trips a sample of CuInsParser's tag vocabulary",
            CuAsm::parseOperandKind("R") == OperandKind::GPR && CuAsm::parseOperandKind("UR") == OperandKind::UGPR &&
                CuAsm::parseOperandKind("Imme.CBank") == OperandKind::CBANK_IMME &&
                CuAsm::parseOperandKind("UR.Desc") == OperandKind::UR_DESC);
    t.checkThrows<std::invalid_argument>("parseOperandKind rejects an unknown tag", [] { (void)CuAsm::parseOperandKind("Bogus"); });

    // ---- parse(): base roles, comments/blank lines, and modifier overrides ----

    OperandRoleTable table;
    t.checkNoThrow("parse() accepts a well-formed IOInfo document", [&] {
        table.parse(R"(
# a whole-line comment, and a blank line above/below should both be ignored

FFMA_R_R_R_R: R:WRITE R:READ R:READ R:READ   # trailing comment on a base-role line

MOV_R_R: R:WRITE R:READ
    @0_64: R:WRITE R:READ   # override: ".64" widens the destination to a register pair
)");
    });
    t.checkEqual("size() counts distinct InsKey entries, not override lines", table.size(), std::size_t{2});
    t.check("contains() is true for a curated key and false for an uncurated one", table.contains("FFMA_R_R_R_R") && !table.contains("IADD3_R_R_R_R"));

    const auto& ffmaRoles = table.lookup("FFMA_R_R_R_R", {});
    t.check("lookup() returns FFMA_R_R_R_R's base roles in file order: one WRITE dest, three READ sources",
            ffmaRoles.size() == 4 && ffmaRoles[0].kind == OperandKind::GPR && ffmaRoles[0].mode == AccessMode::WRITE &&
                ffmaRoles[1].mode == AccessMode::READ && ffmaRoles[2].mode == AccessMode::READ && ffmaRoles[3].mode == AccessMode::READ);

    const auto& movBase = table.lookup("MOV_R_R", {"0_MOV"});
    t.check("lookup() falls back to base roles when insModi doesn't match any override's modifier",
            movBase.size() == 2 && movBase[0].mode == AccessMode::WRITE && movBase[1].mode == AccessMode::READ);

    const auto& movOverride = table.lookup("MOV_R_R", {"0_MOV", "0_64"});
    t.check("lookup() applies the matching modifier override instead of the base roles",
            movOverride.size() == 2 && movOverride[0].kind == OperandKind::GPR && movOverride[0].mode == AccessMode::WRITE);

    // ---- lookup() miss is a hard error, matching the shuffler's reject-rather-than-guess posture ----

    t.checkThrows<std::out_of_range>("lookup() on an uncurated InsKey throws std::out_of_range rather than guessing",
                                      [&] { (void)table.lookup("NOT_A_REAL_KEY", {}); });

    // ---- malformed documents ----

    t.checkThrows<std::runtime_error>("parse() rejects an InsKey line with no ':'",
                                       [] { OperandRoleTable bad; bad.parse("FFMA_R_R_R_R R:WRITE"); });
    t.checkThrows<std::runtime_error>("parse() rejects an unknown operand-kind token",
                                       [] { OperandRoleTable bad; bad.parse("MOV_R_R: BOGUS:WRITE R:READ"); });
    t.checkThrows<std::runtime_error>("parse() rejects an unknown access-mode token",
                                       [] { OperandRoleTable bad; bad.parse("MOV_R_R: R:BOGUS R:READ"); });
    t.checkThrows<std::runtime_error>("parse() rejects an override line before any InsKey line",
                                       [] { OperandRoleTable bad; bad.parse("    @0_64: R:WRITE"); });

    // ---- default/static table loading against the real shipped IOInfo.sm_75.txt ----

    const OperandRoleTable defaultTable = OperandRoleTable::getDefaultTable(75);
    t.check("getDefaultTable(75) loads the shipped curated draft (non-empty)", defaultTable.size() > 0);
    t.checkNoThrow("a known arithmetic/logic InsKey curated by the Phase 0 draft resolves without throwing",
                    [&] { (void)defaultTable.lookup("MOV_R_R", {"0_MOV"}); });

    const OperandRoleTable emptyTable = OperandRoleTable::getDefaultTable(999999);
    t.check("getDefaultTable() on a version with no curated (or aliased) file falls back to an empty table instead of throwing",
            emptyTable.size() == 0);

    t.check("getStaticTable() returns the same shared instance across repeated calls",
            &OperandRoleTable::getStaticTable(75) == &OperandRoleTable::getStaticTable(75));

    return t.finish("test_OperandRole");
}
