#pragma once
// Minimal NNUE: 768 inputs -> HIDDEN (per perspective, shared weights) -> concat(us, them) -> SCReLU -> 1.
// Input feature = (piece colour relative to the perspective, piece type, square relative to the perspective).
// The net is loaded from a file written by tools/train.py (format: see nnue.cpp).
#include <cstdint>
#include <string>

namespace nnue {

constexpr int NUM_FEATURES = 768;
constexpr int MAX_HIDDEN   = 1024;

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

// colour: 0 white, 1 black. pieceType: 0 pawn, 1 knight, 2 bishop, 3 rook, 4 queen, 5 king.
// sq: 0 = a1 ... 63 = h8. perspective: 0 white, 1 black.
inline int featureIndex(int perspective, int colour, int pieceType, int sq) {
    const int rel = colour != perspective ? 1 : 0;
    const int s   = perspective == 0 ? sq : (sq ^ 56);
    return (rel * 6 + pieceType) * 64 + s;
}

void reset(Accumulator& acc);                                   // acc = first-layer bias
void copy(Accumulator& dst, const Accumulator& src);            // copies the active part (both perspectives)
void addFeature(Accumulator& acc, int perspective, int feature);
void subFeature(Accumulator& acc, int perspective, int feature);

// SIMD (AVX2) paths are used automatically when the CPU supports them; the results are bit-identical to the
// plain C++ code. setSimd(false) forces the plain code (for tests and comparisons).
void setSimd(bool on);
bool simdActive();

// Score in centipawns from the point of view of the side to move (0 white, 1 black).
int evaluate(const Accumulator& acc, int stm);

}  // namespace nnue