#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "utils/OrderedMap.hpp"

namespace CuAsm {

/**
 * @brief Whether an instruction's completion latency is fixed by its execution pipe (a known
 *        cycle count, safely covered by a plain stall count) or variable (data-/memory-system-
 *        dependent, and therefore tracked with a scoreboard barrier instead). This is
 *        Reports/tasks.md Phase 1's "mostly boolean fixed/variable" classification, and the
 *        Reports/control-codes-validation.md component it implements: "Determines whether a
 *        dependency needs a scoreboard wait at all, or is safely covered by a fixed stall count."
 **/
enum class LatencyKind {
    FIXED,
    VARIABLE,
};

/**
 * @brief Formats a LatencyKind as the token the LatencyClass text format uses for it.
 * @param kind Value to format.
 * @return "FIXED" or "VARIABLE".
 **/
std::string toString(LatencyKind kind);

/**
 * @brief Parses toString(LatencyKind)'s output back into a LatencyKind.
 * @param s Token to parse ("FIXED"/"VARIABLE").
 * @return The parsed kind.
 * @throws std::invalid_argument if s is neither token.
 **/
LatencyKind parseLatencyKind(const std::string& s);

/**
 * @brief Which scoreboard barrier kind (CuControlCode::getReadSB()/getWriteSB()) a
 *        LatencyKind::VARIABLE instruction sets when it issues. WRITE protects a destination
 *        operand that is written asynchronously -- consumers must wait on the barrier before
 *        reading it (the load/RAW case). READ protects a source operand this instruction is
 *        still consuming asynchronously -- later instructions must wait before overwriting it
 *        (the store/WAR case). Meaningless for LatencyKind::FIXED (see LatencyClassEntry).
 **/
enum class BarrierType {
    READ,
    WRITE,
};

/**
 * @brief Formats a BarrierType as the token the LatencyClass text format uses for it.
 * @param type Value to format.
 * @return "READ" or "WRITE".
 **/
std::string toString(BarrierType type);

/**
 * @brief Parses toString(BarrierType)'s output back into a BarrierType.
 * @param s Token to parse ("READ"/"WRITE").
 * @return The parsed type.
 * @throws std::invalid_argument if s is neither token.
 **/
BarrierType parseBarrierType(const std::string& s);

/**
 * @brief One InsKey's curated latency classification. FIXED instructions never set a barrier
 *        (barrier is std::nullopt); VARIABLE instructions always set exactly one of the two
 *        barrier kinds (barrier always has a value) -- real hardware never needs both a read and
 *        a write barrier for the same instruction, since an operand slot is never simultaneously
 *        curated as both the asynchronously-written result and the asynchronously-consumed
 *        source of the same variable-latency op.
 **/
struct LatencyClassEntry {
    LatencyKind kind;
    std::optional<BarrierType> barrier;
};

/**
 * @brief Curated per-instruction-key latency/pipe classification table -- Reports/tasks.md
 *        Phase 1's deliverable, and the second of the two data tables
 *        Reports/control-codes-validation.md identifies as required before any hazard analysis
 *        can run (the first being OperandRoleTable, Phase 0). Where OperandRoleTable answers
 *        "what does this operand slot do", this table answers "how long does this instruction
 *        take to retire, and if that's data-dependent, which barrier kind guards it".
 *
 *        Loaded once from an external data file, exactly like OperandRoleTable (see
 *        Config::getDefaultLatencyClassFile()) -- scoping the table down to only the opcodes
 *        actually seen in target kernels is just a matter of not adding rows, matching
 *        Reports/tasks.md Phase 1's "same external-data-file/loader pattern and scoping as the
 *        role table". Unlike OperandRoleTable, no modifier-keyed overrides exist here: which
 *        execution pipe an opcode issues to (and therefore its FIXED/VARIABLE class and barrier
 *        kind) does not vary by width/tile-shape modifier the way operand *shape* does, so there
 *        is nothing for an override to key off yet -- see Scripts/gen_io_info.py's latency-draft
 *        pass for the concrete cases this was checked against.
 *
 *        A lookup miss on an opcode actually encountered during verification/correction is a
 *        hard error, matching OperandRoleTable::lookup()'s posture and the shuffler's
 *        reject-rather-than-guess design -- lookup() enforces this itself by throwing rather than
 *        returning a best-effort guess.
 **/
class LatencyClassTable {
public:
    /// Constructs an empty table.
    LatencyClassTable() = default;

    /**
     * @brief Loads a table from a previously curated LatencyClass text file, mirroring
     *        OperandRoleTable::initFromFile.
     * @param fileName Path of the LatencyClass file to load (see
     *        Config::getDefaultLatencyClassFile()).
     * @throws std::runtime_error if the file cannot be opened, or on malformed content (see
     *         parse() for the exact grammar).
     **/
    void initFromFile(const std::string& fileName);

    /**
     * @brief Parses LatencyClass file text directly -- initFromFile()'s implementation, exposed
     *        separately so tests can exercise the grammar without a file on disk. Replaces any
     *        entry already in this table for a repeated InsKey (later entries win), but does not
     *        clear entries from prior parse()/initFromFile() calls -- call on an empty table to
     *        load exactly one file's contents.
     *
     *        Grammar, one InsKey per line, `#` starts a trailing (or whole-line) comment:
     *        @code
     *        InsKey: FIXED
     *        InsKey: VARIABLE:READ
     *        InsKey: VARIABLE:WRITE
     *        @endcode
     *        Everything before the first `:` is the InsKey; everything after is exactly one
     *        token, either the literal `FIXED`, or `VARIABLE:READ`/`VARIABLE:WRITE`.
     * @param text File contents to parse.
     * @throws std::runtime_error on malformed text (bad line shape, unknown kind/barrier token,
     *         or a `VARIABLE` line missing its `:READ`/`:WRITE` suffix).
     **/
    void parse(const std::string& text);

    /**
     * @brief Looks up the curated latency classification for one InsKey.
     * @param insKey Instruction key, e.g. "FFMA_R_R_R_R" (CuInsParser::parse()'s first return
     *        value).
     * @return The applicable latency classification.
     * @throws std::out_of_range if insKey has no entry in this table -- by design (see class
     *         doc): callers must treat an unrecognized opcode as a hard verification/correction
     *         error, never as "assume fixed latency", so this never returns a best-effort guess.
     **/
    const LatencyClassEntry& lookup(const std::string& insKey) const;

    /** @brief Whether insKey has a curated entry, without throwing. */
    bool contains(const std::string& insKey) const;

    /** @brief Number of curated InsKey entries. */
    std::size_t size() const;

    /**
     * @brief Builds a default table for an SM version, loading its curated LatencyClass file
     *        (falling back to an aliased arch's file, or an empty table, if none exists yet),
     *        mirroring OperandRoleTable::getDefaultTable / CuInsAssemblerRepos::getDefaultRepos.
     *        An empty table is a legitimate (if unhelpful) result: every lookup() on it throws,
     *        which is the same hard-error behavior a table missing just one opcode would give.
     * @param versionNumber SM version number, e.g. 75 for sm_75.
     * @return The constructed table.
     **/
    static LatencyClassTable getDefaultTable(int versionNumber);

    /**
     * @brief Gets a process-wide shared default table for an SM version, constructing it on first
     *        use, mirroring OperandRoleTable::getStaticTable.
     * @param versionNumber SM version number, e.g. 75 for sm_75.
     * @return Reference to the shared table instance for versionNumber.
     **/
    static LatencyClassTable& getStaticTable(int versionNumber);

private:
    OrderedMap<std::string, LatencyClassEntry> m_Table;
};

} // namespace CuAsm
