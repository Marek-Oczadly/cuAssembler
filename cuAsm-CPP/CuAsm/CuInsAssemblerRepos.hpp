#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "CuInsAssembler.hpp"
#include "CuInsFeeder.hpp"
#include "CuInsParser.hpp"
#include "CuSMVersion.hpp"

namespace CuAsm {

/**
 * @brief A repository of instruction assemblers keyed by instruction key, mirroring
 *        CuAsm.CuInsAssemblerRepos.CuInsAssemblerRepos. Building a workable repos from scratch is
 *        very time consuming and requires a wide range of inputs covering all frequently used
 *        instructions, so a pre-gathered repos is usually loaded from a "DefaultInsAsmRepos.*.txt"
 *        file instead, then incrementally updated.
 **/
class CuInsAssemblerRepos {
public:
    /// Underlying instruction-assembler dict, mirroring m_InsAsmDict. std::map (rather than an
    /// insertion-ordered dict) is used, consistent with the rest of this rewrite (e.g.
    /// CuInsAssembler.m_ErrRecords); this only affects iteration/save2file order, not behavior.
    using InsAsmDict = std::map<std::string, CuInsAssembler>;

    /**
     * @brief Constructs a repos, either empty (optionally for a given arch) or loaded from a
     *        previously saved repos file, mirroring the original's (InsAsmDict=None, arch=None).
     * @param reposFile Optional path of a previously saved repos file to load from; std::nullopt for an empty repos.
     * @param arch Optional arch string (e.g. "sm_75"); std::nullopt leaves it unset.
     **/
    explicit CuInsAssemblerRepos(std::optional<std::string> reposFile = std::nullopt, std::optional<std::string> arch = std::nullopt);

    /**
     * @brief Constructs an empty repos for a given arch, mirroring
     *        CuInsAssemblerRepos(arch=someCuSMVersion).
     * @param arch Arch to configure the (empty) repos for.
     **/
    explicit CuInsAssemblerRepos(const CuSMVersion& arch);

    /**
     * @brief Resets the arch this repos (and its instruction parser) is configured for, mirroring
     *        CuInsAssemblerRepos.resetArch.
     * @param arch New arch, or std::nullopt to leave the repos unconfigured (mirroring arch=None).
     **/
    void resetArch(std::optional<CuSMVersion> arch);

    /**
     * @brief Switches this repos (and every instruction assembler already in it) over to a new
     *        arch, mirroring CuInsAssemblerRepos.convertArch. A no-op if already on that arch.
     * @param arch New arch.
     **/
    void convertArch(const CuSMVersion& arch);

    /**
     * @brief Gets the arch this repos is configured for.
     * @return The repos's SM version.
     **/
    CuSMVersion getSMVersion() const;

    /**
     * @brief Gets the arch this repos is configured for as a lowercase string (e.g. "sm_75"),
     *        mirroring CuInsAssemblerRepos.getArchString.
     * @return The repos's arch string.
     **/
    std::string getArchString() const;

    /**
     * @brief Loads (or falls back to an empty) default instruction-assembler dict for this
     *        repos's arch, mirroring setToDefaultInsAsmDict.
     **/
    void setToDefaultInsAsmDict();

    /**
     * @brief Builds a fresh default repos for an arch, loading its pre-gathered InsAsmRepos file
     *        (falling back to an aliased arch's, or an empty repos), mirroring
     *        CuInsAssemblerRepos.getDefaultRepos.
     * @param arch Arch string to build a default repos for.
     * @return The constructed repos.
     **/
    static CuInsAssemblerRepos getDefaultRepos(const std::string& arch);

    /**
     * @brief Gets a process-wide shared default repos for arch, constructing it on first use,
     *        mirroring CuInsAssemblerRepos.getStaticRepos.
     * @param arch Arch string to get the shared repos for.
     * @return Reference to the shared repos instance for arch.
     **/
    static CuInsAssemblerRepos& getStaticRepos(const std::string& arch);

    /**
     * @brief Replaces the instruction-assembler dict, mirroring CuInsAssemblerRepos.reset.
     * @param insAsmDict New dict, or std::nullopt for an empty one.
     **/
    void reset(std::optional<InsAsmDict> insAsmDict = std::nullopt);

    /** @brief Looks up the assembler for an instruction key, mirroring __getitem__. Throws std::out_of_range if absent. */
    CuInsAssembler& operator[](const std::string& key);
    /** @brief Const overload of operator[]. */
    const CuInsAssembler& operator[](const std::string& key) const;

    /** @brief Whether an instruction key is present, mirroring __contains__. */
    bool contains(const std::string& key) const;

    /** @brief Number of instruction keys in the repos, mirroring __len__. */
    std::size_t size() const;

    /**
     * @brief Gets the instruction parser used internally by assemble(), exposing its
     *        last-parsed-instruction state (e.g. m_InsOp/m_InsKey/m_InsModifier/m_InsVals) for
     *        callers such as CuKernelAssembler's auto-attribute detection, mirroring python's
     *        direct (duck-typed) access to m_InsParser.
     * @return Reference to the internal parser. Only meaningful after at least one assemble() call.
     **/
    const CuInsParser& getInsParser() const;

    InsAsmDict::iterator begin() { return m_InsAsmDict.begin(); }
    InsAsmDict::iterator end() { return m_InsAsmDict.end(); }
    InsAsmDict::const_iterator begin() const { return m_InsAsmDict.begin(); }
    InsAsmDict::const_iterator end() const { return m_InsAsmDict.end(); }

    /**
     * @brief Loads the repos from a previously saved repos file, mirroring
     *        CuInsAssemblerRepos.initFromFile.
     * @param fileName Path of the repos file to load.
     **/
    void initFromFile(const std::string& fileName);

    /**
     * @brief Tries to assemble the input instruction string, mirroring
     *        CuInsAssemblerRepos.assemble.
     * @param addr Address of the instruction.
     * @param s Instruction asm text.
     * @param precheck Whether to check assemblability before actually assembling; if true, throws
     *        std::runtime_error when the instruction cannot be assembled.
     * @param showCandidates Whether to enrich thrown error messages with candidate InsKeys/known records.
     * @return The assembled instruction code.
     **/
    BigInt assemble(std::uint64_t addr, const std::string& s, bool precheck = true, bool showCandidates = true);

    /**
     * @brief Verifies that instructions yielded by feeder still assemble to their recorded codes,
     *        logging any mismatch/failure, mirroring CuInsAssemblerRepos.verify.
     * @param feeder Instruction feeder yielding (addr, code, asm, ctrl) tuples; ctrl is unused.
     **/
    void verify(CuInsFeeder& feeder);

    /**
     * @brief Updates the repos with instructions yielded by feeder.
     * @param feeder Instruction feeder yielding (addr, code, asm, ctrl) tuples; ctrl is unused.
     * @return The number of new records added; 0 if nothing changed.
     **/
    int update(CuInsFeeder& feeder);

    /**
     * @brief Rebuilds every instruction assembler from its own originally recorded (addr, code,
     *        asm) samples, mirroring CuInsAssemblerRepos.rebuild. Needed after CuInsParser changes
     *        the meaning of some instruction's values/modifiers.
     **/
    void rebuild();

    /**
     * @brief Merges instruction assemblers from another previously saved repos file into this one.
     * @param reposFile Path of the repos file to merge in.
     **/
    void merge(const std::string& reposFile);

    /**
     * @brief Merges instruction assemblers from another already-loaded repos into this one,
     *        mirroring the CuInsAssemblerRepos branch of CuInsAssemblerRepos.merge.
     * @param reposSource Repos to merge in.
     **/
    void merge(CuInsAssemblerRepos& reposSource);

    /**
     * @brief Fills in assemblers for guard-predicated variants of instructions that were only
     *        ever seen without a predicate.
     **/
    void completePredCodes();

    /**
     * @brief Saves the repos's records to a text file.
     * @param fileName Path of the file to write.
     **/
    void save2file(const std::string& fileName);

    /**
     * @brief Gets every (addr, code, asmText) record from every instruction assembler (normal and
     *        error records), optionally restricted to a subset of instruction keys, mirroring
     *        CuInsAssemblerRepos.iterRecords.
     * @param keyFilter Optional predicate over instruction keys; empty (default) means all keys.
     * @return The matching records.
     **/
    std::vector<InsInfo> iterRecords(const std::function<bool(const std::string&)>& keyFilter = nullptr) const;

    /**
     * @brief Gets every record as a (addr, code, asm, ctrl=0) feeder-style record, mirroring
     *        CuInsAssemblerRepos.recordsFeeder.
     * @param keyFilter Optional predicate over instruction keys; empty (default) means all keys.
     * @return The matching records, each with a trailing ctrl=0 field appended.
     **/
    std::vector<InsFeederRecord> recordsFeeder(const std::function<bool(const std::string&)>& keyFilter = nullptr) const;

    /**
     * @brief Prints every recorded assembling error (original vs. re-assembled code, and their
     *        bit-level diff) to stdout, mirroring CuInsAssemblerRepos.showErrRecords.
     **/
    void showErrRecords();

    /**
     * @brief Clears every instruction assembler's recorded assembling errors, mirroring
     *        CuInsAssemblerRepos.clearErrRecords.
     **/
    void clearErrRecords();

    /**
     * @brief Generates predicated variants of every instruction key's first recorded sample,
     *        mirroring CuInsAssemblerRepos.genPredRecords.
     * @return The generated (addr, code, asm, ctrl=0) records; instructions already predicated
     *         (or UNDEF) are skipped.
     **/
    std::vector<InsFeederRecord> genPredRecords() const;

    /**
     * @brief Generates placeholder records for the pseudo instruction "UNDEF", mirroring
     *        CuInsAssemblerRepos.genUndefRecords.
     * @return Three UNDEF records with values 0x1, 0x2, 0x3.
     **/
    std::vector<InsFeederRecord> genUndefRecords() const;

    /**
     * @brief Finds instruction keys close to (but not equal to) an unknown key, mirroring
     *        CuInsAssemblerRepos.getInsKeyCandidates (using an edit-distance-based ratio in place
     *        of python's difflib.SequenceMatcher).
     * @param key Unknown instruction key to find candidates for.
     * @param n Max number of candidates to return.
     * @return Newline-separated, indented candidate keys, or "None" if none are close enough.
     **/
    std::string getInsKeyCandidates(const std::string& key, int n = 5) const;

    /**
     * @brief Builds a text dump of this repos suitable for saving to (and later reloading from) a
     *        repos file, mirroring CuInsAssemblerRepos.__repr__.
     * @return The formatted "CuInsAssemblerRepos(...)" text.
     **/
    std::string repr() const;

    /**
     * @brief Builds a short display string, mirroring CuInsAssemblerRepos.__str__.
     * @return "CuInsAssemblerRepos(<N> keys)".
     **/
    std::string toString() const;

private:
    /**
     * @brief Shared implementation of update()/rebuild()/merge()/completePredCodes(), pulling
     *        records from an arbitrary generator instead of only a CuInsFeeder, mirroring
     *        CuInsAssemblerRepos.update (python's duck-typed `feeder` parameter accepts any
     *        iterable, including the in-memory generators produced by recordsFeeder/genPredRecords).
     * @param next Callable returning the next record, or std::nullopt once exhausted.
     * @param dict Destination instruction-assembler dict.
     * @return The number of new records added.
     **/
    int updateFromGenerator(const std::function<std::optional<InsFeederRecord>()>& next, InsAsmDict& dict);

    /** @brief Shared implementation of both merge() overloads. */
    void mergeFrom(CuInsAssemblerRepos& repos);

    std::optional<CuSMVersion> m_Arch;
    std::optional<CuInsParser> m_InsParser;
    InsAsmDict m_InsAsmDict;

    static std::map<int, CuInsAssemblerRepos> s_staticRepos;
};

} // namespace CuAsm
