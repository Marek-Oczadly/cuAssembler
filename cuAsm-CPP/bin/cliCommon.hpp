#pragma once

#include <filesystem>
#include <iostream>
#include <string>

#include "../CuAsm/CuAsmLogger.hpp"

// Shared helpers used by the bin/ CLI tools (cuasm, dsass, hcubin), previously copy-pasted
// byte-for-byte in each tool's own translation unit.

/**
 * @brief Verifies that an input file exists.
 * @param fname Path of the input file to check.
 * @return True if the file exists; false (after printing a diagnostic) otherwise.
 **/
inline bool checkInFileExistence(const std::string& fname) {
    if (!std::filesystem::is_regular_file(fname)) {
        std::cout << "IOError! Input file \"" << fname << "\" not found!" << std::endl;
        return false;
    }
    return true;
}

/**
 * @brief Checks whether an output file already exists and, if so, backs it up before it is overwritten.
 * @param fname Path of the output file to check.
 * @param doBackup If the file exists, rename it to "fname~" before returning; otherwise leave it untouched.
 * @return True on success; false (after printing a diagnostic) if fname is an existing directory.
 **/
inline bool checkOutFileBackup(const std::string& fname, bool doBackup = true) {
    namespace fs = std::filesystem;
    if (fs::exists(fname)) {
        if (fs::is_directory(fname)) {
            std::cout << "IOError!!! Output file \"" << fname << "\" is an existing directory!" << std::endl;
            return false;
        } else if (doBackup) {
            std::string bname = fname + "~";
            CuAsm::CuAsmLogger::logWarning("Backup existing file " + fname + " to " + bname + "...");
            fs::rename(fname, bname);
        }
    }
    return true;
}
