#include "eval.hpp"
#include "tables.hpp"
#include <algorithm>
#include <vectors>
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
inline U64 rankBB(int r) { return 0xFFULL << (8 * r); }
inline int cheb(int a, int b) { return std::max(std::abs((a & 7) - (b & 7)), std::abs((a >> 3) - (b >> 3))); }

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

// ---------- additional terms (centipawns) ----------
const int OUTPOST_MG = 22, OUTPOST_EG = 12;               // knight on rank 4-6, pawn-supported, no enemy pawn can chase it
const int BACKWARD_MG = 9, BACKWARD_EG = 14;              // pawn that cannot be supported and whose stop square is pawn-attacked
const int CONN_MG[8] = {0, 2, 4, 6, 10, 18, 30, 0};       // pawn defended by a pawn (full) / side by side with one (half), by rank
const int CONN_EG[8] = {0, 2, 3, 5,  9, 16, 28, 0};
const int ROOK_7TH_MG = 20, ROOK_7TH_EG = 35;             // rook on the 7th with enemy pawns there or the king on the 8th
const int THREAT_PAWN_MG = 30, THREAT_PAWN_EG = 20;       // enemy N/B/R/Q attacked by one of our pawns
const int THREAT_HANG_MG = 15, THREAT_HANG_EG = 12;       // enemy N/B/R/Q attacked by us and not defended
const int THREAT_MINOR_MAJOR_MG = 16, THREAT_MINOR_MAJOR_EG = 10;   // enemy R/Q attacked by our N/B
const int STORM[8] = {0, 30, 20, 12, 6, 2, 0, 0};         // enemy pawn this many ranks in front of our king, per file (mg)
const int CHECK_UNITS_N = 3, CHECK_UNITS_B = 2, CHECK_UNITS_R = 3, CHECK_UNITS_Q = 4;   // safe checks feed the king-danger sum
const int PASSED_KD[8] = {0, 0, 1, 1, 2, 2, 3, 0};        // king-distance weight for passed pawns (endgame)
const U64 LIGHT_SQ = 0x55AA55AA55AA55AAULL;

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
    U64 attN[2] = {0, 0}, attB[2] = {0, 0}, attR[2] = {0, 0}, attQ[2] = {0, 0};   // squares attacked, by piece type

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
            const U64 sqb = 1ULL << sq;
            const int stop = c == 0 ? sq + 8 : sq - 8;          // pawns are never on the first/last rank
            if (!(M.passed[c][sq] & pcs[e][0])) {
                // passed pawn: worth less when blocked (a blockading piece is worst), more with the kings placed well
                int pm = PASSED_MG[rel], pe = PASSED_EG[rel];
                if (side[e] & (1ULL << stop))      { pm /= 2; pe /= 2; }
                else if (side[c] & (1ULL << stop)) { pm = pm * 3 / 4; pe = pe * 3 / 4; }
                mg[c] += pm; eg[c] += pe;
                if (kingSq[0] >= 0 && kingSq[1] >= 0)
                    eg[c] += PASSED_KD[rel] * (2 * std::min(cheb(kingSq[e], stop), 6) - std::min(cheb(kingSq[c], stop), 6));
            }
            const U64 friendsAdj = M.adj[f] & pcs[c][0];
            if (!friendsAdj)                    { mg[c] -= ISOLATED_MG;    eg[c] -= ISOLATED_EG; }
            if (fileBB(f) & M.ahead[c][r] & pcs[c][0]) { mg[c] -= DOUBLED_MG; eg[c] -= DOUBLED_EG; }
            // backward: has neighbours, but none level or behind it, and an enemy pawn controls the stop square
            if (friendsAdj && !(friendsAdj & (M.ahead[e][r] | rankBB(r))) && (pawnAtt[e] & (1ULL << stop))) {
                mg[c] -= BACKWARD_MG; eg[c] -= BACKWARD_EG;
            }
            // connected: defended by a pawn, and/or side by side with one
            if (pawnAtt[c] & sqb)           { mg[c] += CONN_MG[rel];     eg[c] += CONN_EG[rel]; }
            if (friendsAdj & rankBB(r))     { mg[c] += CONN_MG[rel] / 2; eg[c] += CONN_EG[rel] / 2; }
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
            attN[c] |= att;
            {   // outpost: ranks 4-6, defended by a pawn, and no enemy pawn on a neighbouring file can ever attack it
                const int r = sq >> 3, f = sq & 7, rel = c == 0 ? r : 7 - r;
                if (rel >= 3 && rel <= 5 && (pawnAtt[c] & (1ULL << sq)) && !(M.adj[f] & M.ahead[c][r] & pcs[e][0])) {
                    mg[c] += OUTPOST_MG; eg[c] += OUTPOST_EG;
                }
            }
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
            attB[c] |= att;
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
            attR[c] |= att;
            {   // rook on the 7th: attacks pawns there and/or pins the king to the back rank
                const int r = sq >> 3, rel = c == 0 ? r : 7 - r;
                if (rel == 6 && ((pcs[e][0] & rankBB(r)) || (kingSq[e] >= 0 && (kingSq[e] >> 3) == (c == 0 ? 7 : 0)))) {
                    mg[c] += ROOK_7TH_MG; eg[c] += ROOK_7TH_EG;
                }
            }
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
            attQ[c] |= att;
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
                // pawn storm: nearest enemy pawn in front of the king on this file; less dangerous behind our own pawn
                const U64 stormers = fileBB(ff) & M.ahead[c][r] & pcs[e][0];
                if (stormers) {
                    const int pr = c == 0 ? (__builtin_ctzll(stormers) >> 3) : ((63 - __builtin_clzll(stormers)) >> 3);
                    int pen = STORM[std::min(std::abs(pr - r), 7)];
                    if (fileBB(ff) & M.ahead[c][r] & pcs[c][0]) pen /= 2;
                    mg[c] -= pen;
                }
            }
        }
    }

    // full attack maps per side
    U64 attAll[2];
    for (int c = 0; c < 2; ++c)
        attAll[c] = pawnAtt[c] | attN[c] | attB[c] | attR[c] | attQ[c] |
                    (kingSq[c] >= 0 ? attacks::king(Square(kingSq[c])).getBits() : 0);

    for (int c = 0; c < 2; ++c) {
        const int e = c ^ 1;

        // threats against the enemy's knights, bishops, rooks and queens
        const U64 targets = pcs[e][1] | pcs[e][2] | pcs[e][3] | pcs[e][4];
        int n = popcnt(targets & pawnAtt[c]);                                       // attacked by a pawn
        mg[c] += n * THREAT_PAWN_MG; eg[c] += n * THREAT_PAWN_EG;
        n = popcnt(targets & attAll[c] & ~attAll[e] & ~pawnAtt[c]);                 // attacked and hanging
        mg[c] += n * THREAT_HANG_MG; eg[c] += n * THREAT_HANG_EG;
        n = popcnt((pcs[e][3] | pcs[e][4]) & (attN[c] | attB[c]) & ~pawnAtt[c]);    // rook/queen hit by a minor piece
        mg[c] += n * THREAT_MINOR_MAJOR_MG; eg[c] += n * THREAT_MINOR_MAJOR_EG;

        // safe checks: squares from which one of our pieces would check their king, that we can reach
        // and that their pieces do not cover. They count as attackers in the king-danger sum below.
        if (kingSq[e] >= 0) {
            const Square ks(kingSq[e]);
            const U64 safe = ~side[c] & ~attAll[e];
            const U64 kn = attacks::knight(ks).getBits();
            const U64 bq = attacks::bishop(ks, Bitboard(occ)).getBits();
            const U64 rq = attacks::rook(ks, Bitboard(occ)).getBits();
            if (attN[c] & kn & safe)        { atkUnits[e] += CHECK_UNITS_N; ++atkCount[e]; }
            if (attB[c] & bq & safe)        { atkUnits[e] += CHECK_UNITS_B; ++atkCount[e]; }
            if (attR[c] & rq & safe)        { atkUnits[e] += CHECK_UNITS_R; ++atkCount[e]; }
            if (attQ[c] & (bq | rq) & safe) { atkUnits[e] += CHECK_UNITS_Q; ++atkCount[e]; }
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

    // drawish endgames: shrink the endgame score of the side that is ahead (sf out of 64)
    {
        const int strong = egScore > 0 ? 0 : 1, weak = strong ^ 1;
        const int pawnsStrong = popcnt(pcs[strong][0]);
        int np[2];                                              // non-pawn material
        for (int c = 0; c < 2; ++c) {
            np[c] = 0;
            for (int pt = 1; pt < 5; ++pt) np[c] += popcnt(pcs[c][pt]) * MG_VAL[pt];
        }
        int sf = 64;
        const bool ocb = popcnt(pcs[0][2]) == 1 && popcnt(pcs[1][2]) == 1 &&
                         ((pcs[0][2] & LIGHT_SQ) != 0) != ((pcs[1][2] & LIGHT_SQ) != 0);
        if (ocb && !(pcs[0][1] | pcs[1][1] | pcs[0][3] | pcs[1][3] | pcs[0][4] | pcs[1][4]))
            sf = std::min(64, 22 + 3 * pawnsStrong);            // opposite-coloured bishops only
        else if (ocb && !(pcs[0][1] | pcs[1][1] | pcs[0][4] | pcs[1][4]) &&
                 popcnt(pcs[0][3]) == 1 && popcnt(pcs[1][3]) == 1)
            sf = 48;                                            // ... with one rook each
        else if (pawnsStrong == 0 && np[strong] - np[weak] <= 340)
            sf = 6;                                             // no pawns and at most a minor piece ahead
        else if (pawnsStrong == 0 && np[weak] == 0 && !pcs[weak][0] && np[strong] == 2 * MG_VAL[1] &&
                 popcnt(pcs[strong][1]) == 2)
            sf = 6;                                             // two knights against a bare king
        egScore = egScore * sf / 64;
    }
    return (mgScore * ph + egScore * (24 - ph)) / 24;
}

// ---------- evaluation cache ----------
// The evaluation depends only on the position, and the search keeps meeting the same positions again.
// Index = low hash bits, check = high 32 bits.
namespace {
struct EvalEntry { uint32_t check; int32_t score; };
constexpr size_t EVAL_CACHE_SIZE = 1 << 20;                      // 8 MB
std::vector<EvalEntry> evalCache(EVAL_CACHE_SIZE, EvalEntry{0, 0});
}

int evalSide(const Board& b) {
    const uint64_t key = b.hash();
    EvalEntry& e = evalCache[key & (EVAL_CACHE_SIZE - 1)];
    const uint32_t check = (uint32_t)(key >> 32) | 1u;           // "| 1" keeps an empty slot from matching
    if (e.check == check) return e.score;
    int s = evaluate(b);
    s = (b.sideToMove() == Color::WHITE ? s : -s) + TEMPO;
    e.check = check;
    e.score = s;
    return s;
}
}