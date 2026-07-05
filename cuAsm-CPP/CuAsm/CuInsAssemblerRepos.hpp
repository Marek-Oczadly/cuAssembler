#pragma once

#include <optional>
#include <string>

#include "CuInsFeeder.hpp"
#include "CuSMVersion.hpp"

namespace CuAsm {

class CuInsAssemblerRepos {
public:
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
     * @brief Gets the arch this repos is configured for.
     * @return The repos's SM version.
     **/
    CuSMVersion getSMVersion() const;

    /**
     * @brief Loads (or falls back to an empty) default instruction-assembler dict for this
     *        repos's arch, mirroring setToDefaultInsAsmDict.
     **/
    void setToDefaultInsAsmDict();

    /**
     * @brief Updates the repos with instructions yielded by feeder.
     * @param feeder Instruction feeder yielding (addr, code, asm, ctrl) tuples; ctrl is unused.
     * @return The number of new records added; 0 if nothing changed.
     **/
    int update(CuInsFeeder& feeder);

    /**
     * @brief Verifies that instructions yielded by feeder still assemble to their recorded codes.
     * @param feeder Instruction feeder yielding (addr, code, asm, ctrl) tuples; ctrl is unused.
     **/
    void verify(CuInsFeeder& feeder);

    /**
     * @brief Merges instruction assemblers from another previously saved repos file into this one.
     * @param reposFile Path of the repos file to merge in.
     **/
    void merge(const std::string& reposFile);

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
};

} // namespace CuAsm
