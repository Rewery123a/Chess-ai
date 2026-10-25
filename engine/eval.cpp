#include "eval.hpp"
#include "tables.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>

using namespace chess;

namespace {

typedef uint64_t U64;

constexpr U64 FILE_A = 0x0101010101010101ULL;
constexpr U64 FILE_H = FILE_A << 7;

inline int popcnt(U64 b) { return __builtin_popcountll(b); }
inline int popLsb(U64& b) { int s = __builtin_ctzll(b); b &= b - 1; return s; }
inline U64 fileBB(int f) { return FILE_A << f; }

// ---------- tuning knobs (centipawns) ----------
const int MG_VAL[5] = {100, 320, 330, 500, 900};     // P N B R Q, middlegame
const int EG_VAL[5] = {115, 310, 330, 530, 940};     // endgame

const int PASSED_MG[8] = {0,  5, 10, 20,  35,  60, 100, 0};   // by rank (from own side)
const int PASSED_EG[8] = {0, 10, 20, 40,  70, 120, 200, 0};

const int ISOLATED_MG = 12, ISOLATED_EG = 18;
const int DOUBLED_MG  = 10, DOUBLED_EG  = 20;
const int BISHOP_PAIR_MG = 30, BISHOP_PAIR_EG = 50;
const int ROOK_OPEN_MG = 25, ROOK_OPEN_EG = 15;
const int ROOK_SEMI_MG = 12, ROOK_SEMI_EG = 8;
const int SHIELD_PAWN = 8;                   // per pawn in front of a castled king (max 4)
const int OPEN_FILE_KING = 20, SEMI_FILE_KING = 12;
const int TEMPO = 10;

const int PHASE_W[5] = {0, 1, 1, 2, 4};      // total 24 at the start

const PieceType PT[6] = {PieceType::PAWN, PieceType::KNIGHT, PieceType::BISHOP,
                         PieceType::ROOK, PieceType::QUEEN, PieceType::KING};

// ---------- precomputed masks ----------
struct Masks {
    U64 adj[8];            // files next to file f
    U64 ahead[2][8];       // all squares strictly ahead of rank r, from that colour's view
    U64 passed[2][64];     // squares that must hold no enemy pawn for a pawn to be passed
    Masks() {
        for (int f = 0; f < 8; ++f) {
            adj[f] = 0;
            if (f > 0) adj[f] |= fileBB(f - 1);
            if (f < 7) adj[f] |= fileBB(f + 1);
        }
        for (int r = 0; r < 8; ++r) {
            ahead[0][r] = r < 7 ? (~0ULL << (8 * (r + 1))) : 0;
            ahead[1][r] = r > 0 ? ((1ULL << (8 * r)) - 1) : 0;
        }
        for (int c = 0; c < 2; ++c)
            for (int sq = 0; sq < 64; ++sq) {
                int f = sq & 7, r = sq >> 3;
                passed[c][sq] = (fileBB(f) | adj[f]) & ahead[c][r];
            }
    }
};
const Masks M;

inline U64 pawnAttacks(U64 p, int c) {
    if (c == 0) return ((p << 7) & ~FILE_H) | ((p << 9) & ~FILE_A);
    return ((p >> 9) & ~FILE_H) | ((p >> 7) & ~FILE_A);
}

} // namespace

// Score from White's point of view, blending middlegame and endgame by material left.
int evaluate(const Board& b) {
    U64 pcs[2][6];
    U64 side[2] = {0, 0};
    for (int c = 0; c < 2; ++c) {
        Color col = c == 0 ? Color::WHITE : Color::BLACK;
        for (int pt = 0; pt < 6; ++pt) {
            pcs[c][pt] = b.pieces(PT[pt], col).getBits();
            side[c] |= pcs[c][pt];
        }
    }
    const U64 occ = side[0] | side[1];

    int mg[2] = {0, 0}, eg[2] = {0, 0};
    int phase = 0;
    int atkUnits[2] = {0, 0}, atkCount[2] = {0, 0};   // attackers ON colour c's king zone
    int kingSq[2];
    U64 kingZone[2];
    for (int c = 0; c < 2; ++c) {
        kingSq[c] = pcs[c][5] ? __builtin_ctzll(pcs[c][5]) : -1;
        kingZone[c] = kingSq[c] >= 0 ? (attacks::king(Square(kingSq[c])).getBits() | (1ULL << kingSq[c])) : 0;
    }
    const U64 pawnAtt[2] = {pawnAttacks(pcs[0][0], 0), pawnAttacks(pcs[1][0], 1)};

    for (int c = 0; c < 2; ++c) {
        const int e = c ^ 1;
        const U64 area = ~side[c] & ~pawnAtt[e];      // squares worth counting for mobility
        U64 bb;

        // pawns
        bb = pcs[c][0];
        while (bb) {
            int sq = popLsb(bb), f = sq & 7, r = sq >> 3;
            int rel = c == 0 ? r : 7 - r;
            int psq = c == 0 ? (sq ^ 56) : sq;
            mg[c] += MG_VAL[0] + PAWN_PST[psq];
            eg[c] += EG_VAL[0] + PAWN_PST[psq];
            if (!(M.passed[c][sq] & pcs[e][0])) { mg[c] += PASSED_MG[rel]; eg[c] += PASSED_EG[rel]; }
            if (!(M.adj[f] & pcs[c][0]))        { mg[c] -= ISOLATED_MG;    eg[c] -= ISOLATED_EG; }
            if (fileBB(f) & M.ahead[c][r] & pcs[c][0]) { mg[c] -= DOUBLED_MG; eg[c] -= DOUBLED_EG; }
        }

        // knights
        bb = pcs[c][1];
        while (bb) {
            int sq = popLsb(bb), psq = c == 0 ? (sq ^ 56) : sq;
            U64 att = attacks::knight(Square(sq)).getBits();
            int v = KNIGHT_PST[psq] + (popcnt(att & area) - 4) * 4;
            mg[c] += MG_VAL[1] + v;
            eg[c] += EG_VAL[1] + v;
            phase += PHASE_W[1];
            if (att & kingZone[e]) { atkUnits[e] += 2; ++atkCount[e]; }
        }

        // bishops
        bb = pcs[c][2];
        while (bb) {
            int sq = popLsb(bb), psq = c == 0 ? (sq ^ 56) : sq;
            U64 att = attacks::bishop(Square(sq), Bitboard(occ)).getBits();
            int v = BISHOP_PST[psq] + (popcnt(att & area) - 6) * 5;
            mg[c] += MG_VAL[2] + v;
            eg[c] += EG_VAL[2] + v;
            phase += PHASE_W[2];
            if (att & kingZone[e]) { atkUnits[e] += 2; ++atkCount[e]; }
        }
        if (popcnt(pcs[c][2]) >= 2) { mg[c] += BISHOP_PAIR_MG; eg[c] += BISHOP_PAIR_EG; }

        // rooks
        bb = pcs[c][3];
        while (bb) {
            int sq = popLsb(bb), f = sq & 7, psq = c == 0 ? (sq ^ 56) : sq;
            U64 att = attacks::rook(Square(sq), Bitboard(occ)).getBits();
            int m = popcnt(att & area) - 7;
            mg[c] += MG_VAL[3] + ROOK_PST[psq] + m * 2;
            eg[c] += EG_VAL[3] + ROOK_PST[psq] + m * 4;
            phase += PHASE_W[3];
            if (!(fileBB(f) & pcs[c][0])) {
                if (!(fileBB(f) & pcs[e][0])) { mg[c] += ROOK_OPEN_MG; eg[c] += ROOK_OPEN_EG; }
                else                           { mg[c] += ROOK_SEMI_MG; eg[c] += ROOK_SEMI_EG; }
            }
            if (att & kingZone[e]) { atkUnits[e] += 3; ++atkCount[e]; }
        }

        // queens
        bb = pcs[c][4];
        while (bb) {
            int sq = popLsb(bb), psq = c == 0 ? (sq ^ 56) : sq;
            U64 att = attacks::bishop(Square(sq), Bitboard(occ)).getBits() |
                      attacks::rook(Square(sq), Bitboard(occ)).getBits();
            int m = popcnt(att & area) - 14;
            mg[c] += MG_VAL[4] + QUEEN_PST[psq] + m * 1;
            eg[c] += EG_VAL[4] + QUEEN_PST[psq] + m * 2;
            phase += PHASE_W[4];
            if (att & kingZone[e]) { atkUnits[e] += 5; ++atkCount[e]; }
        }

        // king: piece-square (tapered), pawn shield and open files (middlegame only)
        if (kingSq[c] >= 0) {
            int ksq = kingSq[c], f = ksq & 7, r = ksq >> 3;
            int psq = c == 0 ? (ksq ^ 56) : ksq;
            mg[c] += KING_MID_PST[psq];
            eg[c] += KING_END_PST[psq];

            int rel = c == 0 ? r : 7 - r;
            if (rel <= 2) {
                int r2 = c == 0 ? std::min(r + 2, 7) : std::max(r - 2, 0);
                U64 shieldRanks = M.ahead[c][r] & ~M.ahead[c][r2];
                int shield = popcnt(pcs[c][0] & (fileBB(f) | M.adj[f]) & shieldRanks);
                mg[c] += SHIELD_PAWN * std::min(shield, 4);
            }
            for (int ff = std::max(0, f - 1); ff <= std::min(7, f + 1); ++ff) {
                if (!(fileBB(ff) & pcs[c][0]))
                    mg[c] -= (fileBB(ff) & pcs[e][0]) ? SEMI_FILE_KING : OPEN_FILE_KING;
            }
        }
    }

    // pieces attacking the king zone (needs 2+ attackers to matter)
    for (int c = 0; c < 2; ++c) {
        if (atkCount[c] >= 2) {
            int p = std::min(atkUnits[c] * atkUnits[c] * 2 / 3, 250);
            if (!pcs[c ^ 1][4]) p /= 2;                  // much less dangerous without a queen
            mg[c] -= p;
        }
    }

    // mop-up: a clearly winning side in a thin endgame drives the enemy king to the edge
    int mat[2];
    for (int c = 0; c < 2; ++c) {
        mat[c] = 0;
        for (int pt = 0; pt < 5; ++pt) mat[c] += popcnt(pcs[c][pt]) * MG_VAL[pt];
    }
    int diff = mat[0] - mat[1];
    if (std::abs(diff) >= 300 && phase <= 10 && kingSq[0] >= 0 && kingSq[1] >= 0) {
        int w = diff > 0 ? 0 : 1, l = w ^ 1;
        int lf = kingSq[l] & 7, lr = kingSq[l] >> 3, wf = kingSq[w] & 7, wr = kingSq[w] >> 3;
        int cornerDist = std::max(3 - lf, lf - 4) + std::max(3 - lr, lr - 4);   // 0 centre .. 6 corner
        int kingDist = std::abs(lf - wf) + std::abs(lr - wr);
        eg[w] += 6 * cornerDist + 3 * (14 - kingDist);
    }

    int ph = std::min(phase, 24);
    int mgScore = mg[0] - mg[1];
    int egScore = eg[0] - eg[1];
    return (mgScore * ph + egScore * (24 - ph)) / 24;
}

int evalSide(const Board& b) {
    int s = evaluate(b);
    return (b.sideToMove() == Color::WHITE ? s : -s) + TEMPO;
}