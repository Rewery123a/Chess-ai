#include "eval.hpp"
#include "tables.hpp"

using namespace chess;

// Piece types: 0 P, 1 N, 2 B, 3 R, 4 Q, 5 K
int evaluate(const Board& b) {
    int score = 0, wk = -1, bk = -1;
    bool queens = false;
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = b.at(Square(sq));
        if (p == Piece::NONE) continue;
        int pt = (int)p.type();
        bool white = p.color() == Color::WHITE;
        if (pt == 5) { (white ? wk : bk) = sq; continue; }
        if (pt == 4) queens = true;
        int v = PIECE_VALUE[pt] + PST[pt][white ? (sq ^ 56) : sq];
        score += white ? v : -v;
    }
    const int* kt = queens ? KING_MID_PST : KING_END_PST;
    if (wk >= 0) score += kt[wk ^ 56];
    if (bk >= 0) score -= kt[bk];
    return score;
}

int evalSide(const Board& b) {
    int s = evaluate(b);
    return b.sideToMove() == Color::WHITE ? s : -s;
}