#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "utils/OrderedMap.hpp"

namespace CuAsm {

/**
 * @brief Whether an operand slot is read, written, or both by its instruction, mirroring the
 *        producer/consumer roles a hazard/scoreboard simulator (Reports/tasks.md Phase 3) needs
 *        per operand.
 **/
enum class AccessMode {
    READ,
    WRITE,
    READ_WRITE,
};

/**
 * @brief Formats an AccessMode as the token the IOInfo text format uses for it.
 * @param mode Value to format.
 * @return "READ", "WRITE", or "READ_WRITE".
 **/
std::string toString(AccessMode mode);

/**
 * @brief Parses toString(AccessMode)'s output back into an AccessMode.
 * @param s Token to parse ("READ"/"WRITE"/"READ_WRITE").
 * @return The parsed mode.
 * @throws std::invalid_argument if s is none of those tokens.
 **/
AccessMode parseAccessMode(const std::string& s);

/**
 * @brief Operand-slot kind, mirroring the tag vocabulary CuInsParser::m_InsTags actually produces
 *        (CuInsParser.cpp's OperandParse::tag / IndexedToken label assignments) -- one enumerator
 *        per distinct tag string CuInsParser can emit for a "real" operand slot. The always-present
 *        guard-predicate slot at insTags[0]/insVals[0] ("P.Guard") is deliberately not included
 *        here: every instruction carries it as an implicit read, so it is not something a curated
 *        per-InsKey table needs to describe (see OperandRoleTable::lookup()).
 **/
enum class OperandKind {
    GPR,        ///< "R"          -- general-purpose register.
    UGPR,       ///< "UR"         -- uniform register.
    PRED,       ///< "P"          -- predicate register.
    UPRED,      ///< "UP"         -- uniform predicate register.
    BARRIER,    ///< "B"          -- named-barrier index operand (e.g. BAR.SYNC).
    SBREG,      ///< "SB"         -- indexed scoreboard-register token.
    SBSET,      ///< "SBSET"      -- scoreboard-set literal (source "{0,1}" translated to SBSETn).
    SREG,       ///< "SR"         -- special register (S2R/S2UR's SR_* operand).
    INT_IMME,   ///< "II"         -- integer immediate.
    FLOAT_IMME, ///< "FI"         -- float immediate.
    R_ADDR,     ///< "R.Addr"     -- register component of a [Rn+...] memory address.
    UR_ADDR,    ///< "UR.Addr"    -- uniform-register component of a memory address.
    IMME_ADDR,  ///< "Imme.Addr"  -- immediate-offset component of a memory address.
    CBANK_IMME, ///< "Imme.CBank" -- constant-bank index of a c[bank][addr] operand.
    UR_CBANK,   ///< "UR.CBank"   -- uniform-register bank index of a cx[URn][addr] operand.
    UR_DESC,    ///< "UR.Desc"    -- uniform-register descriptor index of a desc[URn][addr] operand.
};

/**
 * @brief Formats an OperandKind as the exact CuInsParser tag string it mirrors.
 * @param kind Value to format.
 * @return e.g. "R" for OperandKind::GPR, "R.Addr" for OperandKind::R_ADDR.
 **/
std::string toString(OperandKind kind);

/**
 * @brief Parses a CuInsParser tag string back into an OperandKind.
 * @param tag Tag string to parse, e.g. "R" or "R.Addr".
 * @return The parsed kind.
 * @throws std::invalid_argument if tag matches none of CuInsParser's known tag strings.
 **/
OperandKind parseOperandKind(const std::string& tag);

/// One operand slot's curated role: what kind of value it holds, and whether the instruction
/// reads, writes, or both through it.
struct OperandRoleEntry {
    OperandKind kind;
    AccessMode mode;
};

/**
 * @brief A modifier-keyed override, applied instead of an InsKey's base roles when a given
 *        instance's insModi contains `modifier` verbatim -- e.g. a ".64" width suffix (recorded as
 *        "0_64" in insModi, see CuInsParser::parse()) that turns a single-register destination
 *        operand into a register pair. Reports/tasks.md Phase 0 scopes these overrides to only
 *        the cases where a modifier changes operand *width*, not a full entry per modifier combo.
 **/
struct OperandRoleOverride {
    std::string modifier;
    std::vector<OperandRoleEntry> roles;
};

/// Every role fact curated for one InsKey: its base (default) per-operand roles, plus any
/// modifier-keyed overrides, checked in file order, before falling back to the base roles.
struct OperandRoleRecord {
    std::vector<OperandRoleEntry> roles;
    std::vector<OperandRoleOverride> overrides;
};

/**
 * @brief Curated per-instruction-key operand read/write role table -- the "opcode -> per-operand
 *        read/write role" data Reports/control-codes-validation.md identifies as the largest cost
 *        driver of the control-code verifier/corrector, and Reports/tasks.md Phase 0's deliverable.
 *        `CuInsParser::parse()` only produces positional/syntactic data (insKey/insVals/insModi);
 *        this table is what tags a given InsKey's operand slots as source vs. destination.
 *
 *        Loaded once from an external data file (mirroring CuInsAssemblerRepos's
 *        DefaultInsAsmRepos.sm_<N>.txt loading pattern, via Config::getDefaultIOInfoFile()) rather
 *        than compiled in, so scoping the table down to only the opcodes actually seen in target
 *        kernels is just a matter of not adding rows, and curating more opcodes later never needs
 *        a rebuild. See Scripts/gen_io_info.py for the offline curation script that seeds this file
 *        from a DefaultInsAsmRepos.sm_<N>.txt's recorded instruction samples.
 *
 *        A lookup miss on an opcode actually encountered during verification/correction is meant
 *        to be a hard error, matching the shuffler's reject-rather-than-guess posture (see
 *        Reports/tasks.md Phase 0's checklist) -- lookup() enforces that itself by throwing rather
 *        than returning an empty/best-effort role list.
 **/
class OperandRoleTable {
public:
    /// Constructs an empty table.
    OperandRoleTable() = default;

    /**
     * @brief Loads a table from a previously curated IOInfo text file, mirroring
     *        CuInsAssemblerRepos::initFromFile's load-from-external-file pattern.
     * @param fileName Path of the IOInfo file to load (see Config::getDefaultIOInfoFile()).
     * @throws std::runtime_error if the file cannot be opened, or on malformed content (see
     *         parse() for the exact grammar).
     **/
    void initFromFile(const std::string& fileName);

    /**
     * @brief Parses IOInfo file text directly -- initFromFile()'s implementation, exposed
     *        separately so tests can exercise the grammar without a file on disk. Replaces any
     *        entries already in this table for a repeated InsKey (later entries win), but does not
     *        clear entries from prior parse()/initFromFile() calls -- call on an empty table to
     *        load exactly one file's contents.
     *
     *        Grammar, line-oriented, `#` starts a trailing (or whole-line) comment:
     *        @code
     *        InsKey: kind:mode kind:mode ...
     *            @modifier: kind:mode kind:mode ...
     *        @endcode
     *        An unindented line starts a new InsKey entry: everything before the first `:` is the
     *        InsKey, the rest is a whitespace-separated list of `kind:mode` tokens giving that
     *        key's base roles, one per real operand slot in CuInsVals/CuInsTags order (i.e.
     *        excluding the guard-predicate slot every instruction carries at index 0). A line
     *        indented with leading whitespace is an override for the immediately preceding InsKey:
     *        `@modifier` names one exact element of that instance's insModi (see
     *        OperandRoleOverride), followed by the same `kind:mode ...` role-list grammar.
     * @param text File contents to parse.
     * @throws std::runtime_error on malformed text (bad line shape, unknown kind/mode token, or an
     *         override line before any InsKey line).
     **/
    void parse(const std::string& text);

    /**
     * @brief Looks up the per-operand roles for one decoded instruction instance, applying any
     *        matching modifier override before falling back to insKey's base roles.
     * @param insKey Instruction key, e.g. "FFMA_R_R_R_R" (CuInsParser::parse()'s first return value).
     * @param insModi Modifier list for this instance (CuInsParser::parse()'s third return value);
     *        only consulted to match OperandRoleOverride::modifier entries.
     * @return The applicable per-operand roles, aligned 1:1 with insVals[CuInsParser::OPERAND_VAL_IDX ..]
     *         / insTags[CuInsParser::OPERAND_VAL_IDX ..] for this instance -- i.e. excluding the
     *         always-present P.Guard slot at index 0.
     * @throws std::out_of_range if insKey has no entry in this table -- by design (see class doc):
     *         callers must treat an unrecognized opcode as a hard verification/correction error,
     *         never as "no hazard", so this never returns an empty/best-effort guess.
     **/
    const std::vector<OperandRoleEntry>& lookup(const std::string& insKey, const std::vector<std::string>& insModi) const;

    /** @brief Whether insKey has a curated entry, without throwing. */
    bool contains(const std::string& insKey) const;

    /** @brief Number of curated InsKey entries. */
    std::size_t size() const;

    /**
     * @brief Builds a default table for an SM version, loading its curated IOInfo file (falling
     *        back to an aliased arch's file, or an empty table, if none exists yet), mirroring
     *        CuInsAssemblerRepos::getDefaultRepos / CuInsAssemblerRepos::setToDefaultInsAsmDict.
     *        An empty table is a legitimate (if unhelpful) result: every lookup() on it throws,
     *        which is the same hard-error behavior a table missing just one opcode would give.
     * @param versionNumber SM version number, e.g. 75 for sm_75.
     * @return The constructed table.
     **/
    static OperandRoleTable getDefaultTable(int versionNumber);

    /**
     * @brief Gets a process-wide shared default table for an SM version, constructing it on first
     *        use, mirroring CuInsAssemblerRepos::getStaticRepos.
     * @param versionNumber SM version number, e.g. 75 for sm_75.
     * @return Reference to the shared table instance for versionNumber.
     **/
    static OperandRoleTable& getStaticTable(int versionNumber);

private:
    OrderedMap<std::string, OperandRoleRecord> m_Table;
};

} // namespace CuAsm
