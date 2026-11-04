#pragma once
#include <string>
#include "chess.hpp"
#include "nnue.hpp"

int evaluate(const chess::Board& b);   // score from White's point of view (net if one is loaded, else handcrafted)
int evalSide(const chess::Board& b);   // score from the side to move's point of view

// NNUE: load a net file written by nnue_tools/train.py. Returns false (and fills *err) on failure, in which
// case the previous eval stays active. Never call while a search is running.
bool loadNNUE(const std::string& path, std::string* err = nullptr);
void unloadNNUE();                     // back to the handcrafted eval
bool nnueLoaded();

// Builds the NNUE accumulator for a position from scratch (used at the search root and for verification).
void nnueRefresh(const chess::Board& b, nnue::Accumulator& acc);

// Endgame knowledge applied on top of the net's score (the net was never shown mate scores and has few
// endgame positions). stmScore / the result are from the side to move's point of view.
//   mask bit 1: mop-up  (a side far ahead in material in a thin ending drives the enemy king to the edge)
//   mask bit 2: drawish pawnless endings (no pawns, at most a minor piece ahead; two knights vs bare king)
//   mask bit 4: opposite-coloured-bishop endings are scaled towards a draw
// Default 3. The net was trained on Stockfish scores that already contain the bishop rule, so test bit 4
// separately (SPRT). setNNUEEndgame() clears the eval cache; never call it while a search is running.
int  nnueAdjust(const chess::Board& b, int stmScore);
void setNNUEEndgame(int mask);
int  nnueEndgame();