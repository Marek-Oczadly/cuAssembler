# CuAssembler: An unofficial CUDA assembler

This is a C++ port of [cloudcores/CuAssembler](https://github.com/cloudcores/CuAssembler) (MIT
licensed), living under `cuAsm-CPP/`. See [build.md](build.md) for how to build it on Windows or
Linux (Linux untested), and [Tutorial.md](Tutorial.md)/[UserGuide.md](UserGuide.md) for the
`.cuasm` text format itself, which this port still uses.

## What is CuAssembler

**CuAssembler** is an unofficial assembler for nvidia CUDA. It's an assembler, reads
assemblies(sass) and writes machine codes(in cubin). It's not another compiler just like
officially provided by nvidia such as `nvcc` for CUDA C, and `ptxas` for ptx.

The aim of **CuAssembler** is to bridge the gap between `ptx`(the lowest level officially
supported and documented by nvidia) and the machine code. Some similar packages include `asfermi`
and `maxas`, which can only handle some of CUDA instruction sets. CuAssembler currently supports
`Pascal/Volta/Turing/Ampere` instruction set(`SM60/61/70/75/80/86/...`), but the mechanism could be
easily extended to older and possibly future CUDA instruction sets, since most of the instruction
sets could be probed automatically.

**NOTE**: This library is still in its infancy, there are still a lot of works to be done.
Interfaces and architectures are subject to change, use it at your own risk.

## When and how should CuAssembler be used

Many CUDA users will inspect the generated sass code by `cuobjdump` after doing optimization of
CUDA c code. The easiest way to tune the sass code is to modify CUDA c code itself, and then
recheck the generated sass code again. For many cases, this will lead you to good enough codes (If
you are really good at this :) ). However, for those ninja programers that want to optimize the
codes down to every instruction, it would be rather upset when they cannot command the compiler to
generate the code they want. An alternative tuning method is to modify the intermediate ptx code,
which is full of vague variables particularly tedious and difficult to follow, and the generated
machine codes are still not always satisfying. CuAssembler allows the user to tune the generated
sass code directly.

It should be emphasized that, for most CUDA programmers, CUDA C (sometimes ptx) is always the
first choice. It is full featured with great deal of compiling optimizations, officially supported
and well documented by nvidia. They know best of their hardware, hence the compiler is also
capable of doing some architecture specific optimizations. When the generated sass code is far
from expected, you are very likely to have plenty of space for high level languages to play with.
There are also large amount of communities and forums which could turn to for help. Playing with
assemblies is pretty pain-staking comparing with high level languages, you need to worry about
everything that could be done automatically by the compiler. It becomes an eligible option only
when you are already quite familiar with CUDA c and ptx, and have played all the tricks you know
to optimize, but still find the generated codes are not satisfying. Even in this case, it's still
much more convenient to start with CUDA c or ptx, and then do some minor amendments based on the
generated sass. This is the main usage CuAssembler is designed for: providing an option for minor
adjustment of the generated machine codes, which is not possible by official tools.

Another important usage of CuAssembler is for micro-benchmarking, i.e., probing some specific
details of micro-architecture by some specifically designed small programs. Good code optimization
usually needs quite deep understanding of the hardware, especially performance related figures,
such as the latency and throughput of different instructions, the cache hierarchy, the latency and
throughput of every level of caches, cache replacement policies, etc. Many micro-benchmarking could
be done using CUDA c, but its more straightforward and flexible when using assemblies, since you
can not only arrange the instructions in any order you want, but also set the control codes
directly, which is no way to be done in CUDA c or ptx.

As an assembler, CuAssembler simply translates the assemblies to machine codes literally, and then
embeds them to cubin so it can be loaded and executed. It's programers' responsibility to guarantee
the correctness of the code semantically, such as explicit register allocation, proper arrangement
of instructions, and correct usage of registers (e.g., register pair for 64bit variables always
starts from even). So you should get familiar with those conventions first, otherwise it's not
possible to write legal assembly codes, and this kind of error will be far from conspicuous to
catch. Nevertheless, legal assemblies does not imply legal program. There are many kinds of
resources involved in CUDA program, such as general purpose registers, predicates, shared
memories, and many others. They should match with the hardware configurations, and should be
eligible for launching the specified dimension of blocks. Checking rigorous correctness of the
whole program needs comprehensive understanding of the launch model and instruction set, both
grammatically and semantically, far from possible without official support. So, it is left to the
user to guarantee the correctness of the program, with very limited help from the assembler --
except for one specific, rigorously-checked piece of that correctness: scoreboard control codes
(the `B`/`R`/`W`/wait-barrier bits governing instruction-issue timing), which `verify-cc`/
`correct-cc` (below) can actually verify and repair for you, since unlike general program
correctness, that piece has a closed-form hazard model this codebase implements.

## Control-code verification & correction

Beyond translating assembly to machine code, this repo also hosts an active project to
**verify and correct SASS scoreboard control codes** -- built specifically to support a separate
SASS instruction-shuffling project (see [`Reports/control-codes-validation.md`](../Reports/control-codes-validation.md)
for the full design scope, and [`Reports/tasks.md`](../Reports/tasks.md) for the phase-by-phase
implementation history and known gotchas). In short: when an external tool reorders SASS
instructions within a kernel without adding or removing any, the existing control codes (which
encode wait-barriers, read/write scoreboard barriers, yield, and stall count) can become wrong --
`verify-cc` detects that (RAW/WAW/WAR hazard verification against a 6-slot scoreboard model), and
`correct-cc` repairs it in place, rejecting a reorder outright when in-place repair is provably
impossible rather than papering over it.

This currently covers **Turing and Ampere (`sm_75`+)** only -- older architectures are rejected up
front, not because the underlying assembler can't decode them, but because the curated per-opcode
operand-role/latency data this analysis depends on is only reliably documented from Turing onward.

## A short HOWTO

CuAssembler is not designed for creating CUDA program from scratch, it has to work with other CUDA
toolkits. A good start of cubin is needed, maybe generated by `nvcc` from CUDA C using option
`-cubin` or `--keep`, or `ptxas` from hand-written or tuned PTX codes. Currently `nvcc` doesn't
support resuming the linking with modified cubin directly(not likely even in the future, due to
its vulnerability). Thus the generated cubin usually need to be loaded in driver api. However,
`nvcc` has a `--dryrun` option that can list all the commands that really builds up the compiling
steps; `hnvcc` (below) hacks this to dump or substitute the `ptxas`-produced cubin. Then we can run
this program just using runtime api, which is much simpler (or, for control-code differential
testing specifically, `cc-diff-run` loads and launches a cubin directly via the CUDA Driver API,
without needing a runtime-API executable at all). However, this also implies a limitation of our
approach, all the sections, symbols, global variables in cubin should kept as is, otherwise the
hacking may not work properly.

Remember to keep other optimization works done before coding with CuAssembler, since any
modification of the input cubin may invalidate the modification done in CuAssembler, then you may
need to redo all the work again.

See the [User Guide](UserGuide.md) and [Tutorial](Tutorial.md) for basic tutorial and introduction
of input formats (the `.cuasm` text format they describe is unchanged by this C++ port).

## Building

See [build.md](build.md) for full prerequisites and build/test instructions, on both Windows and
(untested) Linux. In short: CMake 3.20+, a C++23 compiler, Boost (`graph`+`regex` components)
installed system-wide, and (to actually run most of the tools below, or the test suite) the CUDA
Toolkit's `nvcc`/`cuobjdump`/`nvdisasm` on `PATH`. ELFIO, CLI11, and spdlog are fetched
automatically by CMake.

## Command-line tools

All tools live under `bin/` and build to executables of the same name (`cuasm`, `dsass`, `hcubin`,
`hnvcc`, `verify-cc`, `correct-cc`, `cc-diff-run`). Every tool (except `hnvcc`) is a
[CLI11](https://github.com/CLIUtils/CLI11)-based CLI -- run any of them with `-h`/`--help` for the
authoritative, always-up-to-date option list. Every tool other than `hnvcc` also shares two
conventions: an existing output file is renamed to `<outfile>~` before being overwritten, and
`-v`/`--verbose` vs. `-q`/`--quiet` (mutually exclusive) control log verbosity, optionally saved to
a file with `-f`/`--logfile` (rolled to `.1`/`.2`/`.3` if it already exists).

### cuasm

Convert cubin from/to cuasm text files.

```
usage: cuasm [-h] [-o OUTFILE] [-f LOGFILE] [-v | -q] [--bin2asm | --asm2bin] infile [infile]

positional arguments:
  infile                Input file, optionally followed by an output file

options:
  -h, --help            show this help message and exit
  -o OUTFILE, --output OUTFILE
  -f LOGFILE, --logfile LOGFILE
  -v, --verbose
  -q, --quiet
  --bin2asm             Force cubin -> cuasm direction
  --asm2bin             Force cuasm -> cubin direction
```

```
$ cuasm a.cubin
    disassemble a.cubin => a.cuasm (direction inferred from the .cubin extension)

$ cuasm a.cuasm
    assemble a.cuasm => a.cubin

$ cuasm a.cubin -o x.cuasm
    disassemble a.cubin => x.cuasm, output file given explicitly

$ cuasm a.cubin x.cuasm
    same as `cuasm a.cubin -o x.cuasm`

$ cuasm a.o --bin2asm
    disassemble a.o => a.cuasm -- ".o" isn't a recognized extension, so direction must be forced

$ cuasm a.cubin -f abc -v
    disassemble a.cubin => a.cuasm, save log to abc.log, verbose mode
```

### dsass

Dump sass annotated with scoreboard control codes -- `cuobjdump -sass` doesn't show these, which
makes it hard to see instruction dependencies at a glance; `dsass` extracts and displays them
alongside the disassembly. The sass input must match `cuobjdump -sass`'s exact format.

```
usage: dsass [-h] [-o OUTFILE] [-k] [-n] [-f LOGFILE] [-v | -q] infile [infile]

positional arguments:
  infile                Dumped sass (.sass), cubin (.cubin), or any other binary containing a cubin

options:
  -h, --help
  -o OUTFILE, --output OUTFILE
  -k, --keepcode        Keep code-only lines in the output instead of stripping them
  -n, --nodeschack      Skip the SM8x cache-policy desc-bit hack, no matter the cubin's SM version
  -f LOGFILE, --logfile LOGFILE
  -v, --verbose
  -q, --quiet
```

```
$ dsass a.cubin
    dump sass from a.cubin, write the result with control codes to a.dsass

$ dsass a.exe -o a.txt
    dump sass from a.exe (any binary containing a cubin), write to a.txt

$ dsass a.sass
    translate an already-cuobjdumped sass file into a.dsass

$ dsass a.cubin -k
    keep code-only lines in the output (stripped by default, for compact output)
```

### hcubin

Hack an SM8x cubin's cache-policy desc bit so `desc[UR#]` is always shown explicitly in the
disassembly, instead of the default cache-policy UR being silently omitted (which can otherwise
produce an ambiguous/inconsistent re-assembly for `desc[]`-using instructions).

```
usage: hcubin [-h] [-o OUTFILE] [-f LOGFILE] [-v | -q] infile [infile]

positional arguments:
  infile                A valid cubin file

options:
  -h, --help
  -o OUTFILE, --output OUTFILE
  -f LOGFILE, --logfile LOGFILE
  -v, --verbose
  -q, --quiet
```

```
$ hcubin a.cubin
    hack a.cubin into a.hcubin

$ hcubin a.cubin -o x.bin
    hack a.cubin into x.bin

$ hcubin a.cubin x.bin
    same as `hcubin a.cubin -o x.bin`
```

Note: if the cubin's SM version predates Ampere (no hack needed), **no output file is written at
all** -- this isn't an error, just nothing to do.

### hnvcc

**Linux only** (this tool does not build/run on Windows -- it prints a message and exits
immediately there). A hacked wrapper of `nvcc` for substituting a previously-dumped, externally
patched cubin (e.g. one run through `correct-cc`) back into a fresh build in place of `ptxas`'s own
output, without touching the build system. Requires `nvcc`'s `--dryrun` output, so `-keep`/
`-keep-dir` must not already be present in your own `nvcc` arguments. Dispatches on the
`HNVCC_OP` environment variable:

```
Usage: hnvcc nvcc_args...

    Not-set or 'none' : call original nvcc
    'dump' : dump cubins to dump.<name>.sm_#.cubin (rename to hack.<name>.sm_#.cubin before the 'hack' run)
    'hack' : substitute hack.<name>.sm_#.cubin back in if present, otherwise build normally
    Others : error
```

```
$ hnvcc test.cu -arch=sm_75 -o test
    call original nvcc

$ HNVCC_OP=dump hnvcc test.cu -arch=sm_75 -o test
    dump test.sm_#.cubin to dump.test.sm_#.cubin

$ HNVCC_OP=hack hnvcc test.cu -arch=sm_75 -o test
    hack test.sm_#.cubin with hack.test.sm_#.cubin (rename dump.* to hack.* first)
```

### verify-cc

Verify the scoreboard control codes of every kernel in a cubin: decodes each kernel's
instructions (control codes plus curated per-opcode operand-role/latency data), then checks every
RAW/WAW/WAR hazard implied by those instructions against a 6-slot scoreboard simulation of the
kernel's *current* control codes. **Requires Turing (`sm_75`) or newer.**

```
usage: verify-cc [-h] [-k KERNEL] [-f LOGFILE] [-v | -q] infile

positional arguments:
  infile                Input cubin file

options:
  -h, --help
  -k KERNEL, --kernel KERNEL   Restrict output to one kernel by name
  -f LOGFILE, --logfile LOGFILE
  -v, --verbose
  -q, --quiet
```

```
$ verify-cc a.cubin
    decode and verify every kernel's control codes, listing each instruction's control code

$ verify-cc a.cubin -k myKernel
    restrict output to one kernel

$ verify-cc a.cubin -v
    verbose logging
```

Exits 0 and prints `PASSED`/`FAILED` per kernel either way (a `FAILED` kernel is a successful run
of the tool, just with a real finding) -- a nonzero exit means the cubin/kernel itself couldn't be
loaded, decoded, or matched, not that a hazard was found. On a `FAILED` kernel, each unresolved
hazard is printed with its producer/consumer instruction indices, hazard type (RAW/WAW/WAR), and
the conflicting register.

### correct-cc

Correct the scoreboard control codes of every kernel in a cubin: like `verify-cc`, but instead of
just reporting hazards, recomputes `waitbar`/`readbar`/`writebar`/`stall` from scratch to close
every one it can detect. Barrier-id assignment for variable-latency hazards is an interval-coloring
problem over the 6 physical scoreboard slots; if a kernel's instruction order needs more than 6
simultaneously-live barriers, that kernel is **provably unrepairable in place** (there's no
standalone "wait" instruction to insert, and this tool never adds/removes instructions) -- its
control codes are left completely unchanged, and *no output file is written at all* if any kernel
in the cubin hits this. **Requires Turing (`sm_75`) or newer.**

```
usage: correct-cc [-h] [-o OUTFILE] [-f LOGFILE] [-v | -q] infile [infile]

positional arguments:
  infile                Input file, optionally followed by an output file

options:
  -h, --help
  -o OUTFILE, --output OUTFILE   Defaults to infile with its extension replaced by ".ccubin"
  -f LOGFILE, --logfile LOGFILE
  -v, --verbose
  -q, --quiet
```

```
$ correct-cc a.cubin
    correct a.cubin's control codes, writing a.ccubin

$ correct-cc a.cubin -o fixed.cubin
    same, writing fixed.cubin

$ correct-cc a.cubin fixed.cubin
    same as -o
```

### cc-diff-run

A generic CUDA Driver-API host launcher, built specifically for control-code correction
**differential testing**: load an arbitrary cubin (nvcc's original output, or `correct-cc`'s
patched bytes -- either works, since this loads raw cubin bytes via `cuModuleLoadData` rather than
linking a runtime-API executable) and launch one kernel from it with CLI-specified arguments, so
the *same* launcher can run a "before" and "after" cubin identically and let you diff their output
buffers. Only built when a full CUDA Toolkit with the Driver API library is available (`find_package(CUDAToolkit)`)
-- if it's not, this target is silently skipped at configure time rather than failing the build.

```
usage: cc-diff-run --cubin CUBIN --kernel KERNEL [--grid X,Y,Z] [--block X,Y,Z] [--shmem BYTES]
                    [--arg SPEC]... [--dump-arg INDEX:PATH]...

options:
  --cubin CUBIN         Path to the cubin to load (required)
  --kernel KERNEL       __global__ function name to launch (required)
  --grid X,Y,Z           Grid dimensions (default 1,1,1)
  --block X,Y,Z          Block dimensions (default 1,1,1)
  --shmem BYTES          Dynamic shared memory bytes (default 0)
  --arg SPEC             One kernel parameter, in order -- repeatable, see grammar below
  --dump-arg INDEX:PATH  After the launch, copy a "buf=" arg's device buffer back to host and
                         write its raw bytes to PATH -- repeatable
```

`--arg` grammar (repeatable, in kernel-parameter order):

```
i32=<int>                    4-byte signed int parameter
u32=<uint>                   4-byte unsigned int parameter
f32=<float>                  4-byte float parameter
i32x4=<a>,<b>,<c>,<d>        16-byte int4 parameter
f32x4=<a>,<b>,<c>,<d>        16-byte float4 parameter
buf=<bytes>[,fill=<byte>]    Device buffer of <bytes> bytes, filled with the repeated byte value
                              <byte> (default 0) before launch; the kernel parameter is the
                              resulting device pointer.
```

```
$ cc-diff-run --cubin cudatest.7.sm_75.cubin --kernel local_test --grid 4,1,1 --block 64,1,1 \
      --arg i32=2 --arg i32=5 --arg buf=1088,fill=0 --dump-arg 2:local_test.orig.bin
```

**Scope note**: `compute-sanitizer --tool racecheck/synccheck` runs against this tool's output are
a legitimate but *weak* outside-the-repo smoke test for control-code correctness -- they operate at
the PTX/CUDA-memory-model level, one layer above the hardware instruction-issue timing scoreboard
control codes actually govern, so a wrong/missing scoreboard wait produces silent data corruption
that racecheck/synccheck won't flag as a race or illegal sync call. `--dump-arg` exists so you can
diff actual output-buffer bytes between an original-cubin run and a corrected-cubin run instead,
which *is* a check that could catch a real correction bug.

## C++ library interface

Everything the CLI tools above do is also available as a plain C++ API, so it can be called
in-process instead of shelling out to an executable -- e.g. from a separate tool (such as the SASS
instruction-shuffling project this repo's control-code work supports) that wants to verify/correct
control codes as part of its own pipeline. Headers live under `include/CuAsmTools/`, in namespace
`CuAsm::Tools`, and are exposed on `CuAsm`'s public include path -- once your CMake target links
`CuAsm`, just:

```cpp
#include "CuAsmTools/CuAsmTools.hpp"   // pulls in every header below, or include one directly
```

Unlike the CLI tools, these functions **throw on failure** (`std::exception`, or more specifically
`std::invalid_argument`/`std::runtime_error`/`CuAsm::CalledProcessError`) instead of printing a
diagnostic and returning a status code, and they never write to `std::cout` or `CuAsmLogger` --
callers own presentation. They also never silently rename an existing output file to `<path>~`
before overwriting it the way the CLI tools do (see each header's own doc comment) -- a
programmatic caller managing its own filesystem state gets a plain overwrite instead.

| Header | Function | What it does |
|---|---|---|
| `Cuasm.hpp` | `disassembleCubin(cubinPath, cuasmPath = {})` | Disassemble a cubin to cuasm text; returns the output path. |
| | `assembleCuasm(cuasmPath, cubinPath = {})` | Assemble cuasm text to a cubin; returns the output path. |
| | `convert(inPath, outPath = {}, direction = CuasmDirection::Auto)` | Either direction, inferred from extension unless `direction` overrides it. |
| `Dsass.hpp` | `dumpControlCodeSass(inPath, outPath = {}, keepCodeOnlyLines = false, skipDescHack = false)` | Dump control-code-annotated sass; returns the output path. |
| `Hcubin.hpp` | `hackCubinCachePolicyDesc(cubinPath, outPath = {})` | Apply the SM8x desc-bit hack; returns whether it was needed (and, if so, applied -- if not, `outPath` is not written). |
| `Hnvcc.hpp` (Linux only) | `runNvcc(args)` / `dumpNvccCubins(args)` / `hackNvccCubins(args)` | The three `HNVCC_OP` operations, called directly instead of via the environment variable. |
| | `runNvccFromEnvironment(args)` | Exact CLI parity with `hnvcc` itself: dispatches on `HNVCC_OP`. |
| `VerifyCC.hpp` | `verifyCubinControlCodes(cubinPath, kernelFilter = "")` | Load + decode + verify every (or one filtered) kernel; returns a `CubinVerificationReport` (per-kernel control codes, decoded instructions, and the hazard-check result) instead of printing one. |
| `CorrectCC.hpp` | `correctCubinControlCodes(inPath, outPath)` | Load + decode + correct every kernel, patch the result back into a cubin copy, and write it *only* if every kernel was repairable; returns a `CubinCorrectionReport` (`anyUnrepairable`/`wrote` flags plus a per-kernel `ControlCodeCheckResult`). |

`cc-diff-run` deliberately has no header here -- it's a CUDA Driver-API hardware test harness, not
a file-conversion/analysis tool like the other six, and depends on an optional CUDA Toolkit install
this header group does not require.

Example -- verify then correct a cubin in-process:

```cpp
#include "CuAsmTools/VerifyCC.hpp"
#include "CuAsmTools/CorrectCC.hpp"

const auto report = CuAsm::Tools::verifyCubinControlCodes("a.cubin");
for (const auto& kr : report.kernels) {
    if (kr.result.status == CuAsm::Tools::CheckStatus::Violated) {
        const auto fix = CuAsm::Tools::correctCubinControlCodes("a.cubin", "a.ccubin");
        // fix.wrote is false if any kernel was CheckStatus::Unrepairable -- check before assuming
        // "a.ccubin" exists.
        break;
    }
}
```

`bin/ccCommon.hpp` (used by `verify-cc.cpp`/`correct-cc.cpp` and by `VerifyCC.hpp`/`CorrectCC.hpp`
themselves) is the lower-level engine these two headers orchestrate -- `detectArch()`,
`loadControlCodes()`, `decodeInstructions()`, `verifyControlCodes()`, `correctControlCodes()`, and
the `KernelControlCodes`/`DecodedInstruction`/`ControlCodeCheckResult` types -- if you need more
granular control than "whole cubin file in, one report out".

## Classes

* **CuAsmLogger**: spdlog-backed logger used throughout `CuAsm/` and `bin/` in place of raw
  `std::cout`/`std::cerr`.
* **CuAsmParser**: parses a user-edited `.cuasm` text file and can save the result as `.cubin`.
* **CubinFile**: reads a `.cubin` file and can rewrite it into an editable `.cuasm` text file.
* **CuControlCode**: the 19-char control-code text format (`decode()`/`encode()`) and the raw
  bit-packed waitbar/readbar/writebar/yield/stall fields.
* **CuInsAssembler**: handles the value matrix `V` and solution of `w` for a specific instruction
  *key*, such as `FFMA_R_R_R_R`.
* **CuInsAssemblerRepos**: repository of `CuInsAssembler` for all known *keys*. Constructing a
  workable repos from scratch is very time consuming and needs a wide range of inputs covering all
  frequently used instructions -- a pre-gathered repos ships as `DefaultInsAsmRepos.${arch}.txt`.
  **Note**: the repository may be incomplete, but is easy to extend.
* **CuInsParser**: parses an instruction string into *keys*, *values*, and *modifiers* --
  positional/syntactic data only, not operand read/write roles (see `OperandRoleTable` below).
* **CuInsFeeder**: a simple instruction feeder reading instructions from sass dumped by
  `cuobjdump`.
* **CuKernelAssembler**: assembler for a single kernel, handling kernel-wide parameters (mostly
  `NVInfo` attributes).
* **CuNVInfo**: handles the `NVInfo` section of a cubin. Far from complete/robust -- some `NVInfo`
  attributes have very limited support.
* **CuSMVersion**: a uniform interface across all SM versions; other classes are not meant to
  contain architecture-dependent treatments, so most per-architecture work belongs here.
* **`CuAsm::Tools::OperandRoleTable`/`LatencyClassTable`** (`OperandRole.hpp`/`LatencyClass.hpp`):
  curated, per-SM-version data (per-opcode operand read/write roles; fixed-vs-variable latency
  classification) that the control-code hazard analysis above depends on -- see
  [`Reports/tasks.md`](../Reports/tasks.md) Phase 0/1 for how these were built and their current
  coverage gaps (sm_75 is complete; sm_80/86 have real TODOs).

## Future plan

Likely to support:

* Better coverage of instructions, bugfixes for officially unsupported instructions.
* Extending `verify-cc`/`correct-cc`'s `OperandRoleTable`/`LatencyClassTable` coverage to sm_80/86
  (see the coverage-gap note above), and to architectures newer than Ampere.
* More robust correctness check with aid of `nvdisasm`.
* Alias and variable support, for easier programming, may be achieved by preprocessing?

Less likely to support, but still on the plan:
* Register counting, and possibly register allocation.
* More robust parsing and user friendly error reporting.
* Control flow support beyond what the control-code CFG resolver already does for hazard analysis
  (`computeControlFlowSuccessors()`) -- e.g. surfacing it as a general-purpose disassembly aid.
* And others...
