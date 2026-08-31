#pragma once

// Umbrella header for CuAsm::Tools' clean C++ interface to every bin/ CLI tool (cuasm, dsass,
// hcubin, hnvcc, verify-cc, correct-cc) -- so embedding code, e.g. the SASS instruction-shuffling
// project this repo supports, can pull in the whole surface with one include instead of picking
// individual headers. Each tool's own header documents how its functions differ from the CLI
// (exceptions instead of return-code/std::cout diagnostics, no implicit output-file backup); see
// those headers for the actual API. cc-diff-run (bin/cc-diff-run.cpp) is deliberately not
// represented here: it is a CUDA Driver-API differential-testing harness for launching kernels on
// real hardware, not a file-conversion/analysis tool like the other six, and it depends on an
// optional CUDA Toolkit install this header group does not require.

#include "CorrectCC.hpp"
#include "Cuasm.hpp"
#include "Dsass.hpp"
#include "Hcubin.hpp"
#include "Hnvcc.hpp"
#include "VerifyCC.hpp"
