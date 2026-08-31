#pragma once

#include <boost/multiprecision/cpp_int.hpp>

namespace CuAsm {

/// Arbitrary-precision signed integer, used wherever python's native unbounded `int` is relied
/// upon (e.g. instruction codes, which can be up to 128 bits wide).
using BigInt = boost::multiprecision::cpp_int;

/// Arbitrary-precision exact rational number (always kept in lowest terms), used to mirror
/// sympy's exact `Rational`/`Matrix` arithmetic.
using BigRational = boost::multiprecision::cpp_rational;

} // namespace CuAsm
