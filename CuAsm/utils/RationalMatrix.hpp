#pragma once

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "BigNum.hpp"

namespace CuAsm {

/**
 * @brief A dense matrix of exact rationals, used to mirror the handful of sympy.Matrix
 *        operations (solve, nullspace, exact fraction arithmetic) that CuInsAssembler relies on.
 *        A default-constructed (0x0) matrix is used as the "None"/unset sentinel throughout,
 *        mirroring python's use of None for m_ValMatrix/m_PSol/m_ValNullMat/m_Rhs before the
 *        first buildMatrix() call.
 **/
class RationalMatrix {
public:
    RationalMatrix() = default;

    /** @brief Constructs a rows x cols matrix filled with a given value. */
    RationalMatrix(int rows, int cols, BigRational fill = BigRational(0))
        : m_Rows(rows), m_Cols(cols), m_Data(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols), fill) {}

    /** @brief Builds a matrix from a list of equal-length rows, mirroring sympy.Matrix(list-of-lists). */
    static RationalMatrix fromRows(const std::vector<std::vector<BigRational>>& rows) {
        if (rows.empty()) {
            return RationalMatrix();
        }
        const int cols = static_cast<int>(rows[0].size());
        RationalMatrix m(static_cast<int>(rows.size()), cols);
        for (int i = 0; i < m.rows(); ++i) {
            for (int j = 0; j < cols; ++j) {
                m(i, j) = rows[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            }
        }
        return m;
    }

    /** @brief Builds a single-column matrix from a value list, mirroring sympy.Matrix(list). */
    static RationalMatrix columnVector(const std::vector<BigRational>& v) {
        RationalMatrix m(static_cast<int>(v.size()), 1);
        for (std::size_t i = 0; i < v.size(); ++i) {
            m(static_cast<int>(i), 0) = v[i];
        }
        return m;
    }

    /** @brief Stacks `top` above `bottom` (equal column counts), mirroring repeated row_insert(0, ...). */
    static RationalMatrix vstack(const RationalMatrix& top, const RationalMatrix& bottom) {
        if (top.rows() == 0) {
            return bottom;
        }
        if (bottom.rows() == 0) {
            return top;
        }
        if (top.cols() != bottom.cols()) {
            throw std::invalid_argument("RationalMatrix::vstack: column count mismatch!");
        }
        RationalMatrix result(top.rows() + bottom.rows(), top.cols());
        for (int i = 0; i < top.rows(); ++i) {
            for (int j = 0; j < top.cols(); ++j) {
                result(i, j) = top(i, j);
            }
        }
        for (int i = 0; i < bottom.rows(); ++i) {
            for (int j = 0; j < bottom.cols(); ++j) {
                result(top.rows() + i, j) = bottom(i, j);
            }
        }
        return result;
    }

    int rows() const { return m_Rows; }
    int cols() const { return m_Cols; }

    BigRational& operator()(int r, int c) { return m_Data[static_cast<std::size_t>(r) * m_Cols + c]; }
    const BigRational& operator()(int r, int c) const { return m_Data[static_cast<std::size_t>(r) * m_Cols + c]; }

    /** @brief Whether every entry is zero (an empty matrix counts as all-zero, vacuously). */
    bool isZero() const {
        return std::all_of(m_Data.begin(), m_Data.end(), [](const BigRational& v) { return v == 0; });
    }

    /** @brief Horizontal concatenation (same row count), mirroring sympy.Matrix.row_join. */
    RationalMatrix rowJoin(const RationalMatrix& other) const {
        if (rows() != other.rows()) {
            throw std::invalid_argument("RationalMatrix::rowJoin: row count mismatch!");
        }
        RationalMatrix result(rows(), cols() + other.cols());
        for (int i = 0; i < rows(); ++i) {
            for (int j = 0; j < cols(); ++j) {
                result(i, j) = (*this)(i, j);
            }
            for (int j = 0; j < other.cols(); ++j) {
                result(i, cols() + j) = other(i, j);
            }
        }
        return result;
    }

    /** @brief Transposes the matrix. */
    RationalMatrix transpose() const {
        RationalMatrix result(cols(), rows());
        for (int i = 0; i < rows(); ++i) {
            for (int j = 0; j < cols(); ++j) {
                result(j, i) = (*this)(i, j);
            }
        }
        return result;
    }

    /** @brief Matrix multiplication. */
    RationalMatrix operator*(const RationalMatrix& other) const {
        if (cols() != other.rows()) {
            throw std::invalid_argument("RationalMatrix::operator*: shape mismatch!");
        }
        RationalMatrix result(rows(), other.cols());
        for (int i = 0; i < rows(); ++i) {
            for (int k = 0; k < cols(); ++k) {
                const BigRational& a = (*this)(i, k);
                if (a == 0) {
                    continue;
                }
                for (int j = 0; j < other.cols(); ++j) {
                    result(i, j) += a * other(k, j);
                }
            }
        }
        return result;
    }

    /** @brief Standard (elementwise-product sum) dot product of two equal-length column vectors. */
    BigRational dot(const RationalMatrix& other) const {
        if (rows() != other.rows() || cols() != 1 || other.cols() != 1) {
            throw std::invalid_argument("RationalMatrix::dot: expected two column vectors of equal length!");
        }
        BigRational sum(0);
        for (int i = 0; i < rows(); ++i) {
            sum += (*this)(i, 0) * other(i, 0);
        }
        return sum;
    }

    /**
     * @brief Reduces this * x = rhs via Gauss-Jordan elimination and returns a particular
     *        solution x (free variables, if any, are set to zero), mirroring sympy.Matrix.solve.
     * @param rhs Right-hand side, with the same row count as this matrix.
     * @return The solution matrix, shaped (cols(), rhs.cols()).
     **/
    RationalMatrix solve(const RationalMatrix& rhs) const {
        if (rows() != rhs.rows()) {
            throw std::invalid_argument("RationalMatrix::solve: row count mismatch with rhs!");
        }

        RationalMatrix aug = rowJoin(rhs);
        // Pivot search must stay within the original coefficient columns: letting it wander into
        // the appended rhs column(s) (as plain rref(aug) would) can pick a "pivot" there whose
        // column index is >= cols(), which then gets used below as a row index into x (shaped
        // (cols(), rhs.cols())) - an out-of-bounds write. Restricting it here also makes the
        // inconsistent-system detection below correct: a rhs entry can only be recognized as
        // contradicting an all-zero coefficient row if that row was never allowed to claim a
        // (bogus) pivot inside the rhs column(s) to begin with.
        const std::vector<int> pivots = rref(aug, cols());

        for (int i = static_cast<int>(pivots.size()); i < aug.rows(); ++i) {
            for (int k = 0; k < rhs.cols(); ++k) {
                if (aug(i, cols() + k) != 0) {
                    throw std::runtime_error("RationalMatrix::solve: inconsistent linear system, no solution exists!");
                }
            }
        }

        RationalMatrix x(cols(), rhs.cols(), BigRational(0));
        for (std::size_t idx = 0; idx < pivots.size(); ++idx) {
            const int p = pivots[idx];
            for (int k = 0; k < rhs.cols(); ++k) {
                x(p, k) = aug(static_cast<int>(idx), cols() + k);
            }
        }
        return x;
    }

    /**
     * @brief Computes a basis of the (right) null space {x : this * x = 0}, mirroring
     *        sympy.Matrix.nullspace(). Each basis vector is a (cols(), 1) column matrix.
     * @return The basis vectors, in free-variable order; empty if the null space is trivial.
     **/
    std::vector<RationalMatrix> nullspace() const {
        RationalMatrix m = *this;
        const std::vector<int> pivots = rref(m);

        std::vector<bool> isPivotCol(static_cast<std::size_t>(cols()), false);
        for (int p : pivots) {
            isPivotCol[static_cast<std::size_t>(p)] = true;
        }

        std::vector<RationalMatrix> basis;
        for (int f = 0; f < cols(); ++f) {
            if (isPivotCol[static_cast<std::size_t>(f)]) {
                continue;
            }
            RationalMatrix v(cols(), 1, BigRational(0));
            v(f, 0) = 1;
            for (std::size_t idx = 0; idx < pivots.size(); ++idx) {
                const int p = pivots[idx];
                v(p, 0) = -m(static_cast<int>(idx), f);
            }
            basis.push_back(std::move(v));
        }
        return basis;
    }

    /// Row-major backing storage, exposed for iterating all entries (mirrors python's `for e in M`).
    const std::vector<BigRational>& data() const { return m_Data; }

private:
    /**
     * @brief Row-reduces m to reduced row echelon form in place. Row operations (swap, normalize,
     *        eliminate) always span every column of m, but the search for a column to pivot on is
     *        restricted to [0, pivotColLimit) - needed by solve(), whose caller-visible column
     *        count (cols()) is narrower than the augmented [coefficients | rhs] matrix actually
     *        passed in here, so a pivot must never be chosen from inside the rhs columns.
     * @param m Matrix to reduce.
     * @param pivotColLimit Exclusive upper bound on pivot column search; negative (the default)
     *        means "every column of m", used by nullspace()'s unaugmented caller.
     * @return The pivot column index for each pivot row, in row order; every entry is < pivotColLimit
     *         (or < m.cols() if pivotColLimit was left at its default).
     **/
    static std::vector<int> rref(RationalMatrix& m, int pivotColLimit = -1) {
        const int colLimit = pivotColLimit < 0 ? m.cols() : pivotColLimit;
        std::vector<int> pivots;
        int r = 0;
        for (int c = 0; c < colLimit && r < m.rows(); ++c) {
            int piv = -1;
            for (int i = r; i < m.rows(); ++i) {
                if (m(i, c) != 0) {
                    piv = i;
                    break;
                }
            }
            if (piv == -1) {
                continue;
            }
            if (piv != r) {
                for (int j = 0; j < m.cols(); ++j) {
                    std::swap(m(piv, j), m(r, j));
                }
            }

            const BigRational pivVal = m(r, c);
            for (int j = 0; j < m.cols(); ++j) {
                m(r, j) /= pivVal;
            }

            for (int i = 0; i < m.rows(); ++i) {
                if (i == r) {
                    continue;
                }
                const BigRational factor = m(i, c);
                if (factor == 0) {
                    continue;
                }
                for (int j = 0; j < m.cols(); ++j) {
                    m(i, j) -= factor * m(r, j);
                }
            }

            pivots.push_back(c);
            ++r;
        }
        return pivots;
    }

    int m_Rows = 0;
    int m_Cols = 0;
    std::vector<BigRational> m_Data;
};

/**
 * @brief Gets the LCM of all (already-reduced) denominators appearing in m, mirroring
 *        CuInsAssembler.getMatrixDenomLCM's denominator-gathering half.
 * @param m Matrix to scan.
 * @return The LCM of all entries' denominators (1 if every entry is already an integer).
 **/
inline BigInt matrixDenomLCM(const RationalMatrix& m) {
    BigInt lcmVal(1);
    for (const BigRational& e : m.data()) {
        const BigInt denom = boost::multiprecision::denominator(e);
        lcmVal = boost::multiprecision::lcm(lcmVal, denom);
    }
    return lcmVal;
}

/**
 * @brief Scales a matrix by the LCM of its denominators so every entry becomes an integer,
 *        mirroring CuInsAssembler.getMatrixDenomLCM.
 * @param m Matrix to scale.
 * @return Pair of (m * lcm(denominators), lcm(denominators)).
 **/
inline std::pair<RationalMatrix, BigInt> scaleToIntegerMatrix(const RationalMatrix& m) {
    const BigInt fac = matrixDenomLCM(m);
    RationalMatrix scaled(m.rows(), m.cols());
    const BigRational facRat(fac);
    for (int i = 0; i < m.rows(); ++i) {
        for (int j = 0; j < m.cols(); ++j) {
            scaled(i, j) = m(i, j) * facRat;
        }
    }
    return {scaled, fac};
}

/**
 * @brief Computes the null space of M, stacks its basis vectors as rows, and scales the result to
 *        integers, mirroring CuInsAssembler.getNullMatrix.
 * @param M Matrix to compute the null space of.
 * @return The (possibly 0x0, meaning "no null space"/None) integer null-space matrix.
 **/
inline RationalMatrix nullSpaceMatrixOf(const RationalMatrix& M) {
    const std::vector<RationalMatrix> basis = M.nullspace();
    if (basis.empty()) {
        return RationalMatrix();
    }

    RationalMatrix nm = basis[0];
    for (std::size_t i = 1; i < basis.size(); ++i) {
        nm = nm.rowJoin(basis[i]);
    }

    const auto [scaled, fac] = scaleToIntegerMatrix(nm.transpose());
    (void)fac;
    return scaled;
}

} // namespace CuAsm
