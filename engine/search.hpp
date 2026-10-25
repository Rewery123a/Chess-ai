#pragma once
#include "chess.hpp"

// Finds the best move: iterative deepening up to maxDepth, stopping after ms milliseconds.
chess::Move search(chess::Board& b, int maxDepth, int ms);

// Empties the transposition table (call at the start of a new game).
void clearTT();

// Turn the per-depth "info ..." output on/off (off for terminal play).
void setSearchInfo(bool on);


// Ask a running search to stop as soon as possible (thread-safe).
void requestStop();
void resetStop();