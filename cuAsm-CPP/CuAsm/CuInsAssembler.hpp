#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "CuSMVersion.hpp"
#include "utils/BigNum.hpp"
#include "utils/RationalMatrix.hpp"

namespace CuAsm {

/// Value list for one observed instruction instance, e.g. register indices/immediates decoded
/// from its operands. BigInt is used since some fields (e.g. large immediates) can exceed 64 bits.
using InsVals = std::vector<BigInt>;

/// Modifier list for one observed instruction instance, e.g. {"0_MOV"}.
using InsModi = std::vector<std::string>;

/// Ordered set of every modifier seen so far; a modifier's position is its assigned column index,
/// mirroring the insertion-ordered dict CuInsAssembler.m_InsModiSet relies on.
using ModiSet = std::vector<std::string>;

/// One (vals, modi, code) sample kept in CuInsAssembler.m_InsRepos.
struct InsReposEntry {
    InsVals vals;
    InsModi modi;
    BigInt code;
};

/// One (addr, code, asmText) record, mirroring the "ins_info" tuples passed into push() and kept
/// in m_InsRecords/m_ErrRecords.
struct InsInfo {
    std::uint64_t addr = 0;
    BigInt code;
    std::string asmText;
};

/// One (addr, code, asmText, ctrl) record yielded by recordsFeeder(); ctrl is always 0, mirroring
/// the trailing 0 appended by CuInsAssembler.recordsFeeder.
struct InsFeederRecord {
    std::uint64_t addr = 0;
    BigInt code;
    std::string asmText;
    int ctrl = 0;
};

/// Full state needed to reconstruct a CuInsAssembler, mirroring the `d` dict accepted by
/// CuInsAssembler.__init__/initFromDict (as would be produced by parsing a saved repos file).
struct CuInsAssemblerState {
    std::string insKey;
    std::vector<InsReposEntry> insRepos;
    ModiSet insModiSet;
    RationalMatrix valMatrix;
    RationalMatrix pSol;
    BigInt pSolFac;
    RationalMatrix valNullMat;
    RationalMatrix rhs;
    std::vector<InsInfo> insRecords;
    std::map<BigInt, InsInfo> errRecords;
    CuSMVersion arch;
};

/**
 * @brief CuInsAssembler handles the values and weights of one type of instruction, mirroring
 *        CuAsm.CuInsAssembler.CuInsAssembler. Given enough observed (vals, modi, code) samples, it
 *        solves an exact linear system expressing each instruction bit as an integer combination
 *        of value/modifier fields, then uses that solution to assemble new instructions.
 **/
class CuInsAssembler {
public:
    /**
     * @brief Constructs a fresh (empty) assembler for one instruction key.
     * @param insKey Instruction key this assembler handles, e.g. "MOV_R_cAI".
     * @param arch Arch string (e.g. "sm_75") this assembler's codes belong to.
     **/
    explicit CuInsAssembler(const std::string& insKey, const std::string& arch = "sm_75");

    /**
     * @brief Reconstructs an assembler from a previously saved state, mirroring
     *        CuInsAssembler(inskey, d) / initFromDict.
     * @param state Full state to initialize from.
     **/
    explicit CuInsAssembler(const CuInsAssemblerState& state);

    /**
     * @brief Overwrites all state from a previously saved state, mirroring
     *        CuInsAssembler.initFromDict.
     * @param state Full state to initialize from.
     **/
    void initFromDict(const CuInsAssemblerState& state);

    /**
     * @brief No-op placeholder, mirroring CuInsAssembler.initFromJsonDict (`pass` in the original).
     **/
    void initFromJsonDict();

    /**
     * @brief Gets every record (normal and error), mirroring CuInsAssembler.iterRecords.
     * @return All entries of m_InsRecords followed by all entries of m_ErrRecords.
     **/
    std::vector<InsInfo> iterRecords() const;

    /**
     * @brief Gets every record as a (addr, code, asm, ctrl=0) feeder-style record, mirroring
     *        CuInsAssembler.recordsFeeder.
     * @return All records from iterRecords(), each with a trailing ctrl=0 field appended.
     **/
    std::vector<InsFeederRecord> recordsFeeder() const;

    /**
     * @brief Pushes in new modifiers not yet seen, mirroring CuInsAssembler.expandModiSet. The
     *        order modifiers are first seen in matters, since each gets its own column index.
     * @param modi Modifiers of the instruction instance being considered.
     * @return True if any new modifier was added.
     **/
    bool expandModiSet(const InsModi& modi);

    /**
     * @brief Builds the raw (modi-flags then vals) value vector for one instruction instance,
     *        mirroring CuInsAssembler.buildInsValVec(outRawList=True).
     * @param vals Value list of the instruction instance.
     * @param modi Modifier list of the instruction instance.
     * @return The raw value vector, length m_InsModiSet.size() + vals.size().
     **/
    std::vector<BigRational> buildInsValVec(const InsVals& vals, const InsModi& modi) const;

    /**
     * @brief Builds the value vector for one instruction instance as a column matrix, mirroring
     *        CuInsAssembler.buildInsValVec(outRawList=False).
     * @param vals Value list of the instruction instance.
     * @param modi Modifier list of the instruction instance.
     * @return The value vector as a (n, 1) column matrix.
     **/
    RationalMatrix buildInsValVecMatrix(const InsVals& vals, const InsModi& modi) const;

    /// Result of canAssemble(): brief is unset if the instruction can be assembled with current
    /// info, else one of "NewModi"/"NewVals" with info giving details, mirroring the (brief, info)
    /// tuple canAssemble() returns.
    struct AssembleCheck {
        std::optional<std::string> brief;
        std::string info;
    };

    /**
     * @brief Checks whether the input code can be assembled with current info, mirroring
     *        CuInsAssembler.canAssemble.
     * @param vals Value list in integers.
     * @param modi Modifier list.
     * @return Whether the instruction can be assembled, and why not if it can't.
     **/
    AssembleCheck canAssemble(const InsVals& vals, const InsModi& modi) const;

    /// Result of push(): flag is true for an expected outcome ("NewModi"/"NewVals"/"Verified"),
    /// false for an unexpected one ("NewConflict"/"KnownConflict"), mirroring the (flag, info)
    /// tuple push() returns.
    struct PushResult {
        bool flag;
        std::string info;
    };

    /**
     * @brief Pushes in a new instruction instance, verifying it if it can already be assembled,
     *        else recording it as new information, mirroring CuInsAssembler.push.
     * @param vals Value list in integers.
     * @param modi Modifier list.
     * @param code The instruction's actual encoded code.
     * @param insInfo (addr, code, asmText) info for this instance, recorded for later inspection.
     * @return Whether the push matched expectations, and a brief classification of the outcome.
     **/
    PushResult push(const InsVals& vals, const InsModi& modi, const BigInt& code, const InsInfo& insInfo);

    /**
     * @brief Assembles an instruction code from its vals/modi using the current solution,
     *        mirroring CuInsAssembler.buildCode. Does not check whether the matrix is sufficient.
     * @param vals Value list in integers.
     * @param modi Modifier list.
     * @return The assembled instruction code.
     **/
    BigInt buildCode(const InsVals& vals, const InsModi& modi) const;

    /**
     * @brief Rebuilds m_ValMatrix/m_Rhs/m_ValNullMat/m_PSol/m_PSolFac from m_InsRepos, mirroring
     *        CuInsAssembler.buildMatrix. A no-op if m_InsRepos is empty.
     **/
    void buildMatrix();

    /**
     * @brief Tries to solve every variable and prints the result, mirroring CuInsAssembler.solve.
     *        Only possible when m_ValNullMat is empty (fully determined system).
     * @return The solved column matrix, or std::nullopt if not solvable.
     **/
    std::optional<RationalMatrix> solve() const;

    /**
     * @brief Prints the current solution (value/modifier name next to its coefficient), mirroring
     *        CuInsAssembler.printSolution.
     **/
    void printSolution() const;

    /**
     * @brief Builds a sympy-Matrix-literal-style text dump of m_PSol, mirroring
     *        CuInsAssembler.reprPSol.
     * @return The formatted "Matrix([...])" text.
     **/
    std::string reprPSol() const;

    /**
     * @brief Builds a text dump of this assembler suitable for saving to (and later reloading
     *        from) a repos file, mirroring CuInsAssembler.__repr__.
     * @return The formatted "CuInsAssembler(...)" text.
     **/
    std::string repr() const;

    /**
     * @brief Builds a short display string, mirroring CuInsAssembler.__str__.
     * @return "CuInsAssembler(<InsKey>)".
     **/
    std::string toString() const;

    std::string m_InsKey;
    std::vector<InsReposEntry> m_InsRepos;
    ModiSet m_InsModiSet;

    RationalMatrix m_ValMatrix;
    RationalMatrix m_PSol;
    BigInt m_PSolFac{1};
    RationalMatrix m_ValNullMat;
    RationalMatrix m_Rhs;

    std::vector<InsInfo> m_InsRecords;
    std::map<BigInt, InsInfo> m_ErrRecords;

    CuSMVersion m_Arch;
};

} // namespace CuAsm
