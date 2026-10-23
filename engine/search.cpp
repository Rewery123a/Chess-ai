#include "search.hpp"
#include "eval.hpp"
#include "tables.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace chess;
using Clock = std::chrono::steady_clock;

static const int MATE = 100000, MATE_BOUND = MATE - 512, INF = 1000000;
static const int MAX_PLY = 128;

// aspiration window settings (centipawns)
static const int ASP_MIN_DEPTH = 5;     // below this, search with a full window
static const int ASP_DELTA     = 25;    // initial half-window
static const int ASP_MAX_DELTA = 800;   // once the window grows past this, go full-width

// quiescence pruning settings
static const int DELTA_MARGIN = 200;                       // per-move delta pruning slack
static const int BIG_DELTA    = 900 + DELTA_MARGIN;        // global delta: even a queen capture can't help

enum Flag : uint8_t { EXACT, LOWER, UPPER };

// ---------- transposition table ----------
struct Entry {
    uint64_t key = 0;
    Move move;
    int score = 0;
    int depth = -1;
    uint8_t flag = EXACT;
};
static const uint64_t TT_SIZE = 1 << 21;
static std::vector<Entry> TT(TT_SIZE);

void clearTT() { std::fill(TT.begin(), TT.end(), Entry()); }

// mate scores are stored relative to the node, not the root
static int toTT(int s, int ply)   { return s >= MATE_BOUND ? s + ply : (s <= -MATE_BOUND ? s - ply : s); }
static int fromTT(int s, int ply) { return s >= MATE_BOUND ? s - ply : (s <= -MATE_BOUND ? s + ply : s); }

// ---------- time control ----------
struct Timeout {};
static Clock::time_point deadline;
static uint64_t nodes = 0;

static inline void checkTime() {
    if ((++nodes & 2047) == 0 && Clock::now() > deadline) throw Timeout();
}

// ---------- move ordering: hash move, then captures (big victim, small attacker) ----------
static void orderMoves(const Board& b, Movelist& moves, Move ttMove) {
    for (Move& m : moves) {
        int s = 0;
        if (m == ttMove) {
            s = 30000;
        } else {
            if (b.isCapture(m)) {
                int v = (int)b.at(m.to()).type();
                if (v > 5) v = 0;                       // en passant
                int a = (int)b.at(m.from()).type();
                s += 100 + 10 * ORDER_VALUE[v] - ORDER_VALUE[a];
            }
            if (m.typeOf() == Move::PROMOTION) s += 80;
        }
        m.setScore((int16_t)s);
    }
    std::sort(moves.begin(), moves.end(),
              [](const Move& x, const Move& y) { return x.score() > y.score(); });
}

// ---------- static exchange evaluation (swap algorithm, with x-rays) ----------
// Returns the expected material outcome (centipawns) of the capture sequence on m.to().
static const int SEE_VAL[6] = {100, 320, 330, 500, 900, 20000};
static const PieceType PT_LIST[6] = {PieceType::PAWN, PieceType::KNIGHT, PieceType::BISHOP,
                                     PieceType::ROOK, PieceType::QUEEN, PieceType::KING};

static int see(const Board& b, const Move& m) {
    const Square toSq = m.to();
    const int to = toSq.index();
    const bool ep = m.typeOf() == Move::ENPASSANT;
    const bool whiteMoves = b.sideToMove() == Color::WHITE;

    uint64_t pc[2][6];                                   // [0] white, [1] black
    for (int pt = 0; pt < 6; ++pt) {
        pc[0][pt] = b.pieces(PT_LIST[pt], Color::WHITE).getBits();
        pc[1][pt] = b.pieces(PT_LIST[pt], Color::BLACK).getBits();
    }

    auto attackersTo = [&](uint64_t occ) -> uint64_t {
        Bitboard o(occ);
        uint64_t r = 0;
        r |= attacks::pawn(Color::BLACK, toSq).getBits() & pc[0][0];   // white pawns hitting 'to'
        r |= attacks::pawn(Color::WHITE, toSq).getBits() & pc[1][0];   // black pawns hitting 'to'
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
    if (ep) occ &= ~(1ULL << (whiteMoves ? to - 8 : to + 8));   // captured pawn disappears

    int gain[40];
    int d = 0;
    gain[0] = SEE_VAL[victim];
    uint64_t fromSet = 1ULL << m.from().index();
    int side = whiteMoves ? 0 : 1;

    do {
        ++d;
        gain[d] = SEE_VAL[aType] - gain[d - 1];          // speculative: if the capturer gets recaptured
        if (std::max(-gain[d - 1], gain[d]) < 0) break;  // neither side can improve: stop early
        occ &= ~fromSet;                                 // capturer leaves its square (reveals x-rays)
        uint64_t att = attackersTo(occ);
        side ^= 1;
        fromSet = 0;
        for (int pt = 0; pt < 6; ++pt) {                 // least valuable attacker of the other side
            uint64_t x = att & pc[side][pt];
            if (x) { fromSet = x & (~x + 1); aType = pt; break; }
        }
    } while (fromSet);

    while (--d) gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
    return gain[0];
}

// ---------- quiescence search ----------
// Upgrades over plain capture-only qsearch:
//   * draw detection
//   * transposition table probe/store
//   * check evasions: when in check there is no stand-pat; all legal moves are searched (mate detected)
//   * global delta pruning (stand pat + queen can't reach alpha)
//   * per-move delta pruning (stand pat + victim + margin can't reach alpha)
//   * SEE pruning of losing captures
//   * TT move first, then MVV-LVA ordering
//   * ply cap so it can never run away
static int quiesce(Board& b, int alpha, int beta, int ply) {
    checkTime();
    if (b.isInsufficientMaterial() || b.isHalfMoveDraw() || b.isRepetition()) return 0;
    if (ply >= MAX_PLY) return evalSide(b);

    const bool inCheck = b.inCheck();
    const int alphaOrig = alpha;

    // --- TT probe (anything stored is at least depth 0, so always deep enough) ---
    uint64_t key = b.hash();
    Entry& e = TT[key & (TT_SIZE - 1)];
    Move ttMove;
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
        movegen::legalmoves(moves, b);                    // evasions: every legal move
        if (moves.empty()) return -(MATE - ply);
        best = -INF;
    } else {
        stand = evalSide(b);
        if (stand >= beta) return stand;
        if (stand + BIG_DELTA < alpha) return stand + BIG_DELTA;   // global delta pruning
        if (stand > alpha) alpha = stand;
        best = stand;
        movegen::legalmoves<movegen::MoveGenType::CAPTURE>(moves, b);
    }
    orderMoves(b, moves, ttMove);

    Move bestMove;
    for (const Move& m : moves) {
        if (!inCheck) {
            const bool promo = m.typeOf() == Move::PROMOTION;
            int victim = (m.typeOf() == Move::ENPASSANT) ? 0 : (int)b.at(m.to()).type();
            if (victim > 5) victim = 0;
            int attacker = (int)b.at(m.from()).type();

            // per-move delta pruning
            int gain = PIECE_VALUE[victim];
            if (promo) gain += PIECE_VALUE[(int)m.promotionType()] - PIECE_VALUE[0];
            if (stand + gain + DELTA_MARGIN < alpha) continue;

            // SEE pruning: skip captures that lose material. Only worth computing
            // when we capture something worth less than the capturing piece.
            if (!promo && ORDER_VALUE[victim] < ORDER_VALUE[attacker] && see(b, m) < 0) continue;
        }

        b.makeMove(m);
        int s = -quiesce(b, -beta, -alpha, ply + 1);
        b.unmakeMove(m);

        if (s > best) { best = s; bestMove = m; }
        if (s > alpha) alpha = s;
        if (alpha >= beta) break;
    }

    // --- TT store (never overwrite a deeper entry of the same position) ---
    if (!(e.key == key && e.depth > 0)) {
        e.key = key;
        e.move = bestMove;
        e.score = toTT(best, ply);
        e.depth = 0;
        e.flag = best <= alphaOrig ? UPPER : (best >= beta ? LOWER : EXACT);
    }
    return best;
}

// ---------- main search: negamax + PVS + alpha-beta + transposition table ----------
static int negamax(Board& b, int depth, int alpha, int beta, int ply) {
    checkTime();
    if (b.isInsufficientMaterial() || b.isHalfMoveDraw() || b.isRepetition()) return 0;
    if (ply >= MAX_PLY) return evalSide(b);

    bool inCheck = b.inCheck();
    if (inCheck) ++depth;                    // check extension: never stop in check
    if (depth <= 0) return quiesce(b, alpha, beta, ply);

    uint64_t key = b.hash();
    Entry& e = TT[key & (TT_SIZE - 1)];
    const int alphaOrig = alpha, betaOrig = beta;
    Move ttMove;
    if (e.key == key) {
        ttMove = e.move;
        if (e.depth >= depth) {
            int s = fromTT(e.score, ply);
            if (e.flag == EXACT) return s;
            if (e.flag == LOWER) alpha = std::max(alpha, s);
            else beta = std::min(beta, s);
            if (alpha >= beta) return s;
        }
    }

    Movelist moves;
    movegen::legalmoves(moves, b);
    if (moves.empty()) return inCheck ? -(MATE - ply) : 0;   // faster mates score higher
    orderMoves(b, moves, ttMove);

    int best = -INF;
    Move bestMove;
    int moveCount = 0;
    for (const Move& m : moves) {
        b.makeMove(m);
        int s;
        if (moveCount == 0) {
            // first move (expected best): full window
            s = -negamax(b, depth - 1, -beta, -alpha, ply + 1);
        } else {
            // principal variation search: probe with a null window...
            s = -negamax(b, depth - 1, -alpha - 1, -alpha, ply + 1);
            // ...and only re-search with the full window if it unexpectedly beat alpha
            if (s > alpha && s < beta)
                s = -negamax(b, depth - 1, -beta, -alpha, ply + 1);
        }
        b.unmakeMove(m);
        ++moveCount;

        if (s > best) { best = s; bestMove = m; }
        if (s > alpha) alpha = s;
        if (alpha >= beta) break;
    }

    e.key = key;
    e.move = bestMove;
    e.score = toTT(best, ply);
    e.depth = depth;
    e.flag = best <= alphaOrig ? UPPER : (best >= betaOrig ? LOWER : EXACT);
    return best;
}

static std::string scoreStr(int s) {
    if (s >= MATE_BOUND)  return "mate " + std::to_string((MATE - s + 1) / 2);
    if (s <= -MATE_BOUND) return "mate -" + std::to_string((MATE + s) / 2);
    return "cp " + std::to_string(s);
}

// ---------- root search (PVS) ----------
// Searches all root moves inside (alpha, beta). `best` is updated whenever a move beats alpha,
// so if time runs out mid-iteration it still holds the best move proven so far.
// Returns a fail-soft score: <= alpha means fail low, >= beta means fail high.
static int rootSearch(Board& b, Movelist& moves, int depth, int alpha, int beta, Move& best) {
    orderMoves(b, moves, best);                 // previous best move goes first
    int bestScore = -INF;
    bool first = true;
    for (const Move& m : moves) {
        b.makeMove(m);
        int s;
        if (first) {
            s = -negamax(b, depth - 1, -beta, -alpha, 1);
        } else {
            s = -negamax(b, depth - 1, -alpha - 1, -alpha, 1);
            if (s > alpha && s < beta)
                s = -negamax(b, depth - 1, -beta, -alpha, 1);
        }
        b.unmakeMove(m);
        first = false;

        if (s > bestScore) bestScore = s;
        if (s > alpha) {
            alpha = s;
            best = m;
            if (alpha >= beta) break;           // fail high
        }
    }
    return bestScore;
}

// ---------- iterative deepening with aspiration windows and a time limit ----------
// Note: on timeout the board passed in is left mid-search, so callers must pass a copy
// (uci.cpp and play.cpp already do).
Move search(Board& b, int maxDepth, int ms) {
    auto start = Clock::now();
    deadline = start + std::chrono::milliseconds(ms);
    nodes = 0;
    Movelist moves;
    movegen::legalmoves(moves, b);
    if (moves.empty()) return Move();
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
                if (score <= alpha) {                       // fail low: widen downwards
                    beta  = (alpha + beta) / 2;
                    alpha = std::max(score - delta, -INF);
                } else if (score >= beta) {                 // fail high: widen upwards
                    beta = std::min(score + delta, INF);
                } else {
                    break;                                  // score inside the window: done
                }
                delta += delta / 2;
                if (delta > ASP_MAX_DELTA) { alpha = -INF; beta = INF; }
            }

            prevScore = score;
            long long t = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            std::cout << "info depth " << d << " score " << scoreStr(score)
                      << " nodes " << nodes << " time " << t
                      << " pv " << uci::moveToUci(best) << std::endl;
            if (score >= MATE_BOUND) break;                 // forced mate found
        } catch (const Timeout&) {
            break;                                          // keep best (last finished/proven move)
        }
    }
    return best;
}