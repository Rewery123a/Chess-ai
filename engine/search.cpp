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
#include <new>
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
static const int S_COUNTER = 13000;   // countermove: quiet move that refuted the opponent's last move
static const int S_BAD     = -28000;  // + MVV-LVA: captures that lose material (SEE < 0) go after all quiets
static const int QUIET_MAX = 12000, QUIET_MIN = -24000;   // clamp for combined quiet history scores
static const int MAX_HIST  = 8192;    // quiet history lives in [-MAX_HIST, MAX_HIST]

// null move / LMR settings
static const int NMP_MIN_DEPTH = 3;
static const int LMR_MIN_DEPTH = 3;
static const int LMR_MIN_MOVES = 3;

// pruning settings (centipawns)
static const int RFP_MAX_DEPTH = 6, RFP_MARGIN = 80;                    // reverse futility
static const int FP_MAX_DEPTH  = 4, FP_BASE = 120, FP_MARGIN = 100;     // futility
static const int LMP_MAX_DEPTH = 4;                                     // late move pruning
static const int IIR_MIN_DEPTH = 4;                                     // internal iterative reduction
static const int SEE_PRUNE_MAX_DEPTH = 6, SEE_PRUNE_MARGIN = 90;       // skip captures with SEE < -margin * depth

enum Flag : uint8_t { EXACT, LOWER, UPPER };

// ---------- transposition table: 64-byte buckets of 4 entries, age-aware replacement ----------
struct Entry {                       // 16 bytes
    uint64_t key     = 0;
    int32_t  score   = 0;
    uint16_t move    = 0;            // raw chess::Move encoding
    int8_t   depth   = 0;
    uint8_t  ageFlag = 0;            // (age << 2) | flag
    int flag() const { return ageFlag & 3; }
    int age()  const { return ageFlag >> 2; }
};
static const int BUCKET_SIZE = 4;
struct alignas(64) Bucket { Entry e[BUCKET_SIZE]; };
static_assert(sizeof(Entry) == 16 && sizeof(Bucket) == 64, "TT layout");

static std::atomic<bool> stopFlag{false};
void requestStop() { stopFlag = true; }
void resetStop() { stopFlag = false; }

static const size_t DEFAULT_HASH_MB = 64, MAX_HASH_MB = 65536;
static std::vector<Bucket> TT(DEFAULT_HASH_MB * 1024 * 1024 / sizeof(Bucket));
static uint64_t ttBuckets = TT.size();
static size_t   ttSizeMB  = DEFAULT_HASH_MB;
static int      ttAge     = 0;       // bumped once per search(); 6 bits, wraps at 64

static inline Bucket& bucketFor(uint64_t key) {
    // multiply-shift maps the key onto any bucket count (the table need not be a power of two)
    return TT[(uint64_t)(((unsigned __int128)key * ttBuckets) >> 64)];
}

// Returns the matching entry (and refreshes its age so it survives replacement) or nullptr.
static inline Entry* ttProbe(uint64_t key) {
    Bucket& bk = bucketFor(key);
    for (int i = 0; i < BUCKET_SIZE; ++i)
        if (bk.e[i].key == key) {
            bk.e[i].ageFlag = (uint8_t)((ttAge << 2) | bk.e[i].flag());
            return &bk.e[i];
        }
    return nullptr;
}

// Replacement: same position -> update in place (unless the old entry is clearly more valuable);
// otherwise evict the entry with the lowest depth - 4 * age-difference (empty slots go first).
static void ttStore(uint64_t key, uint16_t mv, int score, int depth, int flag) {
    Bucket& bk = bucketFor(key);
    Entry* victim = nullptr;
    int victimValue = 1 << 30;
    for (int i = 0; i < BUCKET_SIZE; ++i) {
        Entry& c = bk.e[i];
        if (c.key == key) {
            const bool sameAge = c.age() == ttAge;
            if (sameAge && ((depth <= 0 && c.depth > 0) || (flag != EXACT && depth + 3 <= c.depth)))
                return;                                   // keep the deeper entry from this search
            if (mv == 0) mv = c.move;                     // never lose a known move for a bound without one
            victim = &c;
            break;
        }
        const int value = c.key == 0 ? -100000 : c.depth - 4 * ((ttAge - c.age() + 64) & 63);
        if (value < victimValue) { victimValue = value; victim = &c; }
    }
    // quiescence entries (depth 0) must not push out useful deeper entries
    if (depth <= 0 && victim->key != key && victimValue > 0) return;

    victim->key     = key;
    victim->score   = score;
    victim->move    = mv;
    victim->depth   = (int8_t)std::max(-127, std::min(depth, 127));
    victim->ageFlag = (uint8_t)((ttAge << 2) | flag);
}

void setHashSize(size_t mb) {
    mb = std::max<size_t>(1, std::min(mb, MAX_HASH_MB));
    std::vector<Bucket>().swap(TT);                       // release the old table first
    try {
        TT.resize(mb * 1024 * 1024 / sizeof(Bucket));
        ttSizeMB = mb;
    } catch (const std::bad_alloc&) {
        std::cerr << "Warning: could not allocate " << mb << " MB hash, using 1 MB" << std::endl;
        TT.resize(1024 * 1024 / sizeof(Bucket));
        ttSizeMB = 1;
    }
    ttBuckets = TT.size();
    ttAge = 0;
}
size_t hashSizeMB() { return ttSizeMB; }

// permille of sampled slots holding an entry from the current search (for "info hashfull")
static int hashfull() {
    const uint64_t n = std::min<uint64_t>(ttBuckets, 250);
    int used = 0;
    for (uint64_t i = 0; i < n; ++i)
        for (int j = 0; j < BUCKET_SIZE; ++j)
            if (TT[i].e[j].key != 0 && TT[i].e[j].age() == ttAge) ++used;
    return (int)(used * 1000 / (n * BUCKET_SIZE));
}

// ---------- ordering heuristics state ----------
static Move killers[MAX_PLY + 1][2];
static int  history[2][64][64];          // [side][from][to]
// continuation history: how good a quiet move is, given the move made 1 (countermove history) or
// 2 (follow-up history) plies earlier. [distance-1][prev piece type*64 + prev to][side*6 + piece type)*64 + to]
static int  contHist[2][6 * 64][2 * 6 * 64];
static Move counterMove[6][64];          // [prev piece type][prev to] -> quiet move that refuted it
// the move that led INTO each ply (stkTo < 0: none, e.g. root or after a null move)
static int  stkPt[MAX_PLY + 2], stkTo[MAX_PLY + 2];
static int  LMR[64][64];                 // [depth][moveCount]
static const int NO_EVAL = -INF;         // "no static eval at this ply" (side was in check)
static int  evalStack[MAX_PLY + 2];      // static eval per ply, for the "improving" flag
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
    std::fill(TT.begin(), TT.end(), Bucket());
    ttAge = 0;
    std::memset(history, 0, sizeof(history));
    std::memset(contHist, 0, sizeof(contHist));
    for (auto& row : counterMove) for (Move& m : row) m = Move();
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

uint64_t searchNodes() { return nodes; }

static inline int sideIdx(const Board& b) { return b.sideToMove() == Color::WHITE ? 0 : 1; }

static inline void gravity(int& h, int bonus) {
    h += bonus - h * std::abs(bonus) / MAX_HIST;      // "gravity": h stays within +-MAX_HIST
}

static inline int curIdx(int side, int pt, int to) { return (side * 6 + pt) * 64 + to; }

// quiet move score: main history + countermove history (1 ply back) + follow-up history (2 plies back)
static inline int histScore(int side, int ply, int pt, const Move& m) {
    const int to = m.to().index(), cur = curIdx(side, pt, to);
    int s = history[side][m.from().index()][to];
    if (stkTo[ply] >= 0)                s += contHist[0][stkPt[ply] * 64 + stkTo[ply]][cur];
    if (ply >= 1 && stkTo[ply - 1] >= 0) s += contHist[1][stkPt[ply - 1] * 64 + stkTo[ply - 1]][cur];
    return s;
}

// board must be in the position the move was played from
static void updateQuiet(const Board& b, int side, int ply, const Move& m, int bonus) {
    const int pt = (int)b.at(m.from()).type(), to = m.to().index(), cur = curIdx(side, pt, to);
    gravity(history[side][m.from().index()][to], bonus);
    if (stkTo[ply] >= 0)                gravity(contHist[0][stkPt[ply] * 64 + stkTo[ply]][cur], bonus);
    if (ply >= 1 && stkTo[ply - 1] >= 0) gravity(contHist[1][stkPt[ply - 1] * 64 + stkTo[ply - 1]][cur], bonus);
}

static int see(const Board& b, const Move& m);       // defined below

// ---------- move ordering ----------
// tt move > good captures (MVV-LVA, SEE >= 0) > killers > countermove > quiets by combined history
// > losing captures (SEE < 0).
// Only assigns scores; callers use pickMove() so we never sort the whole list.
// useCont is false in quiescence, where the continuation context is not tracked.
static void orderMoves(const Board& b, Movelist& moves, Move ttMove, int ply, bool useCont = true) {
    const int side = sideIdx(b);
    const Move counter = (useCont && stkTo[ply] >= 0) ? counterMove[stkPt[ply]][stkTo[ply]] : Move();
    for (Move& m : moves) {
        int s;
        if (m == ttMove) {
            s = S_TT;
        } else if (b.isCapture(m)) {
            int v = (int)b.at(m.to()).type();
            if (v > 5) v = 0;                           // en passant
            int a = (int)b.at(m.from()).type();
            const int mvvLva = 100 + 10 * ORDER_VALUE[v] - ORDER_VALUE[a];
            const bool promo = m.typeOf() == Move::PROMOTION;
            // SEE only when the attacker is worth more than the victim (otherwise it cannot lose material)
            if (!promo && ORDER_VALUE[v] < ORDER_VALUE[a] && see(b, m) < 0) {
                s = S_BAD + mvvLva;
            } else {
                s = S_CAPTURE + mvvLva;
                if (promo) s += 80;
            }
        } else if (m.typeOf() == Move::PROMOTION) {
            s = S_CAPTURE + 80;
        } else if (m == killers[ply][0]) {
            s = S_KILLER1;
        } else if (m == killers[ply][1]) {
            s = S_KILLER2;
        } else if (counter != Move() && m == counter) {
            s = S_COUNTER;
        } else {
            if (useCont) s = histScore(side, ply, (int)b.at(m.from()).type(), m);
            else         s = history[side][m.from().index()][m.to().index()];
            s = std::max(QUIET_MIN, std::min(s, QUIET_MAX));
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
    Move ttMove{};
    if (const Entry* e = ttProbe(key)) {
        ttMove = Move(e->move);
        int s = fromTT(e->score, ply);
        if (e->flag() == EXACT) return s;
        if (e->flag() == LOWER && s >= beta) return s;
        if (e->flag() == UPPER && s <= alpha) return s;
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
    orderMoves(b, moves, ttMove, ply, false);

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

            // losing captures were already marked by orderMoves (negative score); the tt move was not
            if (!promo && ORDER_VALUE[victim] < ORDER_VALUE[attacker] &&
                (m == ttMove ? see(b, m) < 0 : m.score() < 0)) continue;
        }

        b.makeMove(m);
        int s = -quiesce(b, -beta, -alpha, ply + 1);
        b.unmakeMove(m);

        if (s > best) { best = s; bestMove = m; }
        if (s > alpha) alpha = s;
        if (alpha >= beta) break;
    }

    // qsearch entries are depth 0; ttStore keeps them from evicting real (depth > 0) entries
    {
        const int flag = best <= alphaOrig ? UPPER : (best >= beta ? LOWER : EXACT);
        ttStore(key, flag == UPPER ? 0 : bestMove.move(), toTT(best, ply), 0, flag);
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
    const int alphaOrig = alpha;
    Move ttMove{};
    if (const Entry* e = ttProbe(key)) {
        ttMove = Move(e->move);
        // only cut in non-PV nodes: keeps the PV exact and avoids TT-induced oddities
        if (!pvNode && e->depth >= depth) {
            int s = fromTT(e->score, ply);
            if (e->flag() == EXACT) return s;
            if (e->flag() == LOWER && s >= beta) return s;
            if (e->flag() == UPPER && s <= alpha) return s;
        }
    }

    // internal iterative reduction: no hash move here, so search shallower to find one cheaply
    if (depth >= IIR_MIN_DEPTH && ttMove == Move()) --depth;

    // static eval for every node that is not in check (PV nodes too: they feed "improving" below)
    const bool prunable = !pvNode && !inCheck && std::abs(beta) < MATE_BOUND;
    int staticEval = NO_EVAL;
    if (!inCheck) staticEval = evalSide(b);
    evalStack[ply] = staticEval;

    // improving: our static eval is better than two plies ago (same side to move); fall back to four
    // plies ago if we were in check then. When improving we trust the position more and prune/reduce
    // less aggressively where it is risky, and more where it is safe (see each use below).
    bool improving = false;
    if (!inCheck) {
        int prev = ply >= 2 ? evalStack[ply - 2] : NO_EVAL;
        if (prev == NO_EVAL && ply >= 4) prev = evalStack[ply - 4];
        improving = prev != NO_EVAL && staticEval > prev;
    }

    // reverse futility pruning: so far above beta that a shallow search will not change that;
    // an improving side needs a smaller margin
    if (prunable && depth <= RFP_MAX_DEPTH && staticEval - RFP_MARGIN * (depth - improving) >= beta && hasNonPawnMaterial(b))
        return staticEval - RFP_MARGIN * (depth - improving);

    // --- null move pruning: if passing still beats beta, this node is almost surely a cut ---
    if (!pvNode && canNull && !inCheck && depth >= NMP_MIN_DEPTH &&
        std::abs(beta) < MATE_BOUND && hasNonPawnMaterial(b) && staticEval >= beta) {
        int R = 2 + depth / 4;
        stkTo[ply + 1] = -1; stkPt[ply + 1] = 0;
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
        const int movedPt = (int)b.at(m.from()).type();
        const int h = quiet ? histScore(side, ply, movedPt, m) : 0;

        // SEE pruning: at low depth skip captures that clearly lose material. orderMoves already
        // flagged losing captures with a negative score, so SEE is only computed for those (and the
        // hash move, which has a huge score, is never pruned). Never the first move, never in PV or
        // check nodes, and (below) never a move that gives check.
        const bool seeBad = prunable && i > 0 && depth <= SEE_PRUNE_MAX_DEPTH && best > -MATE_BOUND &&
                            !quiet && m.score() < 0 && see(b, m) < -SEE_PRUNE_MARGIN * depth;

        b.makeMove(m);
        stkPt[ply + 1] = movedPt; stkTo[ply + 1] = m.to().index();
        if (seeBad && !b.inCheck()) {
            b.unmakeMove(m);
            continue;
        }

        // futility / late move pruning: skip hopeless quiet moves at shallow depth (never the
        // first move, killers, or moves that give check)
        if (prunable && i > 0 && quiet && !isKiller && best > -MATE_BOUND) {
            const bool lmp = depth <= LMP_MAX_DEPTH && i >= (3 + depth * depth) / (improving ? 1 : 2);
            const bool fut = depth <= FP_MAX_DEPTH && staticEval + FP_BASE + FP_MARGIN * depth <= alpha;
            if ((lmp || fut) && !b.inCheck()) {
                b.unmakeMove(m);
                continue;
            }
        }

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
                if (!improving) ++r;                        // eval going nowhere: reduce more
                r -= h / 12288;                             // good combined history: reduce less, bad: more
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
                updateQuiet(b, side, ply, m, bonus);
                for (int q = 0; q < nq; ++q) updateQuiet(b, side, ply, quiets[q], -bonus);   // penalise quiets that failed
                if (stkTo[ply] >= 0) counterMove[stkPt[ply]][stkTo[ply]] = m;
            }
            break;
        }
        if (quiet && nq < 64) quiets[nq++] = m;
    }

    // fail-low nodes have no real best move, so store none (ttStore keeps any older one)
    {
        const int flag = best <= alphaOrig ? UPPER : (best >= beta ? LOWER : EXACT);
        ttStore(key, flag == UPPER ? 0 : bestMove.move(), toTT(best, ply), depth, flag);
    }
    return best;
}

static std::string scoreStr(int s) {
    if (s >= MATE_BOUND)  return "mate " + std::to_string((MATE - s + 1) / 2);
    if (s <= -MATE_BOUND) return "mate -" + std::to_string((MATE + s) / 2);
    return "cp " + std::to_string(s);
}

// ---------- root search (PVS) ----------
static uint64_t rootBestNodes = 0;     // nodes spent on the current best root move (time management)

static int rootSearch(Board& b, Movelist& moves, int depth, int alpha, int beta, Move& best) {
    orderMoves(b, moves, best, 0);
    std::stable_sort(moves.begin(), moves.end(),
                     [](const Move& x, const Move& y) { return x.score() > y.score(); });
    int bestScore = -INF;
    bool first = true;
    for (const Move& m : moves) {
        const uint64_t n0 = nodes;
        stkPt[1] = (int)b.at(m.from()).type(); stkTo[1] = m.to().index();
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
            rootBestNodes = nodes - n0;
            if (alpha >= beta) break;
        }
    }
    return bestScore;
}

// ---------- iterative deepening with aspiration windows and time management ----------
// Hard limit: the search is aborted mid-iteration (checkTime). Soft limit: checked after every
// finished iteration and stretched or shrunk by how settled the search looks.
static const double STAB_SCALE[9] = {1.40, 1.20, 1.05, 0.95, 0.88, 0.82, 0.78, 0.75, 0.72};

// Note: on timeout the board passed in is left mid-search, so callers must pass a copy.
Move search(Board& b, const SearchLimits& lim) {
    initLMR();
    auto start = Clock::now();
    deadline = start + std::chrono::milliseconds(lim.hardMs);
    nodes = 0;
    ttAge = (ttAge + 1) & 63;                       // new search = new TT generation
    for (auto& k : killers) k[0] = k[1] = Move();
    for (auto& s : history) for (auto& f : s) for (int& h : f) h /= 2;   // age, don't wipe

    Movelist moves;
    movegen::legalmoves(moves, b);
    if (moves.empty()) return Move();
    if (moves.size() == 1 && !lim.infinite) return moves[0];   // forced move: don't burn the clock
    Move best = moves[0], prevBest = best;
    int prevScore = 0, stability = 0;
    stkTo[0] = -1; stkPt[0] = 0;                            // no move led into the root
    evalStack[0] = b.inCheck() ? NO_EVAL : evalSide(b);     // root eval, read by plies 2 and 4

    for (int d = 1; d <= lim.maxDepth; ++d) {
        try {
            const int lastScore = prevScore;
            int alpha = -INF, beta = INF, delta = ASP_DELTA;
            if (d >= ASP_MIN_DEPTH && std::abs(lastScore) < MATE_BOUND) {
                alpha = std::max(lastScore - delta, -INF);
                beta  = std::min(lastScore + delta,  INF);
            }

            int score;
            uint64_t iterNodes = 0;
            while (true) {
                const uint64_t n0 = nodes;
                score = rootSearch(b, moves, d, alpha, beta, best);
                iterNodes = nodes - n0;
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
            const long long t = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            if (printInfo) {
                std::cout << "info depth " << d << " score " << scoreStr(score)
                          << " nodes " << nodes << " nps " << (t > 0 ? nodes * 1000 / t : nodes)
                          << " hashfull " << hashfull() << " time " << t
                          << " pv " << uci::moveToUci(best) << std::endl;
            }
            if (score >= MATE_BOUND) break;

            // ---- time management ----
            stability = (d >= 2 && best == prevBest) ? std::min(stability + 1, 8) : 0;
            prevBest = best;
            if (!lim.infinite) {
                double target = (double)lim.softMs;
                if (lim.dynamic && d >= 6) {
                    double scale = STAB_SCALE[stability];                       // same move for long -> less time
                    const int drop = lastScore - score;
                    if (drop > 0)        scale *= 1.0 + std::min(drop, 80) * 0.006;   // score falling -> think more
                    else if (drop < -20) scale *= 0.95;                         // clearly improving -> relax
                    if (d >= 8 && iterNodes > 0) {                              // one dominant root move -> less time
                        double frac = (double)rootBestNodes / (double)iterNodes;
                        scale *= std::max(0.7, std::min(1.5, 1.7 - frac));
                    }
                    scale = std::max(0.4, std::min(scale, 2.5));
                    target = std::min(target * scale, (double)lim.hardMs);
                }
                if ((double)t >= target) break;
            }
        } catch (const Timeout&) {
            break;
        }
    }
    return best;
}

// Convenience overload: fixed depth and a single time budget (terminal play, bench).
Move search(Board& b, int maxDepth, int ms) {
    SearchLimits lim;
    lim.maxDepth = maxDepth;
    lim.hardMs = ms;
    lim.softMs = (int64_t)(ms * 0.6);
    lim.dynamic = true;
    return search(b, lim);
}