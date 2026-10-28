#pragma once
#include <cstddef>
#include <cstdint>
#include "chess.hpp"

// Limits for one search. The hard limit aborts the search mid-iteration; the soft limit is checked
// between iterations and is scaled by best-move stability, score drops and root-move effort.
struct SearchLimits {
    int      maxDepth = 64;
    int64_t  softMs   = 1000000000;   // target time (only used when the search is timed)
    int64_t  hardMs   = 1000000000;   // never exceed this
    bool     dynamic  = false;        // true: scale the soft limit by how settled the search is
    bool     infinite = false;        // "go infinite": no forced-move shortcut, no soft stop
};

// Finds the best move under the given limits.
chess::Move search(chess::Board& b, const SearchLimits& lim);

// Convenience: iterative deepening up to maxDepth, stopping after ms milliseconds.
chess::Move search(chess::Board& b, int maxDepth, int ms);

// Empties the transposition table, history and killers (call at the start of a new game).
void clearTT();

// Resizes the transposition table (UCI "Hash", in MB). Never call while a search is running.
void setHashSize(size_t mb);
size_t hashSizeMB();

// Turn the per-depth "info ..." output on/off (off for terminal play).
void setSearchInfo(bool on);

// Ask a running search to stop as soon as possible (thread-safe).
void requestStop();
void resetStop();

// Number of nodes visited by the most recent search (used by "bench").
uint64_t searchNodes();