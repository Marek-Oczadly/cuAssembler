# Building cuAssembler2 (C++)

This covers building the C++ port: the `CuAsm` static library, the `bin/`
command-line tools (`cuasm`, `dsass`, `hcubin`, `hnvcc`, `verify-cc`, `correct-cc`, `cc-diff-run`),
and the test suite. See [README.md](README.md) for what each of those actually does.

**Status**: the Windows instructions below are exercised regularly (this is the primary
development platform for this project). The Linux instructions are believed correct from reading
`CMakeLists.txt` and the toolchain requirements, but **have not actually been run/verified on a
Linux machine** -- treat them as a starting point, not a guarantee, and please report gaps.

## What CMake needs, and where it comes from

* **CMake 3.20+** and a **C++23 compiler** (`CMAKE_CXX_STANDARD 23`, `CMAKE_CXX_EXTENSIONS OFF`).
  On Windows this means MSVC from Visual Studio 2022; on Linux, GCC 13+ or Clang 16+ are the
  realistic minimums for the C++23 standard-library features used here (`std::span`, ranges,
  etc.) -- older compilers are untested and likely won't work.
* **Boost, `graph` + `regex` components** -- a *system* dependency (`find_package(Boost REQUIRED
  COMPONENTS graph regex)`), not fetched by CMake. Boost.Graph backs the control-code hazard graph
  (`bin/ccCommon.hpp`); Boost.Regex backs some of `CuInsParser`'s lookbehind-dependent patterns
  that `std::regex` can't express. Both pull in several other Boost component libraries
  transitively (container/random/serialization/thread/atomic/chrono/date_time) -- installing the
  package manager's `boost-graph`/`libboost-graph-dev` package should pull all of that in for you.
* **ELFIO, CLI11, spdlog** -- fetched automatically via CMake `FetchContent` on first configure.
  No manual install needed, but the first configure needs network access to clone them.
* **CUDA Toolkit** (`nvcc`, `cuobjdump`, `nvdisasm` on `PATH`) -- *not* required to configure or
  build the C++ code itself (`find_package(CUDAToolkit)` is `QUIET`/optional, and only gates
  `cc-diff-run` and two CUDA-language microbenchmark targets under `TestData/microbench/`). It
  *is* required to actually run most of the `bin/` tools against real cubins, and nearly the
  entire test suite compiles fixture kernels with `nvcc` and/or shells out to `cuobjdump`/
  `nvdisasm` at test-*run* time -- those tests will fail outright (not skip) if the toolkit isn't
  on `PATH`. A full Driver-API-capable install (not just the redistributable `nvcc`/`cuobjdump`/
  `nvdisasm` binaries) additionally enables building `cc-diff-run`.
* **Graphviz (`dot` on `PATH`)** -- optional. Exactly one test (`test_nvinfo`) best-effort renders
  a dependency graph through it and treats a missing/failing `dot` as non-fatal; nothing else in
  the build or tools touches it.

## Windows

### Prerequisites

* Visual Studio 2022 with the "Desktop development with C++" workload (this repo's own build
  scripts pin MSVC toolset `14.44` via `-vcvars_ver=14.44`; adjust that if you have a different
  toolset installed).
* [vcpkg](https://github.com/microsoft/vcpkg), with `VCPKG_ROOT` set in your environment, and
  Boost installed through it:
  ```cmd
  vcpkg install boost-graph boost-regex
  ```
  (classic mode -- there's no `vcpkg.json` manifest in this repo, so packages must already be
  installed in your vcpkg instance before configuring.)
* CUDA Toolkit (see above) -- install normally and make sure `nvcc`/`cuobjdump`/`nvdisasm` end up
  on `PATH`.
* Graphviz, optional (see above).

### Using the provided scripts

Two `.bat` scripts live at the repo root and are the easiest way to build/test:

* **`build.bat`**: requires `VCPKG_ROOT` to be set; deletes any existing `build/` directory,
  reconfigures from scratch with the vcpkg toolchain file, builds the full solution in `Release`,
  then runs the whole test suite (`ctest`). Pass `-s` to redirect the `ctest` output to
  `test-results.log` at the repo root instead of the console. **This always does a full clean
  rebuild** -- expect it to take a while (compiling `CuAsm`, all `bin/` tools, and every test
  target, then running the full suite, including nvcc-compiling every test fixture kernel).
  ```cmd
  build.bat
  ```
* **`test.bat`**: does *not* reconfigure or rebuild -- it just re-runs `ctest` against whatever is
  already in `build/` (run `build.bat` or the manual steps below at least once first). Also
  accepts `-s` for the same log-redirect behavior.
  ```cmd
  test.bat
  ```

### Manual steps

Equivalent to what `build.bat` does, if you want more control (e.g. an incremental build after
editing a handful of files, which is much faster than the scripts' always-clean `build.bat`):

In a Developer Command Prompt for VS 2022 (or after running `vcvarsall.bat x64`):

```cmd
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
msbuild build\cuAsmCPP.sln /p:Configuration=Release /m
```

`cmake -S . -B build` on its own (re-running against an existing `build/` directory) is a
fast reconfigure, not a full wipe -- prefer it over `build.bat` when you've only touched
`CMakeLists.txt` files or added/removed source files, not when you just edited existing `.cpp`/
`.hpp` bodies (which `msbuild` alone picks up incrementally with no reconfigure needed).

To build a single target instead of the whole solution (faster while iterating on one tool):

```cmd
msbuild build\bin\verify-cc.vcxproj /p:Configuration=Release
```

### Running tests

```cmd
cd build
ctest -C Release --output-on-failure
```

Or target one test/test group by name (ctest matches by regex):

```cmd
ctest -C Release --output-on-failure -R test_ccHazard
ctest -C Release --output-on-failure -R CheckControlCodesRoundtrip
```

## Linux (untested)

The project has no Linux-specific CMake logic and no reason it shouldn't build there -- `hnvcc`
(`bin/hnvcc.cpp`) is in fact Linux-*only* (see [README.md](README.md)) -- but nobody has actually
run this build on Linux yet. The steps below are a best-effort translation of the Windows
instructions from reading `CMakeLists.txt`; expect to hit and need to fix small issues (compiler
version availability, Boost package naming on your distro, etc.).

### Prerequisites

On a Debian/Ubuntu-family system, something like:

```sh
sudo apt install cmake g++-13 libboost-graph-dev libboost-regex-dev
```

(`g++-13` for C++23 support -- use whatever your distro calls a GCC 13+ or Clang 16+ package;
`libboost-graph-dev`/`libboost-regex-dev` should pull in their transitive Boost dependencies.)
Install the CUDA Toolkit per NVIDIA's Linux instructions and ensure `nvcc`, `cuobjdump`, and
`nvdisasm` end up on `PATH` (needed for most tools and nearly the whole test suite -- see above).
Graphviz (`dot`) is optional, same as on Windows.

### Build

There is no Linux equivalent of `build.bat`/`test.bat` yet -- configure and build directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

(No `-DCMAKE_TOOLCHAIN_FILE` is needed here unless your Boost install itself comes from a vcpkg
instance -- a system package manager's Boost should be found by `find_package(Boost)` directly.)

### Running tests

```sh
cd build
ctest -C Release --output-on-failure
```
