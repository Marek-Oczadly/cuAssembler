#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

/**
 * @brief Counts positions where bit vbit of v differs between index x and index x with bit
 *        xbit flipped, mirroring the Python match_bit() helper.
 * @param v Values array (already truncated to length NT, a power of two).
 * @param vbit Bit index of the values to test.
 * @param xbit Bit index of the index to flip.
 * @return Number of positions where the tested bit differs after flipping xbit.
 */
static long long matchBit(const std::vector<int32_t>& v, int vbit, int xbit) {
    size_t NT = v.size();
    long long s = 0;
    for (size_t x = 0; x < NT; x++) {
        size_t rx = x ^ (size_t(1) << xbit);
        bool vs_x = ((v[x] >> vbit) & 1) != 0;
        bool vs_rx = ((v[rx] >> vbit) & 1) != 0;
        s += (vs_x != vs_rx) ? 1 : 0;
    }
    return s;
}

/**
 * @brief Formats an integer as a zero-padded binary string of at least the given length,
 *        mirroring the Python binstr() helper.
 * @param b Value to format.
 * @param l Minimum string length (left-padded with '0').
 * @return Binary string representation of b.
 */
static std::string binstr(long long b, int l) {
    std::string bs;
    if (b == 0) {
        bs = "0";
    } else {
        long long v = b;
        while (v > 0) {
            bs.insert(bs.begin(), char('0' + (v & 1)));
            v >>= 1;
        }
    }
    if ((int)bs.size() < l)
        bs.insert(0, std::string(l - bs.size(), '0'));

    return bs;
}

/**
 * @brief Computes the number of bits required to represent n, mirroring Python's
 *        int.bit_length().
 * @param n Value to measure (treated as non-negative).
 * @return Number of bits required to represent n.
 */
static int bitLength(unsigned long long n) {
    int bits = 0;
    while (n > 0) {
        bits++;
        n >>= 1;
    }

    return bits;
}

/**
 * @brief Entry point mirroring the original TestData/microbench/L2Bank/test_resplot.py script:
 *        reads the group-scan results produced by l2bank_test, measures how flipping each
 *        address bit correlates with each low bank-index bit, prints the per-bit mismatch
 *        table, and writes the resulting curve data to a file for external plotting, replacing
 *        plt.show() (no portable equivalent here) as done elsewhere in this port.
 * @return 0 on success, 1 if group_bak.dat could not be read.
 */
int main() {
    std::ifstream fin("group_bak.dat", std::ios::binary);
    if (!fin) {
        std::cerr << "Failed to open group_bak.dat\n";
        return 1;
    }

    std::vector<int32_t> ts;
    int32_t v;
    while (fin.read(reinterpret_cast<char*>(&v), sizeof(v)))
        ts.push_back(v - 1);

    size_t nts = ts.size();
    int N_xbit = bitLength(nts) - 1;
    const int N_vbit = 3;
    size_t NT = size_t(1) << N_xbit;

    ts.resize(NT);

    std::vector<std::vector<double>> cs(N_xbit, std::vector<double>(N_vbit));

    for (int vb = 0; vb < N_vbit; vb++) {
        for (int xb = 0; xb < N_xbit; xb++) {
            long long s = matchBit(ts, vb, xb);
            std::printf(" %2d  %2d  %8lld  %s\n", vb, xb, s, binstr(s, N_xbit + 1).c_str());
            cs[xb][vb] = double(s) / double(NT);
        }
        std::printf("\n");
    }

    std::ofstream fout("resplot.dat");
    for (int xb = 0; xb < N_xbit; xb++) {
        for (int vb = 0; vb < N_vbit; vb++) {
            fout << cs[xb][vb];
            if (vb + 1 < N_vbit)
                fout << " ";
        }
        fout << "\n";
    }

    return 0;
}
