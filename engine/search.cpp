#include "search.hpp"
#include "eval.hpp"
#include "tables.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace chess;
using Clock = std::chrono::steady_clock;

static const int MATE = 100000, MATE_BOUND = MATE - 512, INF = 1000000;
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

// ---------- quiescence search: only captures, avoids stopping mid-trade ----------
static int quiesce(Board& b, int alpha, int beta) {
    checkTime();
    int stand = evalSide(b);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;
    Movelist moves;
    movegen::legalmoves<movegen::MoveGenType::CAPTURE>(moves, b);
    orderMoves(b, moves, Move());
    for (const Move& m : moves) {
        b.makeMove(m);
        int s = -quiesce(b, -beta, -alpha);
        b.unmakeMove(m);
        if (s >= beta) return beta;
        if (s > alpha) alpha = s;
    }
    return alpha;
}

// ---------- main search: negamax + alpha-beta + transposition table ----------
static int negamax(Board& b, int depth, int alpha, int beta, int ply) {
    checkTime();
    if (b.isInsufficientMaterial() || b.isHalfMoveDraw() || b.isRepetition()) return 0;
    if (ply >= 100) return evalSide(b);

    bool inCheck = b.inCheck();
    if (inCheck) ++depth;                    // check extension: never stop in check
    if (depth <= 0) return quiesce(b, alpha, beta);

    uint64_t key = b.hash();
    Entry& e = TT[key & (TT_SIZE - 1)];
    int alphaOrig = alpha;
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
    for (const Move& m : moves) {
        b.makeMove(m);
        int s = -negamax(b, depth - 1, -beta, -alpha, ply + 1);
        b.unmakeMove(m);
        if (s > best) { best = s; bestMove = m; }
        if (s > alpha) alpha = s;
        if (alpha >= beta) break;
    }

    e.key = key;
    e.move = bestMove;
    e.score = toTT(best, ply);
    e.depth = depth;
    e.flag = best <= alphaOrig ? UPPER : (best >= beta ? LOWER : EXACT);
    return best;
}

static std::string scoreStr(int s) {
    if (s >= MATE_BOUND)  return "mate " + std::to_string((MATE - s + 1) / 2);
    if (s <= -MATE_BOUND) return "mate -" + std::to_string((MATE + s) / 2);
    return "cp " + std::to_string(s);
}

// ---------- iterative deepening with a time limit ----------
Move search(Board& b, int maxDepth, int ms) {
    deadline = Clock::now() + std::chrono::milliseconds(ms);
    nodes = 0;
    Movelist moves;
    movegen::legalmoves(moves, b);
    if (moves.empty()) return Move();
    Move best = moves[0];

    for (int d = 1; d <= maxDepth; ++d) {
        try {
            orderMoves(b, moves, best);
            Move cur;
            int curScore = -INF, alpha = -INF;
            for (const Move& m : moves) {
                b.makeMove(m);
                int s = -negamax(b, d - 1, -INF, -alpha, 1);
                b.unmakeMove(m);
                if (s > curScore) { curScore = s; cur = m; }
                if (s > alpha) alpha = s;
            }
            best = cur;
            std::cout << "info depth " << d << " score " << scoreStr(curScore)
                      << " nodes " << nodes << " pv " << uci::moveToUci(best) << std::endl;
            if (curScore >= MATE_BOUND) break;     // forced mate found
        } catch (const Timeout&) {
            break;                                  // use the last finished depth
        }
    }
    return best;
}