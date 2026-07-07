#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../CuInsAssemblerRepos.hpp"
#include "../CuInsFeeder.hpp"
#include "../CuNVInfo.hpp"
#include "BigNum.hpp"

namespace CuAsm {

/// Set this bit to show desc explicitly in SM8x, mirroring CubinUtils.pShowDesc.
extern const BigInt pShowDesc;

/**
 * @brief Modifies a PTX file's ".version"/".target" directives, mirroring
 *        CuAsm.utils.CubinUtils.transPTXVersion. CAUTION: this may cause some inconsistency!
 * @param ptxname Input PTX file name.
 * @param outname Output file name; empty (default) modifies ptxname in place.
 * @param arch Target arch string to write, e.g. "sm_75".
 * @param version PTX ISA version string to write, e.g. "7.3".
 **/
void transPTXVersion(const std::string& ptxname, const std::string& outname = "", const std::string& arch = "sm_75",
                      const std::string& version = "7.3");

/**
 * @brief Extracts every ".text*" section of a cubin as a standalone bin file, invoking callback
 *        with the output file name after each write (once, with every kernel merged together, if
 *        mergeAllKernels), mirroring CuAsm.utils.CubinUtils.feedBinFromCubin.
 * @param binname Input cubin file name.
 * @param callback Invoked with the output bin file name after each write. NOTE: the file name may
 *        be the same on every call (only its contents differ), mirroring the python generator.
 * @param outname Output bin file name; empty (default) replaces a trailing ".cubin" in binname
 *        with ".bin" (or appends ".bin" if binname has no ".cubin" suffix).
 * @param mergeAllKernels Write all kernels into one bin file instead of one file per kernel. NOTE:
 *        this may change disassembly of branching instructions.
 **/
void feedBinFromCubin(const std::string& binname, const std::function<void(const std::string&)>& callback,
                       const std::string& outname = "", bool mergeAllKernels = false);

/**
 * @brief Updates a repos with instructions disassembled from a cubin, mirroring
 *        CuAsm.utils.CubinUtils.updateReposWithCubin. Subprocess/disassembly failures are logged
 *        and swallowed rather than propagated, matching the original's broad except clauses.
 * @param repos Repos to update; should be configured for the same arch as binname.
 * @param binname Input cubin file name.
 * @param savname If non-empty, path to save repos to after a successful update.
 * @param useNvdisasm Whether to disassemble via `nvdisasm -b` (fast) instead of `cuobjdump -sass`
 *        (slow, but shows fixups).
 * @param doDescHack Whether to apply the desc hack first if the repos's arch needs it.
 * @param mergeAllKernels Whether to merge all kernels into one bin file before disassembling (only
 *        used when useNvdisasm is true).
 **/
void updateReposWithCubin(CuInsAssemblerRepos& repos, const std::string& binname, const std::string& savname = "",
                          bool useNvdisasm = true, bool doDescHack = true, bool mergeAllKernels = false);

/**
 * @brief Collects any not-yet-known .nv.info(.*) attribute, logging + recording each newly seen
 *        one, mirroring CuAsm.utils.CubinUtils.updateUnknownNVInfoWithCubin.
 * @param binname Input cubin file name.
 * @param unknownNVInfo Map of already-known unknown attribute name -> raw value; updated in place.
 **/
void updateUnknownNVInfoWithCubin(const std::string& binname, std::map<std::string, CuNVInfoValue>& unknownNVInfo);

/**
 * @brief Assembles a PTX file to a cubin, then updates a repos from it, mirroring
 *        CuAsm.utils.CubinUtils.updateReposWithPTX. Subprocess failures are logged and swallowed
 *        rather than propagated.
 * @param repos Repos to update.
 * @param ptxname Input PTX file name.
 * @param savname If non-empty, path to save repos to after a successful update.
 **/
void updateReposWithPTX(CuInsAssemblerRepos& repos, const std::string& ptxname, const std::string& savname = "");

/**
 * @brief Wraps a feeder, clearing the desc-show bit on any instruction whose disassembly doesn't
 *        actually show "desc", mirroring CuAsm.utils.CubinUtils.transDescFeeder.
 * @param feeder Feeder to wrap; must outlive the returned generator.
 * @return Generator function usable with CuInsAssemblerRepos::update(...).
 **/
std::function<std::optional<InsFeederRecord>()> transDescFeeder(CuInsFeeder& feeder);

/**
 * @brief A fat binary (executable/library) that cuobjdump can extract per-arch cubin/PTX files
 *        from, mirroring CuAsm.utils.CubinUtils.CudaBinFile.
 **/
class CudaBinFile {
public:
    /**
     * @brief Constructs a CudaBinFile bound to a fat binary path, mirroring CudaBinFile.__init__.
     * @param name Path to the input fat binary (executable/library).
     **/
    explicit CudaBinFile(const std::string& name);

    /**
     * @brief Rebinds this CudaBinFile to a different fat binary path, mirroring
     *        CudaBinFile.resetFileName.
     * @param name New fat binary path.
     **/
    void resetFileName(const std::string& name);

    /**
     * @brief Dumps a single cubin or PTX file out of the fat binary, mirroring
     *        CudaBinFile.dumpFile.
     * @param fname Output cubin/PTX file name (also used to auto-detect arch/ftype).
     * @param arch Arch string; empty (default) auto-detects from fname.
     * @param ftype "elf" or "ptx"; empty (default) auto-detects from fname's extension.
     **/
    void dumpFile(const std::string& fname, std::string arch = "", std::string ftype = "");

    /**
     * @brief Collects every not-yet-known .nv.info attribute across all cubins for an arch,
     *        mirroring CudaBinFile.updateUnknownNVInfo.
     * @param arch Arch string to restrict to.
     * @param unknownNVInfo Map of already-known unknown attribute name -> raw value; updated in place.
     * @return Reference to unknownNVInfo, for chaining.
     **/
    std::map<std::string, CuNVInfoValue>& updateUnknownNVInfo(const std::string& arch,
                                                                std::map<std::string, CuNVInfoValue>& unknownNVInfo);

    /**
     * @brief Lists every ELF (cubin) file name for an arch, mirroring CudaBinFile.iterELF (a
     *        generator in python; materialized to a vector here for simplicity).
     * @param arch Arch string to restrict to.
     * @return The matching file names.
     **/
    std::vector<std::string> iterELF(const std::string& arch);

    /**
     * @brief Lists every PTX file name, mirroring CudaBinFile.iterPTX (materialized to a vector).
     * @return The matching file names.
     **/
    std::vector<std::string> iterPTX();

    /**
     * @brief Updates a repos with every cubin (and/or PTX) file for the repos's arch, mirroring
     *        CudaBinFile.updateRepos.
     * @param repos Repos to update.
     * @param savename Path to save the repos to after each successful update.
     * @param ftype Which file kinds to process ("elf", "ptx", or both).
     **/
    void updateRepos(CuInsAssemblerRepos& repos, const std::string& savename, const std::set<std::string>& ftype = {"elf"});

    /**
     * @brief Lists file names of a given kind inside the fat binary, mirroring
     *        CudaBinFile.listFile.
     * @param ftype "elf" or "ptx".
     * @param arch Arch string; only meaningful for ftype="elf".
     * @return The matching file names, or empty on subprocess failure (logged).
     **/
    std::vector<std::string> listFile(const std::string& ftype = "elf", const std::string& arch = "sm_75");

    /**
     * @brief Iteratively dumps and processes every cubin/PTX file inside the fat binary, mirroring
     *        CudaBinFile.iterProcess. NOTE: the cubin/PTX is dumped as-is; do the desc hack in
     *        callback if necessary.
     * @param callback Invoked with the dumped file's temp path for each file.
     * @param arch Arch string to restrict to.
     * @param ftype "elf" or "ptx".
     **/
    void iterProcess(const std::function<void(const std::string&)>& callback, const std::string& arch, const std::string& ftype = "elf");

private:
    std::string mName;
    std::string mTempBinName;
    std::string mTempPTXName;
};

/**
 * @brief Collects unknown .nv.info attributes across every fat binary matched by fpatternList,
 *        mirroring CuAsm.utils.CubinUtils.updateNVInfoForArch. NOTE: mirrors the original's
 *        behavior of returning nothing (its computed dict is discarded internally too).
 * @param fpatternList Glob patterns (non-recursive) of fat binaries to scan.
 * @param arch Arch string to restrict to.
 **/
void updateNVInfoForArch(const std::vector<std::string>& fpatternList, const std::string& arch);

/**
 * @brief Iterates every cubin/PTX file across every fat binary matched by fpatternList, invoking
 *        callback for each, mirroring CuAsm.utils.CubinUtils.iterProcessFilesFromBinFiles.
 * @param fpatternList Glob patterns (non-recursive) of fat binaries to scan.
 * @param arch Arch string to restrict to.
 * @param callback Invoked with each dumped file's temp path.
 * @param ftype "elf" or "ptx".
 * @param callback2 If set, invoked after each fat binary finishes.
 * @param callback3 If set, invoked after each glob pattern finishes.
 **/
void iterProcessFilesFromBinFiles(const std::vector<std::string>& fpatternList, const std::string& arch,
                                   const std::function<void(const std::string&)>& callback, const std::string& ftype = "elf",
                                   const std::function<void()>& callback2 = nullptr, const std::function<void()>& callback3 = nullptr);

/**
 * @brief Makes a copy of a cubin with the cache-policy desc bit forcibly set on every instruction
 *        (not just those that use desc[UR#]) within its .text.* sections, mirroring
 *        CuAsm.utils.CubinUtils.hackCubinDesc. Cheaper (no disassembly needed) than fixCubinDesc,
 *        but may show desc[] on instructions that don't actually use it.
 * @param fin Input cubin file name.
 * @param fout Output cubin file name.
 * @param alwaysOutput Whether to copy fin to fout even when the hack wasn't needed.
 * @return True if the cubin needed (and received) the hack; false if its SM version predates
 *         Ampere and no hack was necessary.
 **/
bool hackCubinDesc(const std::string& fin, const std::string& fout, bool alwaysOutput = true);

/**
 * @brief Makes a copy of a cubin with the cache-policy desc bit set only on instructions that
 *        actually use desc[UR#], needed so those instructions assemble consistently on SM8x.
 * @param fin Input cubin file name.
 * @param fout Output cubin file name.
 * @return True if the cubin needed (and received) the desc hack; false if its SM version predates
 *         Ampere and no hack was necessary.
 **/
bool fixCubinDesc(const std::string& fin, const std::string& fout);

} // namespace CuAsm
