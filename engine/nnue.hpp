#pragma once
// NNUE v2: king-bucketed 768 inputs -> HIDDEN (per perspective, shared weights) -> concat(us, them) -> SCReLU
// -> one of 8 output layers (chosen by piece count).
//
// Input feature = (piece colour relative to the perspective, piece type, square relative to the perspective),
// in the block of the perspective's own king bucket. When that king stands on files e-h the board is mirrored
// left-right for that perspective, so only 4 king files (and 8 buckets) are needed.
// The net is loaded from a file written by nnue_tools/train.py (format: see nnue.cpp).
#include <cstdint>
#include <string>

namespace nnue {

constexpr int NUM_FEATURES   = 768;      // per king bucket
constexpr int KING_BUCKETS   = 8;
constexpr int OUTPUT_BUCKETS = 8;
constexpr int MAX_HIDDEN     = 1024;

// Accumulator: first-layer sums for both perspectives (0 = white, 1 = black).
struct alignas(64) Accumulator {
    int16_t v[2][MAX_HIDDEN];
};

// Loads a net file. Returns false (and fills *err) if the file is missing or malformed;
// the previously loaded net, if any, is kept in that case.
bool load(const std::string& path, std::string* err = nullptr);
void unload();
bool loaded();
int  hiddenSize();

// ---- feature indexing (must match tools/nnue_format.py) ----
// 4 files x 8 ranks of the (mirrored) king square -> bucket. Index = rank * 4 + file.
constexpr int KING_BUCKET[32] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 4, 4, 5, 5,
                                 6, 6, 7, 7, 6, 6, 7, 7, 6, 6, 7, 7, 6, 6, 7, 7};

// king key of one perspective: (bucket << 1) | mirror. Changes only when that side's king changes bucket
// or crosses the d/e file boundary; every such change needs a refresh of that perspective's accumulator.
// perspective: 0 white, 1 black. ksq: the square of that perspective's OWN king (0 = a1 ... 63 = h8).
inline int kingKey(int perspective, int ksq) {
    int s = perspective == 0 ? ksq : (ksq ^ 56);
    const int mirror = (s & 7) >= 4 ? 1 : 0;
    if (mirror) s ^= 7;
    return (KING_BUCKET[(s >> 3) * 4 + (s & 7)] << 1) | mirror;
}

// colour: 0 white, 1 black. pieceType: 0 pawn, 1 knight, 2 bishop, 3 rook, 4 queen, 5 king.
// sq: 0 = a1 ... 63 = h8. key = kingKey(perspective, that perspective's own king square).
inline int featureIndex(int perspective, int key, int colour, int pieceType, int sq) {
    int s = perspective == 0 ? sq : (sq ^ 56);
    if (key & 1) s ^= 7;
    const int rel = colour != perspective ? 1 : 0;
    return (key >> 1) * NUM_FEATURES + (rel * 6 + pieceType) * 64 + s;
}

// Output layer for a position with n pieces on the board (both kings included).
inline int outputBucket(int n) {
    const int b = (n - 2) / 4;                       // n >= 2, so this never divides a negative number
    return b < 0 ? 0 : (b > OUTPUT_BUCKETS - 1 ? OUTPUT_BUCKETS - 1 : b);
}

// ---- accumulator operations ----
void reset(Accumulator& acc);                                   // both perspectives = first-layer bias
void resetPerspective(Accumulator& acc, int perspective);       // one perspective = first-layer bias
void copy(Accumulator& dst, const Accumulator& src);            // copies the active part (both perspectives)
void addFeature(Accumulator& acc, int perspective, int feature);
void subFeature(Accumulator& acc, int perspective, int feature);

// SIMD (AVX2) paths are used automatically when the CPU supports them; the results are bit-identical to the
// plain C++ code. setSimd(false) forces the plain code (for tests and comparisons).
void setSimd(bool on);
bool simdActive();

// Score in centipawns from the point of view of the side to move (stm: 0 white, 1 black).
// bucket = outputBucket(number of pieces on the board).
int evaluate(const Accumulator& acc, int stm, int bucket);

}  // namespace nnue