#pragma once

#include <string>

namespace CuAsm {

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
