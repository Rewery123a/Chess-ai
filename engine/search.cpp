#include "search.hpp"
#include "eval.hpp"
#include "tables.hpp"
#include "tb.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace chess;
using Clock = std::chrono::steady_clock;

static const int MATE = 100000, MATE_BOUND = MATE - 512, INF = 1000000;
static const int MAX_PLY = 128;
static const int TB_WIN  = 30000;   // tablebase win: far above any eval, far below MATE_BOUND
static const int MAX_THREADS = 256;

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
static const int S_BAD_THRESH = -26000;   // below every clamped quiet score, above every losing capture
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
static const int SE_MIN_DEPTH = 7, SE_MARGIN = 2;                       // singular extension: margin = SE_MARGIN * depth
static const int RAZOR_MAX_DEPTH = 3, RAZOR_BASE = 200, RAZOR_MARGIN = 150;   // razoring: eval + base + margin * depth <= alpha
static const int PROBCUT_MIN_DEPTH = 5, PROBCUT_MARGIN = 160, PROBCUT_REDUCTION = 4;
static const int SEE_PRUNE_MAX_DEPTH = 6, SEE_PRUNE_MARGIN = 90;       // skip captures with SEE < -margin * depth

enum Flag : uint8_t { EXACT, LOWER, UPPER };

// ---------- transposition table: 64-byte buckets of 4 slots, age-aware replacement ----------
// Shared by all search threads without locks. Each 16-byte slot holds the packed entry data and
// (key XOR data): a read that was torn by a concurrent write fails the key check and is treated as
// a miss, so a garbled entry is never used.
struct TTEntry {                     // unpacked view of a slot
    uint64_t key     = 0;
    int32_t  score   = 0;
    uint16_t move    = 0;            // raw chess::Move encoding
    int8_t   depth   = 0;
    uint8_t  ageFlag = 0;            // (age << 2) | flag
    int flag() const { return ageFlag & 3; }
    int age()  const { return ageFlag >> 2; }
};
struct Slot { uint64_t keyx = 0, data = 0; };
static const int BUCKET_SIZE = 4;
struct alignas(64) Bucket { Slot e[BUCKET_SIZE]; };
static_assert(sizeof(Slot) == 16 && sizeof(Bucket) == 64, "TT layout");

static inline uint64_t packData(int32_t score, uint16_t move, int8_t depth, uint8_t ageFlag) {
    return (uint64_t)(uint32_t)score | ((uint64_t)move << 32) | ((uint64_t)(uint8_t)depth << 48) |
           ((uint64_t)ageFlag << 56);
}
static inline TTEntry loadSlot(const Slot& s) {
    const uint64_t d = __atomic_load_n(&s.data, __ATOMIC_RELAXED);
    const uint64_t k = __atomic_load_n(&s.keyx, __ATOMIC_RELAXED);
    TTEntry t;
    t.key     = k ^ d;
    t.score   = (int32_t)(uint32_t)d;
    t.move    = (uint16_t)(d >> 32);
    t.depth   = (int8_t)(uint8_t)(d >> 48);
    t.ageFlag = (uint8_t)(d >> 56);
    return t;
}
static inline void storeSlot(Slot& s, uint64_t key, uint64_t data) {
    __atomic_store_n(&s.data, data, __ATOMIC_RELAXED);
    __atomic_store_n(&s.keyx, key ^ data, __ATOMIC_RELAXED);
}

// ---------- global search control ----------
static std::atomic<bool> stopFlag{false};      // external stop request (UCI "stop", new command)
static std::atomic<bool> abortFlag{false};     // internal: every thread must unwind (time up / search over)
static std::atomic<bool> g_pondering{false};   // "go ponder" is active and has not been confirmed by "ponderhit"
static std::atomic<int64_t> g_deadlineNs{INT64_MAX};   // hard limit (steady clock, ns); INT64_MAX while pondering
static int64_t   g_hardNs = 0, g_softNs = 0, g_searchStartNs = 0;   // limits of the running search (under g_timeMx)
static std::mutex g_timeMx;                    // keeps ponderhit and the start of a search consistent
static uint64_t  g_totalNodes = 0;
static std::vector<Move> g_lastPv;             // PV belonging to the move search() returned (for the ponder move)
static uint64_t  g_lastPvKey = 0;              // hash of the root position that PV was searched from

static inline int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

void requestStop() { stopFlag = true; }
void resetStop() { stopFlag = false; }
bool stopRequested() { return stopFlag.load(); }

void setPondering(bool on) { g_pondering = on; }
bool isPondering() { return g_pondering.load(); }
// The opponent played the expected move. The hard limit now counts from this moment (pondering was
// free), but the time already spent pondering counts toward the soft limit: the search stops at the
// end of the next iteration once it has thought "long enough" in total, which banks clock time.
// If that time is already used up, the iteration in progress only gets a short grace period.
void ponderHit() {
    std::lock_guard<std::mutex> lk(g_timeMx);
    if (!g_pondering.load()) return;
    const int64_t n = nowNs();
    int64_t limit = g_hardNs;
    if (n - g_searchStartNs >= g_softNs)
        limit = std::min(limit, std::max<int64_t>(g_softNs / 4, 20000000LL));
    g_deadlineNs = n + limit;
    g_pondering = false;
}

// ---------- the table itself ----------
static const size_t DEFAULT_HASH_MB = 64, MAX_HASH_MB = 65536;
static std::vector<Bucket> TT(DEFAULT_HASH_MB * 1024 * 1024 / sizeof(Bucket));
static uint64_t ttBuckets = TT.size();
static size_t   ttSizeMB  = DEFAULT_HASH_MB;
static int      ttAge     = 0;       // bumped once per search(); 6 bits, wraps at 64. Only changed while no thread runs.

static inline Bucket& bucketFor(uint64_t key) {
    // multiply-shift maps the key onto any bucket count (the table need not be a power of two)
    return TT[(uint64_t)(((unsigned __int128)key * ttBuckets) >> 64)];
}

// Start loading a position's bucket before it is needed: the table is far bigger than the caches and,
// with several threads hammering it, a miss is the most expensive thing a node does.
static inline void ttPrefetch(uint64_t key) { __builtin_prefetch(&bucketFor(key)); }

// Fills `out` and returns true on a hit (and refreshes the entry's age so it survives replacement).
static inline bool ttProbe(uint64_t key, TTEntry& out) {
    Bucket& bk = bucketFor(key);
    for (int i = 0; i < BUCKET_SIZE; ++i) {
        TTEntry c = loadSlot(bk.e[i]);
        if (c.key == key) {
            if (c.age() != ttAge) {
                c.ageFlag = (uint8_t)((ttAge << 2) | c.flag());
                storeSlot(bk.e[i], key, packData(c.score, c.move, c.depth, c.ageFlag));
            }
            out = c;
            return true;
        }
    }
    return false;
}

// Replacement: same position -> update in place (unless the old entry is clearly more valuable);
// otherwise evict the entry with the lowest depth - 4 * age-difference (empty slots go first).
static void ttStore(uint64_t key, uint16_t mv, int score, int depth, int flag) {
    Bucket& bk = bucketFor(key);
    Slot* victim = nullptr;
    TTEntry vc;
    int victimValue = 1 << 30;
    for (int i = 0; i < BUCKET_SIZE; ++i) {
        Slot& s = bk.e[i];
        const TTEntry c = loadSlot(s);
        if (c.key == key) {
            const bool sameAge = c.age() == ttAge;
            if (sameAge && ((depth <= 0 && c.depth > 0) || (flag != EXACT && depth + 3 <= c.depth)))
                return;                                   // keep the deeper entry from this search
            if (mv == 0) mv = c.move;                     // never lose a known move for a bound without one
            victim = &s; vc = c;
            break;
        }
        const int value = c.key == 0 ? -100000 : c.depth - 4 * ((ttAge - c.age() + 64) & 63);
        if (value < victimValue) { victimValue = value; victim = &s; vc = c; }
    }
    // quiescence entries (depth 0) must not push out useful deeper entries
    if (depth <= 0 && vc.key != key && victimValue > 0) return;

    const int8_t d8 = (int8_t)std::max(-127, std::min(depth, 127));
    storeSlot(*victim, key, packData((int32_t)score, mv, d8, (uint8_t)((ttAge << 2) | flag)));
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
        for (int j = 0; j < BUCKET_SIZE; ++j) {
            const TTEntry c = loadSlot(TT[i].e[j]);
            if (c.key != 0 && c.age() == ttAge) ++used;
        }
    return (int)(used * 1000 / (n * BUCKET_SIZE));
}

// ---------- per-thread search state ----------
// Everything a search thread writes while searching lives here, so threads never share it.
struct ThreadData {
    int      id = 0;                              // 0 = main thread (prints info, manages time)
    uint64_t nodes = 0;
    uint64_t rootBestNodes = 0;                   // nodes spent on the current best root move (time management)
    Move     killers[MAX_PLY + 1][2];
    int      history[2][64][64];                  // [side][from][to]
    // continuation history: how good a quiet move is, given the move made 1 (countermove history) or
    // 2 (follow-up history) plies earlier. [distance-1][prev piece type*64 + prev to][(side*6 + piece type)*64 + to]
    int      contHist[2][6 * 64][2 * 6 * 64];
    // capture history: how good a capture is, by [side][moving piece type][to square][captured piece type]
    int      capHist[2][6][64][6];
    Move     counterMove[6][64];                  // [prev piece type][prev to] -> quiet move that refuted it
    // the move that led INTO each ply (stkTo < 0: none, e.g. root or after a null move)
    int      stkPt[MAX_PLY + 2], stkTo[MAX_PLY + 2];
    int      evalStack[MAX_PLY + 2];              // static eval per ply, for the "improving" flag
    // NNUE: one accumulator per ply, updated lazily. delta[p] describes the move that led INTO ply p and
    // accValid[p] says acc[p] already includes it (acc[0] is built from scratch at the root).
    struct FeatureRef { int8_t colour, pt, sq; };
    struct AccDelta { int nsub = 0, nadd = 0; FeatureRef sub[2], add[2]; };
    nnue::Accumulator acc[MAX_PLY + 2];
    AccDelta delta[MAX_PLY + 2];
    bool     accValid[MAX_PLY + 2];
    bool     useNN = false;                       // a net is loaded: evaluate with it
    // principal variation (triangular table): pv[ply][ply .. pvLen[ply]-1] is the line found from that ply
    Move     pv[MAX_PLY + 2][MAX_PLY + 2];
    int      pvLen[MAX_PLY + 2];
    // result of the last COMPLETED iteration; read by the main thread once all threads have stopped
    int      doneDepth = 0, doneScore = 0;
    Move     doneBest;
    Move     donePv[MAX_PLY + 2];
    int      donePvLen = 0;

    void clear() {
        std::memset(pvLen, 0, sizeof(pvLen));
        doneDepth = doneScore = donePvLen = 0;
        doneBest = Move();
        std::memset(history, 0, sizeof(history));
        std::memset(contHist, 0, sizeof(contHist));
        std::memset(capHist, 0, sizeof(capHist));
        for (auto& row : counterMove) for (Move& m : row) m = Move();
        for (auto& k : killers) k[0] = k[1] = Move();
        std::memset(stkPt, 0, sizeof(stkPt));
        for (int& t : stkTo) t = -1;
        std::memset(evalStack, 0, sizeof(evalStack));
        nodes = 0;
        rootBestNodes = 0;
    }
};

static std::vector<std::unique_ptr<ThreadData>> g_threads;

// ---------- NNUE accumulator handling ----------
// Call accSetMove BEFORE b.makeMove(m) (it reads the position the move is played from); `ply` is the ply of the
// position AFTER the move. Nothing is computed until a node actually needs its static eval (evalAt).
static inline void accSetMove(ThreadData& td, const Board& b, const Move m, int ply) {
    if (!td.useNN) return;
    ThreadData::AccDelta& d = td.delta[ply];
    td.accValid[ply] = false;
    const int us = b.sideToMove() == Color::WHITE ? 0 : 1;
    const int from = m.from().index(), to = m.to().index();
    const int pt = (int)b.at(m.from()).type();
    const int type = m.typeOf();
    d.nsub = d.nadd = 0;
    auto sub = [&](int c, int p, int sq) { d.sub[d.nsub++] = {(int8_t)c, (int8_t)p, (int8_t)sq}; };
    auto add = [&](int c, int p, int sq) { d.add[d.nadd++] = {(int8_t)c, (int8_t)p, (int8_t)sq}; };
    if (type == Move::CASTLING) {                      // encoded as "king takes own rook"
        const bool kingSide = to > from;
        const int rank = from & ~7;
        sub(us, 5, from); sub(us, 3, to);
        add(us, 5, rank + (kingSide ? 6 : 2)); add(us, 3, rank + (kingSide ? 5 : 3));
    } else if (type == Move::ENPASSANT) {
        sub(us, 0, from); sub(us ^ 1, 0, us == 0 ? to - 8 : to + 8); add(us, 0, to);
    } else {
        const int victim = (int)b.at(m.to()).type();   // 6 = empty square
        sub(us, pt, from);
        if (victim <= 5) sub(us ^ 1, victim, to);
        add(us, type == Move::PROMOTION ? (int)m.promotionType() : pt, to);
    }
}

static inline void accSetNull(ThreadData& td, int ply) {   // null move: same pieces, other side to move
    if (!td.useNN) return;
    td.accValid[ply] = false;
    td.delta[ply].nsub = td.delta[ply].nadd = 0;
}

static void accUpdateTo(ThreadData& td, int ply) {
    int k = ply;
    while (k > 0 && !td.accValid[k]) --k;
    for (int j = k + 1; j <= ply; ++j) {
        nnue::copy(td.acc[j], td.acc[j - 1]);
        const ThreadData::AccDelta& d = td.delta[j];
        for (int i = 0; i < d.nsub; ++i)
            for (int p = 0; p < 2; ++p) nnue::subFeature(td.acc[j], p, nnue::featureIndex(p, d.sub[i].colour, d.sub[i].pt, d.sub[i].sq));
        for (int i = 0; i < d.nadd; ++i)
            for (int p = 0; p < 2; ++p) nnue::addFeature(td.acc[j], p, nnue::featureIndex(p, d.add[i].colour, d.add[i].pt, d.add[i].sq));
        td.accValid[j] = true;
    }
}

// static eval from the side to move's point of view (net if loaded, else the handcrafted eval)
static inline int evalAt(ThreadData& td, const Board& b, int ply) {
    if (!td.useNN) return evalSide(b);
    accUpdateTo(td, ply);
#ifdef NNUE_VERIFY
    {
        nnue::Accumulator ref;
        nnueRefresh(b, ref);
        for (int p = 0; p < 2; ++p)
            if (std::memcmp(ref.v[p], td.acc[ply].v[p], (size_t)nnue::hiddenSize() * sizeof(int16_t)) != 0) {
                std::cerr << "NNUE accumulator mismatch at ply " << ply << " fen " << b.getFen() << std::endl;
                std::abort();
            }
    }
#endif
    const int s = nnue::evaluate(td.acc[ply], b.sideToMove() == Color::WHITE ? 0 : 1);
    return std::max(-20000, std::min(20000, s));
}

// root: build acc[0] from scratch and return the root's static eval
static int rootEval(ThreadData& td, const Board& b) {
    td.useNN = nnue::loaded();
    if (td.useNN) {
        nnueRefresh(b, td.acc[0]);
        td.accValid[0] = true;
    }
    return b.inCheck() ? -INF : evalAt(td, b, 0);
}

void setThreads(int n) {                              // never call while a search is running
    n = std::max(1, std::min(n, MAX_THREADS));
    while ((int)g_threads.size() > n) g_threads.pop_back();
    while ((int)g_threads.size() < n) {
        auto t = std::make_unique<ThreadData>();
        t->id = (int)g_threads.size();
        t->clear();
        g_threads.push_back(std::move(t));
    }
}
int threadCount() { return std::max<int>(1, (int)g_threads.size()); }
static void ensureThreads() { if (g_threads.empty()) setThreads(1); }

static uint64_t totalNodes() {
    uint64_t n = 0;
    for (auto& t : g_threads) n += __atomic_load_n(&t->nodes, __ATOMIC_RELAXED);
    return n;
}

static int  LMR[64][64];                 // [depth][moveCount]  (read-only after init)
static const int NO_EVAL = -INF;         // "no static eval at this ply" (side was in check)
static bool lmrReady = false;
static bool printInfo = true;

static void initLMR() {
    if (lmrReady) return;
    for (int d = 1; d < 64; ++d)
        for (int m = 1; m < 64; ++m)
            LMR[d][m] = (int)(0.75 + std::log((double)d) * std::log((double)m) / 2.25);
    lmrReady = true;
}

// Zeroing a multi-gigabyte table on one core takes seconds ("ucinewgame", "Clear Hash"): split it
// over as many threads as the search uses once it is big enough to matter.
static void clearTable() {
    const size_t n = TT.size();
    const int nt = (n * sizeof(Bucket) >= (64u << 20)) ? std::max(1, (int)g_threads.size()) : 1;
    if (nt <= 1) {
        std::fill(TT.begin(), TT.end(), Bucket());
        return;
    }
    std::vector<std::thread> workers;
    const size_t chunk = (n + nt - 1) / nt;
    for (int i = 0; i < nt; ++i) {
        const size_t lo = (size_t)i * chunk, hi = std::min(n, lo + chunk);
        if (lo >= hi) break;
        workers.emplace_back([lo, hi] { std::fill(TT.begin() + lo, TT.begin() + hi, Bucket()); });
    }
    for (auto& w : workers) w.join();
}

void clearTT() {
    ensureThreads();
    clearTable();
    ttAge = 0;
    for (auto& t : g_threads) t->clear();
}

void setSearchInfo(bool on) { printInfo = on; }

// mate scores are stored relative to the node, not the root
static int toTT(int s, int ply)   { return s >= MATE_BOUND ? s + ply : (s <= -MATE_BOUND ? s - ply : s); }
static int fromTT(int s, int ply) { return s >= MATE_BOUND ? s - ply : (s <= -MATE_BOUND ? s + ply : s); }

// ---------- time control ----------
struct Timeout {};

// Every thread counts its own nodes and polls the stop flags; only the main thread reads the clock
// and raises abortFlag so that all helpers unwind as well.
static inline void checkTime(ThreadData& td) {
    const uint64_t n = td.nodes + 1;
    __atomic_store_n(&td.nodes, n, __ATOMIC_RELAXED);
    if ((n & 2047) == 0) {
        if (stopFlag.load(std::memory_order_relaxed) || abortFlag.load(std::memory_order_relaxed)) throw Timeout();
        if (td.id == 0 && nowNs() > g_deadlineNs.load(std::memory_order_relaxed)) {
            abortFlag = true;
            throw Timeout();
        }
    }
}

uint64_t searchNodes() { return g_totalNodes; }

static inline int sideIdx(const Board& b) { return b.sideToMove() == Color::WHITE ? 0 : 1; }

static inline void gravity(int& h, int bonus) {
    h += bonus - h * std::abs(bonus) / MAX_HIST;      // "gravity": h stays within +-MAX_HIST
}

static inline int curIdx(int side, int pt, int to) { return (side * 6 + pt) * 64 + to; }

// quiet move score: main history + countermove history (1 ply back) + follow-up history (2 plies back)
static inline int histScore(ThreadData& td, int side, int ply, int pt, const Move& m) {
    const int to = m.to().index(), cur = curIdx(side, pt, to);
    int s = td.history[side][m.from().index()][to];
    if (td.stkTo[ply] >= 0)                  s += td.contHist[0][td.stkPt[ply] * 64 + td.stkTo[ply]][cur];
    if (ply >= 1 && td.stkTo[ply - 1] >= 0)  s += td.contHist[1][td.stkPt[ply - 1] * 64 + td.stkTo[ply - 1]][cur];
    return s;
}

// board must be in the position the move was played from
static void updateQuiet(ThreadData& td, const Board& b, int side, int ply, const Move& m, int bonus) {
    const int pt = (int)b.at(m.from()).type(), to = m.to().index(), cur = curIdx(side, pt, to);
    gravity(td.history[side][m.from().index()][to], bonus);
    if (td.stkTo[ply] >= 0)                  gravity(td.contHist[0][td.stkPt[ply] * 64 + td.stkTo[ply]][cur], bonus);
    if (ply >= 1 && td.stkTo[ply - 1] >= 0)  gravity(td.contHist[1][td.stkPt[ply - 1] * 64 + td.stkTo[ply - 1]][cur], bonus);
}

// captured piece type of a capture (en passant counts as a pawn); board = position before the move
static inline int capVictim(const Board& b, const Move& m) {
    int v = (int)b.at(m.to()).type();
    return v > 5 ? 0 : v;
}
static inline int capHistScore(const ThreadData& td, const Board& b, int side, const Move& m) {
    return td.capHist[side][(int)b.at(m.from()).type()][m.to().index()][capVictim(b, m)];
}
static inline void updateCapture(ThreadData& td, const Board& b, int side, const Move& m, int bonus) {
    gravity(td.capHist[side][(int)b.at(m.from()).type()][m.to().index()][capVictim(b, m)], bonus);
}

static int see(const Board& b, const Move& m);       // defined below

// ---------- move ordering ----------
// tt move > good captures (MVV-LVA, SEE >= 0) > killers > countermove > quiets by combined history
// > losing captures (SEE < 0).
// Only assigns scores; callers use pickMove() so we never sort the whole list.
// useCont is false in quiescence, where the continuation context is not tracked.
static void orderMoves(ThreadData& td, const Board& b, Movelist& moves, Move ttMove, int ply, bool useCont = true) {
    const int side = sideIdx(b);
    const Move counter = (useCont && td.stkTo[ply] >= 0) ? td.counterMove[td.stkPt[ply]][td.stkTo[ply]] : Move();
    for (Move& m : moves) {
        int s;
        if (m == ttMove) {
            s = S_TT;
        } else if (b.isCapture(m)) {
            int v = (int)b.at(m.to()).type();
            if (v > 5) v = 0;                           // en passant
            int a = (int)b.at(m.from()).type();
            const int mvvLva = 100 + 10 * ORDER_VALUE[v] - ORDER_VALUE[a]
                             + td.capHist[side][a][m.to().index()][v] / 16;   // +-512: reorders within a band only
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
        } else if (m == td.killers[ply][0]) {
            s = S_KILLER1;
        } else if (m == td.killers[ply][1]) {
            s = S_KILLER2;
        } else if (counter != Move() && m == counter) {
            s = S_COUNTER;
        } else {
            if (useCont) s = histScore(td, side, ply, (int)b.at(m.from()).type(), m);
            else         s = td.history[side][m.from().index()][m.to().index()];
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
static int quiesce(ThreadData& td, Board& b, int alpha, int beta, int ply) {
    checkTime(td);
    // isRepetition(1): a single earlier occurrence already counts as a draw inside the search
    if (b.isInsufficientMaterial() || b.isHalfMoveDraw() || b.isRepetition(1)) return 0;
    if (ply >= MAX_PLY) return evalAt(td, b, ply);

    const bool inCheck = b.inCheck();
    const int alphaOrig = alpha;

    uint64_t key = b.hash();
    Move ttMove{};
    TTEntry e;
    if (ttProbe(key, e)) {
        ttMove = Move(e.move);
        int s = fromTT(e.score, ply);
        if (e.flag() == EXACT) return s;
        if (e.flag() == LOWER && s >= beta) return s;
        if (e.flag() == UPPER && s <= alpha) return s;
    }

    Movelist moves;
    int stand = -INF, best;
    if (inCheck) {
        movegen::legalmoves(moves, b);
        if (moves.empty()) return -(MATE - ply);
        best = -INF;
    } else {
        stand = evalAt(td, b, ply);
        if (stand >= beta) return stand;
        if (stand + BIG_DELTA < alpha) return stand + BIG_DELTA;
        if (stand > alpha) alpha = stand;
        best = stand;
        movegen::legalmoves<movegen::MoveGenType::CAPTURE>(moves, b);
    }
    orderMoves(td, b, moves, ttMove, ply, false);

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

        accSetMove(td, b, m, ply + 1);
        b.makeMove(m);
        ttPrefetch(b.hash());
        int s = -quiesce(td, b, -beta, -alpha, ply + 1);
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
static int negamax(ThreadData& td, Board& b, int depth, int alpha, int beta, int ply, bool canNull, Move excluded = Move()) {
    checkTime(td);
    td.pvLen[ply] = ply;                              // empty PV until a move raises alpha in a PV node
    if (b.isInsufficientMaterial() || b.isHalfMoveDraw() || b.isRepetition(1)) return 0;
    if (ply >= MAX_PLY) return evalAt(td, b, ply);

    // mate distance pruning: no line from here can score better than mating on the next ply, or
    // worse than being mated right now. If that already closes the window, a shorter mate was found
    // elsewhere in the tree and this node cannot improve on it. (Skipped at the root.)
    if (ply > 0) {
        alpha = std::max(alpha, -MATE + ply);
        beta  = std::min(beta,   MATE - ply - 1);
        if (alpha >= beta) return alpha;
    }

    const bool inCheck = b.inCheck();
    if (inCheck) ++depth;
    if (depth <= 0) return quiesce(td, b, alpha, beta, ply);

    const bool pvNode = beta - alpha > 1;
    // a singular-extension verification search: same position as its parent, one move excluded.
    // Its result only holds for "everything except that move", so it never reads or writes the TT.
    const bool singular = !(excluded == Move());

    uint64_t key = b.hash();
    const int alphaOrig = alpha;
    Move ttMove{};
    int ttScore = 0, ttDepth = -1, ttFlag = 0;
    TTEntry e;
    if (!singular && ttProbe(key, e)) {
        ttMove = Move(e.move);
        ttScore = fromTT(e.score, ply); ttDepth = e.depth; ttFlag = e.flag();
        // only cut in non-PV nodes: keeps the PV exact and avoids TT-induced oddities
        if (!pvNode && e.depth >= depth) {
            int s = fromTT(e.score, ply);
            if (e.flag() == EXACT) return s;
            if (e.flag() == LOWER && s >= beta) return s;
            if (e.flag() == UPPER && s <= alpha) return s;
        }
    }

    // ---- Syzygy WDL probe (non-root, not inside a singular verification search) ----
    // A win/loss is only a bound (a faster win or a longer loss may exist), a draw is exact.
    int tbFloor = -INF, tbCeil = INF;
    if (!singular && ply > 0 && depth >= tbProbeDepth() && b.halfMoveClock() == 0 && tbCanProbe(b)) {
        int wdl;
        if (tbProbeWDL(b, wdl)) {
            int score, flag;
            if (wdl >= 2)       { score =  TB_WIN - ply; flag = LOWER; }
            else if (wdl <= -2) { score = -TB_WIN + ply; flag = UPPER; }
            else                { score = 0;             flag = EXACT; }   // incl. cursed win / blessed loss
            if (flag == EXACT || (flag == LOWER && score >= beta) || (flag == UPPER && score <= alpha)) {
                ttStore(key, 0, flag == LOWER ? TB_WIN : (flag == UPPER ? -TB_WIN : 0), std::min(depth + 6, 100), flag);
                return score;
            }
            if (pvNode) {
                if (flag == LOWER) { tbFloor = score; alpha = std::max(alpha, score); }
                else               tbCeil = score;
            }
        }
    }

    // internal iterative reduction: no hash move here, so search shallower to find one cheaply
    if (!singular && depth >= IIR_MIN_DEPTH && ttMove == Move()) --depth;

    // static eval for every node that is not in check (PV nodes too: they feed "improving" below)
    const bool prunable = !pvNode && !inCheck && std::abs(beta) < MATE_BOUND;
    int staticEval = NO_EVAL;
    if (!inCheck) staticEval = evalAt(td, b, ply);
    td.evalStack[ply] = staticEval;

    // improving: our static eval is better than two plies ago (same side to move); fall back to four
    // plies ago if we were in check then. When improving we trust the position more and prune/reduce
    // less aggressively where it is risky, and more where it is safe (see each use below).
    bool improving = false;
    if (!inCheck) {
        int prev = ply >= 2 ? td.evalStack[ply - 2] : NO_EVAL;
        if (prev == NO_EVAL && ply >= 4) prev = td.evalStack[ply - 4];
        improving = prev != NO_EVAL && staticEval > prev;
    }

    if (prunable && !singular && depth <= RAZOR_MAX_DEPTH && staticEval + RAZOR_BASE + RAZOR_MARGIN * depth <= alpha) {
        const int v = quiesce(td, b, alpha - 1, alpha, ply);
        if (v < alpha) return v;
    }

    // reverse futility pruning: so far above beta that a shallow search will not change that;
    // an improving side needs a smaller margin
    if (prunable && !singular && depth <= RFP_MAX_DEPTH && staticEval - RFP_MARGIN * (depth - improving) >= beta && hasNonPawnMaterial(b))
        return staticEval - RFP_MARGIN * (depth - improving);

    // --- null move pruning: if passing still beats beta, this node is almost surely a cut ---
    if (!pvNode && canNull && !inCheck && depth >= NMP_MIN_DEPTH &&
        std::abs(beta) < MATE_BOUND && hasNonPawnMaterial(b) && staticEval >= beta) {
        int R = 2 + depth / 4;
        td.stkTo[ply + 1] = -1; td.stkPt[ply + 1] = 0;
        accSetNull(td, ply + 1);
        b.makeNullMove();
        int s = -negamax(td, b, depth - 1 - R, -beta, -beta + 1, ply + 1, false);
        b.unmakeNullMove();
        if (s >= beta) return s >= MATE_BOUND ? beta : s;     // never trust unproven mate scores
    }

    if (prunable && !singular && depth >= PROBCUT_MIN_DEPTH) {
        const int probBeta = beta + PROBCUT_MARGIN;
        if (!(ttDepth >= depth - 3 && ttFlag != LOWER && ttScore < probBeta)) {
            Movelist caps;
            movegen::legalmoves<movegen::MoveGenType::CAPTURE>(caps, b);
            orderMoves(td, b, caps, ttMove, ply);
            for (int i = 0; i < caps.size(); ++i) {
                pickMove(caps, i);
                const Move m = caps[i];
                if (!b.isCapture(m) && m.typeOf() != Move::PROMOTION) continue;
                if (see(b,m) < probBeta - staticEval) continue;
                const int movedPt = (int)b.at(m.from()).type();
                accSetMove(td, b, m, ply + 1);
                b.makeMove(m);
                td.stkPt[ply + 1] = movedPt; td.stkTo[ply + 1] = m.to().index();
                int s = -quiesce(td, b, -probBeta, -probBeta + 1, ply + 1);
                if (s >= probBeta) {
                    s = -negamax(td, b, depth - PROBCUT_REDUCTION, -probBeta, -probBeta + 1, ply + 1, true);
                }
                b.unmakeMove(m);
                if (s >= probBeta) {
                    ttStore(key, m.move(), toTT(s, ply), depth - PROBCUT_REDUCTION + 1, LOWER);
                    return s - (probBeta - beta);
                }
            }
        }
    }

    Movelist moves;
    movegen::legalmoves(moves, b);
    if (moves.empty()) return inCheck ? -(MATE - ply) : 0;
    orderMoves(td, b, moves, ttMove, ply);

    const int side = sideIdx(b);
    int best = tbFloor;
    Move bestMove{};
    Move quiets[64];
    int nq = 0;
    Move captures[32];                                 // captures tried before a cutoff (for capture history malus)
    int ncap = 0;
    // LMR context: a noisy hash move means the quiet moves here are probably worse; a countermove
    // is a quiet move that already refuted the opponent's last move once.
    const bool ttNoisy = !(ttMove == Move()) && (b.isCapture(ttMove) || ttMove.typeOf() == Move::PROMOTION);
    const Move counter = td.stkTo[ply] >= 0 ? td.counterMove[td.stkPt[ply]][td.stkTo[ply]] : Move();

    for (int i = 0; i < moves.size(); ++i) {
        pickMove(moves, i);
        const Move m = moves[i];
        if (singular && m == excluded) continue;
        const bool quiet = !b.isCapture(m) && m.typeOf() != Move::PROMOTION;
        const bool isKiller = m == td.killers[ply][0] || m == td.killers[ply][1];
        const int movedPt = (int)b.at(m.from()).type();
        const int h = quiet ? histScore(td, side, ply, movedPt, m)
                            : (b.isCapture(m) ? capHistScore(td, b, side, m) : 0);

        int ext = 0;
        if (i == 0 && !singular && ply > 0 && depth >= SE_MIN_DEPTH && m == ttMove && ttFlag != UPPER && ttDepth >= depth - 3 && std::abs(ttScore) < MATE_BOUND) {
            const int sBeta = ttScore - SE_MARGIN * depth;
            const int sv = negamax(td, b, (depth - 1 ) / 2, sBeta - 1, sBeta, ply, false, m);
            if (sv < sBeta) ext = 1;
            else if (sBeta >= beta) return sBeta;
            else if (ttScore >= beta) ext = -1;
        }

        // SEE pruning: at low depth skip captures that clearly lose material. orderMoves already
        // flagged losing captures with a negative score, so SEE is only computed for those (and the
        // hash move, which has a huge score, is never pruned). Never the first move, never in PV or
        // check nodes, and (below) never a move that gives check.
        const bool seeBad = prunable && i > 0 && depth <= SEE_PRUNE_MAX_DEPTH && best > -MATE_BOUND &&
                            !quiet && m.score() < 0 && see(b, m) < -SEE_PRUNE_MARGIN * depth;

        accSetMove(td, b, m, ply + 1);
        b.makeMove(m);
        td.stkPt[ply + 1] = movedPt; td.stkTo[ply + 1] = m.to().index();
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

        ttPrefetch(b.hash());                              // the child's first TT probe happens right below
        int s;
        if (i == 0) {
            s = -negamax(td, b, depth - 1 + ext, -beta, -alpha, ply + 1, true);
        } else {
            // late move reduction: late quiet moves (and clearly losing captures) are probably bad,
            // so search them shallower. Adjustments push the base reduction up or down.
            int r = 0;
            const bool badCapture = !quiet && m.score() < S_BAD_THRESH;
            if (depth >= LMR_MIN_DEPTH && i >= LMR_MIN_MOVES && (quiet || badCapture) && !inCheck && !b.inCheck()) {
                r = LMR[std::min(depth, 63)][std::min(i, 63)];
                if (pvNode) --r;                            // PV nodes matter more: reduce less
                if (isKiller) --r;
                if (m == counter) --r;                      // known refutation of the previous move
                if (!improving) ++r;                        // eval going nowhere: reduce more
                if (ttNoisy && quiet) ++r;                  // the hash move is a capture: quiets rarely beat it
                if (badCapture) --r;                        // losing captures: more careful than quiets
                r -= quiet ? h / 12288 : h / 4096;          // good history: reduce less, bad: more (capture history has a smaller range)
                r = std::max(0, std::min(r, depth - 2));
            }
            s = -negamax(td, b, depth - 1 - r, -alpha - 1, -alpha, ply + 1, true);
            if (s > alpha && r > 0)                        // reduced search surprised us: verify at full depth
                s = -negamax(td, b, depth - 1, -alpha - 1, -alpha, ply + 1, true);
            if (s > alpha && s < beta)                     // PVS re-search with the full window
                s = -negamax(td, b, depth - 1, -beta, -alpha, ply + 1, true);
        }
        b.unmakeMove(m);

        if (s > best) { best = s; bestMove = m; }
        if (s > alpha) {
            alpha = s;
            if (pvNode) {                                  // this move + the child's line becomes our PV
                td.pv[ply][ply] = m;
                const int cl = td.pvLen[ply + 1];
                for (int k = ply + 1; k < cl; ++k) td.pv[ply][k] = td.pv[ply + 1][k];
                td.pvLen[ply] = cl;
            }
        }
        if (alpha >= beta) {
            const int bonus = std::min(depth * depth, 1200);
            if (quiet) {
                if (!(m == td.killers[ply][0])) { td.killers[ply][1] = td.killers[ply][0]; td.killers[ply][0] = m; }
                updateQuiet(td, b, side, ply, m, bonus);
                for (int q = 0; q < nq; ++q) updateQuiet(td, b, side, ply, quiets[q], -bonus);   // penalise quiets that failed
                if (td.stkTo[ply] >= 0) td.counterMove[td.stkPt[ply]][td.stkTo[ply]] = m;
            } else if (b.isCapture(m)) {
                updateCapture(td, b, side, m, bonus);
            }
            // captures that were tried first but did not cut are penalised, whatever move finally did
            for (int c = 0; c < ncap; ++c) updateCapture(td, b, side, captures[c], -bonus);
            break;
        }
        if (quiet && nq < 64) quiets[nq++] = m;
        else if (!quiet && ncap < 32 && b.isCapture(m)) captures[ncap++] = m;
    }

    if (pvNode && best > tbCeil) best = tbCeil;       // a tablebase loss caps what this node can score

    // fail-low nodes have no real best move, so store none (ttStore keeps any older one)
    if (!singular) {
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

// PV as a UCI string; falls back to the single best move if the stored line does not start with it.
static std::string pvString(const Move* pv, int len, const Move& best) {
    std::string s;
    if (len > 0 && pv[0] == best) {
        for (int i = 0; i < len; ++i) s += (i ? " " : "") + uci::moveToUci(pv[i]);
    } else {
        s = uci::moveToUci(best);
    }
    return s;
}

// Remember the result of a finished iteration (used to pick the best thread at the end of the search).
static void recordDone(ThreadData& td, int depth, int score, const Move& best) {
    td.doneDepth = depth;
    td.doneScore = score;
    td.doneBest  = best;
    int n = 0;
    if (td.pvLen[0] > 0 && td.pv[0][0] == best) {
        n = td.pvLen[0];
        for (int i = 0; i < n; ++i) td.donePv[i] = td.pv[0][i];
    }
    td.donePvLen = n;
}

// ---------- root search (PVS) ----------
static int rootSearch(ThreadData& td, Board& b, Movelist& moves, int depth, int alpha, int beta, Move& best) {
    orderMoves(td, b, moves, best, 0);
    std::stable_sort(moves.begin(), moves.end(),
                     [](const Move& x, const Move& y) { return x.score() > y.score(); });
    int bestScore = -INF;
    bool first = true;
    for (const Move& m : moves) {
        const uint64_t n0 = td.nodes;
        td.stkPt[1] = (int)b.at(m.from()).type(); td.stkTo[1] = m.to().index();
        accSetMove(td, b, m, 1);
        b.makeMove(m);
        int s;
        if (first) {
            s = -negamax(td, b, depth - 1, -beta, -alpha, 1, true);
        } else {
            s = -negamax(td, b, depth - 1, -alpha - 1, -alpha, 1, true);
            if (s > alpha && s < beta)
                s = -negamax(td, b, depth - 1, -beta, -alpha, 1, true);
        }
        b.unmakeMove(m);
        first = false;

        if (s > bestScore) bestScore = s;
        if (s > alpha) {
            alpha = s;
            best = m;
            td.rootBestNodes = td.nodes - n0;
            td.pv[0][0] = m;
            const int cl = td.pvLen[1];
            for (int k = 1; k < cl; ++k) td.pv[0][k] = td.pv[1][k];
            td.pvLen[0] = std::max(cl, 1);
            if (alpha >= beta) break;
        }
    }
    return bestScore;
}

// One iteration with aspiration windows (shared by the main thread and the helpers).
// iterNodes is the node count of the final (successful) pass.
static int aspirationSearch(ThreadData& td, Board& b, Movelist& moves, int d, int lastScore,
                            Move& best, uint64_t& iterNodes) {
    int alpha = -INF, beta = INF, delta = ASP_DELTA;
    if (d >= ASP_MIN_DEPTH && std::abs(lastScore) < MATE_BOUND) {
        alpha = std::max(lastScore - delta, -INF);
        beta  = std::min(lastScore + delta,  INF);
    }
    int score;
    while (true) {
        const uint64_t n0 = td.nodes;
        score = rootSearch(td, b, moves, d, alpha, beta, best);
        iterNodes = td.nodes - n0;
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
    return score;
}

// ---------- Lazy SMP helper thread ----------
// Helpers search the same root with their own history tables and share only the transposition table.
// To keep them from walking in lock-step they skip some iterations (different pattern per thread), so
// at any moment the threads are working on a mix of depths and feed each other through the TT.
// Helpers print nothing and never decide when to stop; the main thread aborts them.
static const int SKIP_SIZE[20]  = {1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4};
static const int SKIP_PHASE[20] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 6, 7};

static void helperMain(ThreadData& td, Board b, Movelist moves, int maxDepth) {
    try {
        Move best = moves[0];
        td.stkTo[0] = -1; td.stkPt[0] = 0;
        td.pvLen[0] = 0;
        td.evalStack[0] = rootEval(td, b);
        const int idx = (td.id - 1) % 20;
        int prevScore = 0;
        for (int d = 1; d <= maxDepth; ++d) {
            if (abortFlag.load(std::memory_order_relaxed) || stopFlag.load(std::memory_order_relaxed)) break;
            if (((d + SKIP_PHASE[idx]) / SKIP_SIZE[idx]) & 1) continue;
            uint64_t iterNodes = 0;
            prevScore = aspirationSearch(td, b, moves, d, prevScore, best, iterNodes);
            recordDone(td, d, prevScore, best);
            if (std::abs(prevScore) >= MATE_BOUND) break;
        }
    } catch (const Timeout&) {
        // main thread asked everybody to stop
    }
}

// ---------- choosing the final move among the threads ----------
// Every thread votes for the best move of its last completed iteration. A vote is worth
// (score - lowest thread score + 14) * depth, so deeper and better-scoring threads count more.
// The main thread wins when it reached the requested depth, so "go depth N" stays deterministic in
// meaning. Mate scores are not voted on: the thread with the best (shortest mate / longest defence)
// score is taken. Returns the index of the thread whose result is played.
static int pickBestThread(int maxDepth) {
    const int n = (int)g_threads.size();
    int first = -1;
    for (int i = 0; i < n; ++i)
        if (g_threads[i]->doneDepth > 0) { first = i; break; }
    if (first < 0) return 0;
    const ThreadData& mainTd = *g_threads[0];
    if (mainTd.doneDepth >= maxDepth) return 0;

    bool mate = mainTd.doneDepth > 0 && mainTd.doneScore <= -MATE_BOUND;
    for (int i = 0; i < n; ++i)
        if (g_threads[i]->doneDepth > 0 && g_threads[i]->doneScore >= MATE_BOUND) mate = true;
    int best = first;
    if (mate) {
        for (int i = 0; i < n; ++i)
            if (g_threads[i]->doneDepth > 0 && g_threads[i]->doneScore > g_threads[best]->doneScore) best = i;
        return best;
    }

    int minScore = INF;
    for (int i = 0; i < n; ++i)
        if (g_threads[i]->doneDepth > 0) minScore = std::min(minScore, g_threads[i]->doneScore);

    struct Vote { uint16_t mv; int64_t v; };
    std::vector<Vote> votes;
    for (int i = 0; i < n; ++i) {
        const ThreadData& t = *g_threads[i];
        if (t.doneDepth <= 0) continue;
        const int64_t w = (int64_t)(t.doneScore - minScore + 14) * t.doneDepth;
        const uint16_t mv = t.doneBest.move();
        bool found = false;
        for (Vote& x : votes) if (x.mv == mv) { x.v += w; found = true; break; }
        if (!found) votes.push_back({mv, w});
    }
    auto votesFor = [&](const Move& m) -> int64_t {
        for (const Vote& x : votes) if (x.mv == m.move()) return x.v;
        return 0;
    };
    for (int i = 0; i < n; ++i) {
        const ThreadData& t = *g_threads[i];
        if (t.doneDepth <= 0) continue;
        if (votesFor(t.doneBest) > votesFor(g_threads[best]->doneBest)) best = i;   // ties keep the lower index
    }
    return best;
}

// ---------- iterative deepening with aspiration windows and time management ----------
// Hard limit: the search is aborted mid-iteration (checkTime). Soft limit: checked after every
// finished iteration and stretched or shrunk by how settled the search looks.
static const double STAB_SCALE[9] = {1.40, 1.20, 1.05, 0.95, 0.88, 0.82, 0.78, 0.75, 0.72};

// Note: on timeout the board passed in is left mid-search, so callers must pass a copy.
Move search(Board& b, const SearchLimits& lim) {
    initLMR();
    ensureThreads();
    const int64_t startNs = nowNs();
    {
        std::lock_guard<std::mutex> lk(g_timeMx);
        g_hardNs = lim.hardMs * 1000000LL;
        g_softNs = lim.softMs * 1000000LL;
        g_searchStartNs = startNs;
        // while pondering there is no hard limit; ponderHit() installs it later
        g_deadlineNs = g_pondering.load() ? INT64_MAX : startNs + g_hardNs;
    }
    abortFlag = false;
    g_totalNodes = 0;
    g_lastPv.clear();                               // early returns below leave "no PV"
    const uint64_t rootKey = b.hash();              // b is left mid-search after a timeout
    ttAge = (ttAge + 1) & 63;                       // new search = new TT generation
    for (auto& up : g_threads) {
        ThreadData& t = *up;
        t.nodes = 0;
        t.rootBestNodes = 0;
        t.pvLen[0] = 0;
        t.doneDepth = t.doneScore = t.donePvLen = 0;
        t.doneBest = Move();
        for (auto& k : t.killers) k[0] = k[1] = Move();
        for (auto& s : t.history) for (auto& f : s) for (int& h : f) h /= 2;   // age, don't wipe
        for (auto& a : t.capHist) for (auto& b2 : a) for (auto& c : b2) for (int& h : c) h /= 2;
    }

    Movelist moves;
    movegen::legalmoves(moves, b);
    if (moves.empty()) return Move();
    if (moves.size() == 1 && !lim.infinite) return moves[0];   // forced move: don't burn the clock

    // ---- Syzygy root probe (needs the DTZ .rtbz files) ----
    // Win or loss: play Fathom's move (fastest win / toughest defence). Draw-ish: only search the moves
    // that keep the tablebase result, so the engine never throws a draw away or walks into a loss.
    if (!lim.infinite && tbCanProbe(b)) {
        TBRoot r;
        if (tbProbeRoot(b, r)) {
            if (r.wdl >= 2 || r.wdl <= -2) {
                if (printInfo)
                    std::cout << "info depth 1 score cp " << (r.wdl > 0 ? TB_WIN : -TB_WIN) << " nodes 0 tbhits 1 pv " << uci::moveToUci(r.best) << std::endl;
                return r.best;
            }
            moves = r.keep;
            if (moves.size() == 1) return moves[0];
        }
    }

    ThreadData& td = *g_threads[0];
    Move best = moves[0], prevBest = best;
    int prevScore = 0, stability = 0;
    td.stkTo[0] = -1; td.stkPt[0] = 0;                            // no move led into the root
    td.evalStack[0] = rootEval(td, b);        // root eval, read by plies 2 and 4

    // Lazy SMP: start the helpers on copies of the root position (after the TB filter above)
    std::vector<std::thread> helpers;
    for (int i = 1; i < (int)g_threads.size(); ++i)
        helpers.emplace_back(helperMain, std::ref(*g_threads[i]), b, moves, lim.maxDepth);

    for (int d = 1; d <= lim.maxDepth; ++d) {
        try {
            const int lastScore = prevScore;
            uint64_t iterNodes = 0;
            const int score = aspirationSearch(td, b, moves, d, lastScore, best, iterNodes);
            recordDone(td, d, score, best);

            prevScore = score;
            const int64_t now = nowNs();
            const long long t = (now - startNs) / 1000000;
            if (printInfo) {
                const uint64_t total = totalNodes();
                std::cout << "info depth " << d << " score " << scoreStr(score)
                          << " nodes " << total << " nps " << (t > 0 ? total * 1000 / t : total)
                          << " hashfull " << hashfull() << " time " << t
                          << " pv " << pvString(td.pv[0], td.pvLen[0], best) << std::endl;
            }
            if (score >= MATE_BOUND) break;

            // ---- time management ----
            stability = (d >= 2 && best == prevBest) ? std::min(stability + 1, 8) : 0;
            prevBest = best;
            // while pondering there is no clock; after "ponderhit" the time spent pondering counts too
            // (t is measured from the start of the search), so a long ponder lets us move at once
            if (!lim.infinite && !g_pondering.load()) {
                double target = (double)lim.softMs;
                if (lim.dynamic && d >= 6) {
                    double scale = STAB_SCALE[stability];                       // same move for long -> less time
                    const int drop = lastScore - score;
                    if (drop > 0)        scale *= 1.0 + std::min(drop, 80) * 0.006;   // score falling -> think more
                    else if (drop < -20) scale *= 0.95;                         // clearly improving -> relax
                    if (d >= 8 && iterNodes > 0) {                              // one dominant root move -> less time
                        double frac = (double)td.rootBestNodes / (double)iterNodes;
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

    abortFlag = true;                                 // tell the helpers to unwind, then wait for them
    for (auto& h : helpers) h.join();
    g_totalNodes = totalNodes();

    // Lazy SMP: the threads vote on the move to play (the main thread's live result if it wins, since it
    // may have improved on its last completed iteration)
    const int pick = pickBestThread(lim.maxDepth);
    Move result = best;
    g_lastPvKey = rootKey;
    if (pick == 0) {
        if (td.pvLen[0] > 0 && td.pv[0][0] == best) g_lastPv.assign(td.pv[0], td.pv[0] + td.pvLen[0]);
    } else {
        const ThreadData& t = *g_threads[pick];
        result = t.doneBest;
        g_lastPv.assign(t.donePv, t.donePv + t.donePvLen);
        if (printInfo)
            std::cout << "info depth " << t.doneDepth << " score " << scoreStr(t.doneScore)
                      << " nodes " << g_totalNodes << " pv " << pvString(t.donePv, t.donePvLen, t.doneBest) << std::endl;
    }
    return result;
}

// Convenience overload: fixed depth and a single time budget (terminal play, bench).
Move search(Board& b, int maxDepth, int ms) {
    setPondering(false);
    SearchLimits lim;
    lim.maxDepth = maxDepth;
    lim.hardMs = ms;
    lim.softMs = (int64_t)(ms * 0.6);
    lim.dynamic = true;
    return search(b, lim);
}

// Last resort for the ponder move (forced moves, tablebase roots, book moves have no PV): a tiny
// search of the position after our move, at most ~30 ms. Only called when no search is running.
static Move quickReply(const Board& after) {
    Movelist moves;
    movegen::legalmoves(moves, after);
    if (moves.empty()) return Move();
    if (moves.size() == 1) return moves[0];
    ensureThreads();
    ThreadData& td = *g_threads[0];
    Board b = after;
    td.stkTo[0] = -1; td.stkPt[0] = 0;
    td.evalStack[0] = rootEval(td, b);
    abortFlag = false;                                // the finished search left it set
    g_deadlineNs = nowNs() + 30000000LL;
    Move best = moves[0];
    int score = 0;
    try {
        for (int d = 1; d <= 8; ++d) {
            uint64_t iterNodes = 0;
            score = aspirationSearch(td, b, moves, d, score, best, iterNodes);
            if (std::abs(score) >= MATE_BOUND) break;
        }
    } catch (const Timeout&) {
        // time is up: use what the last iterations found
    }
    return best;
}

// The reply we expect after `best`, for the "ponder" part of "bestmove". `root` is the position `best`
// was searched from. Sources, in order: the PV of the search that chose `best`, the transposition table,
// a quick search. Returns Move() if nothing usable is found.
Move ponderMoveFor(const Board& root, Move best) {
    if (best == Move()) return Move();
    Board b = root;
    b.makeMove(best);
    Movelist legal;
    movegen::legalmoves(legal, b);
    if (legal.empty()) return Move();
    auto legalMatch = [&](const Move& c) -> Move {
        for (const Move& m : legal)
            if (m == c) return m;
        return Move();
    };

    if (g_lastPvKey == root.hash() && g_lastPv.size() >= 2 && g_lastPv[0] == best) {
        const Move m = legalMatch(g_lastPv[1]);
        if (!(m == Move())) return m;
    }
    TTEntry e;
    if (ttProbe(b.hash(), e) && e.move != 0) {
        const Move m = legalMatch(Move(e.move));
        if (!(m == Move())) return m;
    }
    if (stopRequested()) return Move();
    return quickReply(b);
}