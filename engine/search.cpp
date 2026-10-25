#include "search.hpp"
#include "eval.hpp"
#include "tables.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace chess;
using Clock = std::chrono::steady_clock;

static const int MATE = 100000, MATE_BOUND = MATE - 512, INF = 1000000;
static const int MAX_PLY = 128;

// aspiration window settings (centipawns)
static const int ASP_MIN_DEPTH = 5;
static const int ASP_DELTA     = 25;
static const int ASP_MAX_DELTA = 800;

// quiescence pruning settings
static const int DELTA_MARGIN = 200;
static const int BIG_DELTA    = 900 + DELTA_MARGIN;

// move-ordering score bands (Move::score is int16, so everything must stay < 32767)
static const int S_TT      = 32000;
static const int S_CAPTURE = 20000;   // + MVV-LVA
static const int S_KILLER1 = 15000;
static const int S_KILLER2 = 14000;
static const int MAX_HIST  = 8192;    // quiet history lives in [-MAX_HIST, MAX_HIST]

// null move / LMR settings
static const int NMP_MIN_DEPTH = 3;
static const int LMR_MIN_DEPTH = 3;
static const int LMR_MIN_MOVES = 3;

enum Flag : uint8_t { EXACT, LOWER, UPPER };

// ---------- transposition table ----------
struct Entry {
    uint64_t key = 0;
    Move move{};
    int score = 0;
    int depth = -1;
    uint8_t flag = EXACT;
};
static std::atomic<bool> stopFlag{false};
void requestStop() { stopFlag = true; }
void resetStop() { stopFlag = false; }

static const uint64_t TT_SIZE = 1 << 22
static std::vector<Entry> TT(TT_SIZE);

// ---------- ordering heuristics state ----------
static Move killers[MAX_PLY + 1][2];
static int  history[2][64][64];          // [side][from][to]
static int  LMR[64][64];                 // [depth][moveCount]
static bool lmrReady = false;
static bool printInfo = true;

static void initLMR() {
    if (lmrReady) return;
    for (int d = 1; d < 64; ++d)
        for (int m = 1; m < 64; ++m)
            LMR[d][m] = (int)(0.75 + std::log((double)d) * std::log((double)m) / 2.25);
    lmrReady = true;
}

void clearTT() {
    std::fill(TT.begin(), TT.end(), Entry());
    std::memset(history, 0, sizeof(history));
    for (auto& k : killers) k[0] = k[1] = Move();
}

void setSearchInfo(bool on) { printInfo = on; }

// mate scores are stored relative to the node, not the root
static int toTT(int s, int ply)   { return s >= MATE_BOUND ? s + ply : (s <= -MATE_BOUND ? s - ply : s); }
static int fromTT(int s, int ply) { return s >= MATE_BOUND ? s - ply : (s <= -MATE_BOUND ? s + ply : s); }

// ---------- time control ----------
struct Timeout {};
static Clock::time_point deadline;
static uint64_t nodes = 0;

static inline void checkTime() {
    if ((++nodes & 2047) == 0 && (stopFlag.load(std::memory_order_relaxed) || Clock::now() > deadline)) throw Timeout();
}

static inline int sideIdx(const Board& b) { return b.sideToMove() == Color::WHITE ? 0 : 1; }

static void updateHistory(int side, const Move& m, int bonus) {
    int& h = history[side][m.from().index()][m.to().index()];
    h += bonus - h * std::abs(bonus) / MAX_HIST;      // "gravity": h stays within +-MAX_HIST
}

// ---------- move ordering ----------
// tt move > captures (MVV-LVA) > killers > quiet moves by history.
// Only assigns scores; callers use pickMove() so we never sort the whole list.
static void orderMoves(const Board& b, Movelist& moves, Move ttMove, int ply) {
    const int side = sideIdx(b);
    for (Move& m : moves) {
        int s;
        if (m == ttMove) {
            s = S_TT;
        } else if (b.isCapture(m)) {
            int v = (int)b.at(m.to()).type();
            if (v > 5) v = 0;                           // en passant
            int a = (int)b.at(m.from()).type();
            s = S_CAPTURE + 100 + 10 * ORDER_VALUE[v] - ORDER_VALUE[a];
            if (m.typeOf() == Move::PROMOTION) s += 80;
        } else if (m.typeOf() == Move::PROMOTION) {
            s = S_CAPTURE + 80;
        } else if (m == killers[ply][0]) {
            s = S_KILLER1;
        } else if (m == killers[ply][1]) {
            s = S_KILLER2;
        } else {
            s = history[side][m.from().index()][m.to().index()];
        }
        m.setScore((int16_t)s);
    }
}

// selection step: move the best remaining move to index i (cheaper than sorting, since
// most nodes cut off after 1-3 moves)
static inline void pickMove(Movelist& moves, int i) {
    int bi = i;
    for (int j = i + 1; j < moves.size(); ++j)
        if (moves[j].score() > moves[bi].score()) bi = j;
    if (bi != i) std::swap(moves[i], moves[bi]);
}

// ---------- static exchange evaluation (swap algorithm, with x-rays) ----------
static const int SEE_VAL[6] = {100, 320, 330, 500, 900, 20000};
static const PieceType PT_LIST[6] = {PieceType::PAWN, PieceType::KNIGHT, PieceType::BISHOP,
                                     PieceType::ROOK, PieceType::QUEEN, PieceType::KING};

static int see(const Board& b, const Move& m) {
    const Square toSq = m.to();
    const int to = toSq.index();
    const bool ep = m.typeOf() == Move::ENPASSANT;
    const bool whiteMoves = b.sideToMove() == Color::WHITE;

    uint64_t pc[2][6];
    for (int pt = 0; pt < 6; ++pt) {
        pc[0][pt] = b.pieces(PT_LIST[pt], Color::WHITE).getBits();
        pc[1][pt] = b.pieces(PT_LIST[pt], Color::BLACK).getBits();
    }

    auto attackersTo = [&](uint64_t occ) -> uint64_t {
        Bitboard o(occ);
        uint64_t r = 0;
        r |= attacks::pawn(Color::BLACK, toSq).getBits() & pc[0][0];
        r |= attacks::pawn(Color::WHITE, toSq).getBits() & pc[1][0];
        r |= attacks::knight(toSq).getBits() & (pc[0][1] | pc[1][1]);
        r |= attacks::bishop(toSq, o).getBits() & (pc[0][2] | pc[1][2] | pc[0][4] | pc[1][4]);
        r |= attacks::rook(toSq, o).getBits()   & (pc[0][3] | pc[1][3] | pc[0][4] | pc[1][4]);
        r |= attacks::king(toSq).getBits() & (pc[0][5] | pc[1][5]);
        return r & occ;
    };

    int victim = ep ? 0 : (int)b.at(toSq).type();
    if (victim > 5) victim = 0;
    int aType = (int)b.at(m.from()).type();

    uint64_t occ = b.occ().getBits();
    if (ep) occ &= ~(1ULL << (whiteMoves ? to - 8 : to + 8));

    int gain[40];
    int d = 0;
    gain[0] = SEE_VAL[victim];
    uint64_t fromSet = 1ULL << m.from().index();
    int side = whiteMoves ? 0 : 1;

    do {
        ++d;
        gain[d] = SEE_VAL[aType] - gain[d - 1];
        if (std::max(-gain[d - 1], gain[d]) < 0) break;
        occ &= ~fromSet;
        uint64_t att = attackersTo(occ);
        side ^= 1;
        fromSet = 0;
        for (int pt = 0; pt < 6; ++pt) {
            uint64_t x = att & pc[side][pt];
            if (x) { fromSet = x & (~x + 1); aType = pt; break; }
        }
    } while (fromSet);

    while (--d) gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
    return gain[0];
}

// ---------- quiescence search ----------
static int quiesce(Board& b, int alpha, int beta, int ply) {
    checkTime();
    // isRepetition(1): a single earlier occurrence already counts as a draw inside the search
    if (b.isInsufficientMaterial() || b.isHalfMoveDraw() || b.isRepetition(1)) return 0;
    if (ply >= MAX_PLY) return evalSide(b);

    const bool inCheck = b.inCheck();
    const int alphaOrig = alpha;

    uint64_t key = b.hash();
    Entry& e = TT[key & (TT_SIZE - 1)];
    Move ttMove{};
    if (e.key == key) {
        ttMove = e.move;
        int s = fromTT(e.score, ply);
        if (e.flag == EXACT) return s;
        if (e.flag == LOWER && s >= beta) return s;
        if (e.flag == UPPER && s <= alpha) return s;
    }

    Movelist moves;
    int stand = -INF, best;
    if (inCheck) {
        movegen::legalmoves(moves, b);
        if (moves.empty()) return -(MATE - ply);
        best = -INF;
    } else {
        stand = evalSide(b);
        if (stand >= beta) return stand;
        if (stand + BIG_DELTA < alpha) return stand + BIG_DELTA;
        if (stand > alpha) alpha = stand;
        best = stand;
        movegen::legalmoves<movegen::MoveGenType::CAPTURE>(moves, b);
    }
    orderMoves(b, moves, ttMove, ply);

    Move bestMove{};
    for (int i = 0; i < moves.size(); ++i) {
        pickMove(moves, i);
        const Move m = moves[i];
        if (!inCheck) {
            const bool promo = m.typeOf() == Move::PROMOTION;
            int victim = (m.typeOf() == Move::ENPASSANT) ? 0 : (int)b.at(m.to()).type();
            if (victim > 5) victim = 0;
            int attacker = (int)b.at(m.from()).type();

            int gain = PIECE_VALUE[victim];
            if (promo) gain += PIECE_VALUE[(int)m.promotionType()] - PIECE_VALUE[0];
            if (stand + gain + DELTA_MARGIN < alpha) continue;

            if (!promo && ORDER_VALUE[victim] < ORDER_VALUE[attacker] && see(b, m) < 0) continue;
        }

        b.makeMove(m);
        int s = -quiesce(b, -beta, -alpha, ply + 1);
        b.unmakeMove(m);

        if (s > best) { best = s; bestMove = m; }
        if (s > alpha) alpha = s;
        if (alpha >= beta) break;
    }

    // qsearch entries are depth 0: never let them evict a real (depth > 0) entry,
    // otherwise the millions of qsearch nodes wipe out the useful deep entries.
    if (e.depth <= 0) {
        e.key = key;
        e.move = bestMove;
        e.score = toTT(best, ply);
        e.depth = 0;
        e.flag = best <= alphaOrig ? UPPER : (best >= beta ? LOWER : EXACT);
    }
    return best;
}

static bool hasNonPawnMaterial(const Board& b) {
    const Color c = b.sideToMove();
    return (b.pieces(PieceType::KNIGHT, c).getBits() | b.pieces(PieceType::BISHOP, c).getBits() |
            b.pieces(PieceType::ROOK, c).getBits()   | b.pieces(PieceType::QUEEN, c).getBits()) != 0;
}

// ---------- main search: negamax + PVS + TT + NMP + LMR + killers/history ----------
static int negamax(Board& b, int depth, int alpha, int beta, int ply, bool canNull) {
    checkTime();
    if (b.isInsufficientMaterial() || b.isHalfMoveDraw() || b.isRepetition(1)) return 0;
    if (ply >= MAX_PLY) return evalSide(b);

    const bool inCheck = b.inCheck();
    if (inCheck) ++depth;
    if (depth <= 0) return quiesce(b, alpha, beta, ply);

    const bool pvNode = beta - alpha > 1;

    uint64_t key = b.hash();
    Entry& e = TT[key & (TT_SIZE - 1)];
    const int alphaOrig = alpha;
    Move ttMove{};
    if (e.key == key) {
        ttMove = e.move;
        // only cut in non-PV nodes: keeps the PV exact and avoids TT-induced oddities
        if (!pvNode && e.depth >= depth) {
            int s = fromTT(e.score, ply);
            if (e.flag == EXACT) return s;
            if (e.flag == LOWER && s >= beta) return s;
            if (e.flag == UPPER && s <= alpha) return s;
        }
    }

    // --- null move pruning: if passing still beats beta, this node is almost surely a cut ---
    if (!pvNode && canNull && !inCheck && depth >= NMP_MIN_DEPTH &&
        std::abs(beta) < MATE_BOUND && hasNonPawnMaterial(b) && evalSide(b) >= beta) {
        int R = 2 + depth / 4;
        b.makeNullMove();
        int s = -negamax(b, depth - 1 - R, -beta, -beta + 1, ply + 1, false);
        b.unmakeNullMove();
        if (s >= beta) return s >= MATE_BOUND ? beta : s;     // never trust unproven mate scores
    }

    Movelist moves;
    movegen::legalmoves(moves, b);
    if (moves.empty()) return inCheck ? -(MATE - ply) : 0;
    orderMoves(b, moves, ttMove, ply);

    const int side = sideIdx(b);
    int best = -INF;
    Move bestMove{};
    Move quiets[64];
    int nq = 0;

    for (int i = 0; i < moves.size(); ++i) {
        pickMove(moves, i);
        const Move m = moves[i];
        const bool quiet = !b.isCapture(m) && m.typeOf() != Move::PROMOTION;
        const bool isKiller = m == killers[ply][0] || m == killers[ply][1];

        b.makeMove(m);
        int s;
        if (i == 0) {
            s = -negamax(b, depth - 1, -beta, -alpha, ply + 1, true);
        } else {
            // late move reduction: late quiet moves are probably bad, so search them shallower
            int r = 0;
            if (depth >= LMR_MIN_DEPTH && i >= LMR_MIN_MOVES && quiet && !inCheck && !b.inCheck()) {
                r = LMR[std::min(depth, 63)][std::min(i, 63)];
                if (pvNode) --r;
                if (isKiller) --r;
                r = std::max(0, std::min(r, depth - 2));
            }
            s = -negamax(b, depth - 1 - r, -alpha - 1, -alpha, ply + 1, true);
            if (s > alpha && r > 0)                        // reduced search surprised us: verify at full depth
                s = -negamax(b, depth - 1, -alpha - 1, -alpha, ply + 1, true);
            if (s > alpha && s < beta)                     // PVS re-search with the full window
                s = -negamax(b, depth - 1, -beta, -alpha, ply + 1, true);
        }
        b.unmakeMove(m);

        if (s > best) { best = s; bestMove = m; }
        if (s > alpha) alpha = s;
        if (alpha >= beta) {
            if (quiet) {
                if (!(m == killers[ply][0])) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
                int bonus = std::min(depth * depth, 1200);
                updateHistory(side, m, bonus);
                for (int q = 0; q < nq; ++q) updateHistory(side, quiets[q], -bonus);   // penalise quiets that failed
            }
            break;
        }
        if (quiet && nq < 64) quiets[nq++] = m;
    }

    // keep a deeper entry for the same position
    if (e.key != key || depth >= e.depth) {
        e.key = key;
        e.move = bestMove;
        e.score = toTT(best, ply);
        e.depth = depth;
        e.flag = best <= alphaOrig ? UPPER : (best >= beta ? LOWER : EXACT);
    }
    return best;
}

static std::string scoreStr(int s) {
    if (s >= MATE_BOUND)  return "mate " + std::to_string((MATE - s + 1) / 2);
    if (s <= -MATE_BOUND) return "mate -" + std::to_string((MATE + s) / 2);
    return "cp " + std::to_string(s);
}

// ---------- root search (PVS) ----------
static int rootSearch(Board& b, Movelist& moves, int depth, int alpha, int beta, Move& best) {
    orderMoves(b, moves, best, 0);
    std::stable_sort(moves.begin(), moves.end(),
                     [](const Move& x, const Move& y) { return x.score() > y.score(); });
    int bestScore = -INF;
    bool first = true;
    for (const Move& m : moves) {
        b.makeMove(m);
        int s;
        if (first) {
            s = -negamax(b, depth - 1, -beta, -alpha, 1, true);
        } else {
            s = -negamax(b, depth - 1, -alpha - 1, -alpha, 1, true);
            if (s > alpha && s < beta)
                s = -negamax(b, depth - 1, -beta, -alpha, 1, true);
        }
        b.unmakeMove(m);
        first = false;

        if (s > bestScore) bestScore = s;
        if (s > alpha) {
            alpha = s;
            best = m;
            if (alpha >= beta) break;
        }
    }
    return bestScore;
}

// ---------- iterative deepening with aspiration windows and a time limit ----------
// Note: on timeout the board passed in is left mid-search, so callers must pass a copy.
Move search(Board& b, int maxDepth, int ms) {
    initLMR();
    auto start = Clock::now();
    deadline = start + std::chrono::milliseconds(ms);
    nodes = 0;
    for (auto& k : killers) k[0] = k[1] = Move();
    for (auto& s : history) for (auto& f : s) for (int& h : f) h /= 2;   // age, don't wipe

    Movelist moves;
    movegen::legalmoves(moves, b);
    if (moves.empty()) return Move();
    if (moves.size() == 1) return moves[0];
    Move best = moves[0];
    int prevScore = 0;

    for (int d = 1; d <= maxDepth; ++d) {
        try {
            int alpha = -INF, beta = INF, delta = ASP_DELTA;
            if (d >= ASP_MIN_DEPTH && std::abs(prevScore) < MATE_BOUND) {
                alpha = std::max(prevScore - delta, -INF);
                beta  = std::min(prevScore + delta,  INF);
            }

            int score;
            while (true) {
                score = rootSearch(b, moves, d, alpha, beta, best);
                if (score <= alpha) {
                    beta  = (alpha + beta) / 2;
                    alpha = std::max(score - delta, -INF);
                } else if (score >= beta) {
                    beta = std::min(score + delta, INF);
                } else {
                    break;
                }
                delta += delta / 2;
                if (delta > ASP_MAX_DELTA) { alpha = -INF; beta = INF; }
            }

            prevScore = score;
            if (printInfo) {
                long long t = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
                std::cout << "info depth " << d << " score " << scoreStr(score)
                          << " nodes " << nodes << " time " << t
                          << " pv " << uci::moveToUci(best) << std::endl;
            }
            if (score >= MATE_BOUND) break;
        } catch (const Timeout&) {
            break;
        }
    }
    return best;
}