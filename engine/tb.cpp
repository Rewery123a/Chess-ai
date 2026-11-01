#include "tb.hpp"
#include <algorithm>
#include <cstdint>

extern "C" {
    #include "tbprobe.h"
}

using namespace chess;

static bool g_loaded = false;
static bool g_enabled = true;
static int g_probeDepth = 1;

bool tbInit(const std::string& path) {
    if (g_loaded) { tb_free(); g_loaded = false; }
    if (path.empty() || path == "<empty>") return false;
    if (!tb_init(path.c_str()) || TB_LARGEST == 0) { tb_free(); return false; }
    g_loaded = true;
    return true;
}

int tbLargest()              { return g_loaded ? (int)TB_LARGEST : 0; }
bool tbEnabled()             { return g_loaded && g_enabled; }
void tbSetEnabled(bool on)   { g_enabled = on; }
void tbSetProbeDepth(int d)  { g_probeDepth = std::max(1,d); }
int tbProbeDepth()           { return g_probeDepth; }

bool tbCanProbe(const Board& b) {
    if (!tbEnabled()) return false;
    if (!b.castlingRights().isEmpty()) return false;
    return b.occ().count() <= tbLargest();
}

struct Bits { uint64_t w, b, k, q, r, bi, n, p; };

static Bits bitsOf(const Board& b) {
    Bits x;
    x.w = b.us(Color::WHITE).getBits();
    x.b = b.us(Color::BLACK).getBits();
    x.k = b.pieces(PieceType::KING).getBits();
    x.q = b.pieces(PieceType::QUEEN).getBits();
    x.r = b.pieces(PieceType::ROOK).getBits();
    x.bi = b.pieces(PieceType::BISHOP).getBits();
    x.n = b.pieces(PieceType::KNIGHT).getBits();
    x.p = b.pieces(PieceType::PAWN).getBits();
    return x;
}

static unsigned epOf(const Board& b) {
    return b.enpassantSq() == Square::NO_SQ ? 0u : (unsigned)b.enpassantSq().index();
}

bool tbProbeWDL(const Board& b, int& wdl) {
    if (!tbCanProbe(b) || b.halfMoveClock() != 0) return false;
    const Bits x = bitsOf(b);
    const unsigned r = tb_probe_wdl(x.w, x.b, x.k, x.q, x.r, x.bi, x.n, x.p, 0, 0, epOf(b), b.sideToMove() == Color::WHITE);
    if (r == TB_RESULT_FAILED) return false;
    wdl = (int)r - 2;
    return true;
}

bool tbProbeRoot(const Board& b, TBRoot& out) {
    if (!tbCanProbe(b)) return false;
    const Bits x = bitsOf(b);
    unsigned results[TB_MAX_MOVES];
    const unsigned r = tb_probe_root(x.w, x.b, x.k, x.q, x.r, x.bi, x.n, x.p, b.halfMoveClock(), 0, epOf(b), b.sideToMove() == Color::WHITE, results);
    if (r == TB_RESULT_FAILED || r == TB_RESULT_CHECKMATE || r == TB_RESULT_STALEMATE) return false;

    Movelist legal;
    movegen::legalmoves(legal, b);

    auto find = [&](unsigned from, unsigned to, unsigned promo) -> Move {
        for (const Move& m : legal) {
            if ((unsigned)m.from().index() != from || (unsigned)m.to().index() != to) continue;
            const bool isPromo = m.typeOf() == Move::PROMOTION;
            unsigned mp = 0;
            if (isPromo) {
                switch ((int)m.promotionType()) {
                    case (int)PieceType::QUEEN:  mp = 1; break;
                    case (int)PieceType::ROOK:   mp = 2; break;
                    case (int)PieceType::BISHOP: mp = 3; break;
                    default:                     mp = 4; break;
                }
            }
            if (mp == promo) return m;
        }
        return Move();
    };

    out.best = find(TB_GET_FROM(r), TB_GET_TO(r), TB_GET_PROMOTES(r));
    if (out.best == Move()) return false;
    out.wdl = (int)TB_GET_WDL(r) - 2;

    out.keep.clear();
    for (int i = 0; i < TB_MAX_MOVES && results[i] != TB_RESULT_FAILED; ++i) {
        if ((int)TB_GET_WDL(results[i]) - 2 != out.wdl) continue;
        const Move m = find(TB_GET_FROM(results[i]), TB_GET_TO(results[i]), TB_GET_PROMOTES(results[i]));
        if (!(m == Move())) out.keep.add(m);
    }
    if (out.keep.empty()) out.keep.add(out.best);
    return true;
}