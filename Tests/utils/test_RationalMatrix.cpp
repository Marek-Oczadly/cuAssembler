#include <stdexcept>
#include <vector>

#include "../../CuAsm/utils/RationalMatrix.hpp"
#include "TestUtilsCommon.hpp"

using CuAsm::BigInt;
using CuAsm::BigRational;
using CuAsm::matrixDenomLCM;
using CuAsm::nullSpaceMatrixOf;
using CuAsm::RationalMatrix;
using CuAsm::scaleToIntegerMatrix;

/**
 * @brief Exercises CuAsm::RationalMatrix, the exact-rational linear algebra backing
 *        CuInsAssembler's basis solving (per-opcode "solve for the bit-field encoding that
 *        reproduces these known-good example instructions" step). This is the single largest
 *        untested piece of core logic outside the CheckDisasm round trips: a bug in solve(),
 *        nullspace(), or the rref() they share could silently produce a wrong instruction
 *        encoding for exactly one modifier/operand combination and never trip a CheckDisasm
 *        fixture that happens not to exercise it.
 * @return 0 if every check passed, 1 otherwise.
 **/
int main() {
    CuAsm::Test::TestReporter t;

    const RationalMatrix fromRows = RationalMatrix::fromRows({{BigRational(1), BigRational(2)}, {BigRational(3), BigRational(4)}});
    t.check("fromRows produces the right shape and places entries at (row, col)",
            fromRows.rows() == 2 && fromRows.cols() == 2 && fromRows(0, 1) == BigRational(2) && fromRows(1, 0) == BigRational(3));

    const RationalMatrix col = RationalMatrix::columnVector({BigRational(7), BigRational(8), BigRational(9)});
    t.check("columnVector builds an Nx1 matrix in order", col.rows() == 3 && col.cols() == 1 && col(2, 0) == BigRational(9));

    t.check("isZero is true for an all-zero matrix, false once any entry is nonzero and for a filled one",
            RationalMatrix(2, 2, BigRational(0)).isZero() && !fromRows.isZero() && RationalMatrix(2, 3, BigRational(5))(1, 2) == BigRational(5));

    const RationalMatrix top = RationalMatrix::fromRows({{BigRational(1), BigRational(2)}});
    const RationalMatrix bottom = RationalMatrix::fromRows({{BigRational(3), BigRational(4)}});
    const RationalMatrix stacked = RationalMatrix::vstack(top, bottom);
    t.check("vstack concatenates rows in order", stacked.rows() == 2 && stacked(0, 0) == BigRational(1) && stacked(1, 1) == BigRational(4));
    t.checkThrows<std::invalid_argument>("vstack rejects mismatched column counts", [&] {
        RationalMatrix::vstack(top, RationalMatrix::fromRows({{BigRational(1), BigRational(2), BigRational(3)}}));
    });

    const RationalMatrix joined = top.rowJoin(bottom);
    t.check("rowJoin concatenates columns in order",
            joined.cols() == 4 && joined(0, 1) == BigRational(2) && joined(0, 3) == BigRational(4));

    const RationalMatrix transposed = fromRows.transpose();
    t.check("transpose swaps shape and swaps (i,j) with (j,i)",
            transposed.rows() == fromRows.cols() && transposed(1, 0) == fromRows(0, 1));

    // [1 2] [5 6]   [19 22]
    // [3 4] [7 8] = [43 50]
    const RationalMatrix a = RationalMatrix::fromRows({{BigRational(1), BigRational(2)}, {BigRational(3), BigRational(4)}});
    const RationalMatrix b = RationalMatrix::fromRows({{BigRational(5), BigRational(6)}, {BigRational(7), BigRational(8)}});
    const RationalMatrix ab = a * b;
    t.check("matrix multiplication computes the correct product",
            ab(0, 0) == BigRational(19) && ab(0, 1) == BigRational(22) && ab(1, 0) == BigRational(43) && ab(1, 1) == BigRational(50));
    t.checkThrows<std::invalid_argument>("multiplying matrices with incompatible shapes throws",
                                          [&] { (void)(a * RationalMatrix(3, 1, BigRational(0))); });

    const RationalMatrix u = RationalMatrix::columnVector({BigRational(1), BigRational(2), BigRational(3)});
    const RationalMatrix v = RationalMatrix::columnVector({BigRational(4), BigRational(5), BigRational(6)});
    t.checkEqual("dot product of two column vectors", u.dot(v), BigRational(1 * 4 + 2 * 5 + 3 * 6));
    t.checkThrows<std::invalid_argument>("dot product of mismatched-length vectors throws",
                                          [&] { (void)u.dot(RationalMatrix::columnVector({BigRational(1), BigRational(2)})); });

    // x+y=3, x-y=1  ->  x=2, y=1; and a genuinely fractional system, exercising exact (not
    // floating-point) rational arithmetic: 2x=1 -> x=1/2.
    const RationalMatrix sysA = RationalMatrix::fromRows({{BigRational(1), BigRational(1)}, {BigRational(1), BigRational(-1)}});
    const RationalMatrix sol = sysA.solve(RationalMatrix::columnVector({BigRational(3), BigRational(1)}));
    const RationalMatrix fracSol = RationalMatrix::fromRows({{BigRational(2)}}).solve(RationalMatrix::columnVector({BigRational(1)}));
    t.check("solve() finds the exact solution to a determined system, including a fractional one",
            sol(0, 0) == BigRational(2) && sol(1, 0) == BigRational(1) && fracSol(0, 0) == BigRational(1, 2));

    // Parallel lines (x+y=1 and x+y=2) have no solution.
    const RationalMatrix inconsistentSys = RationalMatrix::fromRows({{BigRational(1), BigRational(1)}, {BigRational(1), BigRational(1)}});
    t.checkThrows<std::runtime_error>("solve() on an inconsistent system throws", [&] {
        (void)inconsistentSys.solve(RationalMatrix::columnVector({BigRational(1), BigRational(2)}));
    });

    // An underdetermined system still returns *a* particular solution, with the free variable
    // set to zero, per RationalMatrix::solve's documented contract.
    const RationalMatrix underdetermined = RationalMatrix::fromRows({{BigRational(1), BigRational(1)}});
    const RationalMatrix underdeterminedSol = underdetermined.solve(RationalMatrix::columnVector({BigRational(4)}));
    t.check("solve() on an underdetermined system sets the free variable to 0",
            underdeterminedSol(0, 0) == BigRational(4) && underdeterminedSol(1, 0) == BigRational(0));

    // [1 1] has a 1-dimensional null space {(x, -x)}; the identity matrix's is trivial.
    const RationalMatrix singleRow = RationalMatrix::fromRows({{BigRational(1), BigRational(1)}});
    const std::vector<RationalMatrix> ns = singleRow.nullspace();
    const RationalMatrix identity2 = RationalMatrix::fromRows({{BigRational(1), BigRational(0)}, {BigRational(0), BigRational(1)}});
    t.check("nullspace() finds a basis vector that solves M*x=0, and is trivial for a full-rank matrix",
            ns.size() == 1 && !ns[0].isZero() && (singleRow * ns[0])(0, 0) == BigRational(0) && identity2.nullspace().empty());

    // matrixDenomLCM / scaleToIntegerMatrix: scaling {1/2, 1/3} by their LCM (6) makes both integers.
    const RationalMatrix fracMat = RationalMatrix::fromRows({{BigRational(1, 2), BigRational(1, 3)}});
    const auto [scaled, fac] = scaleToIntegerMatrix(fracMat);
    t.check("scaleToIntegerMatrix scales by matrixDenomLCM to clear all denominators",
            matrixDenomLCM(fracMat) == BigInt(6) && fac == BigInt(6) && scaled(0, 0) == BigRational(3) && scaled(0, 1) == BigRational(2));

    // nullSpaceMatrixOf: the full CuInsAssembler-facing helper, one row per basis vector.
    t.check("nullSpaceMatrixOf shapes its result as (#basis vectors x original column count), "
            "and returns the empty sentinel for a full-rank matrix",
            nullSpaceMatrixOf(singleRow).rows() == 1 && nullSpaceMatrixOf(singleRow).cols() == 2 &&
                nullSpaceMatrixOf(identity2).rows() == 0);

    return t.finish("test_RationalMatrix");
}
