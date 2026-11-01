#pragma once
#include <string>
#include "chess.hpp"

// Syzygy endgame tablebases through Fathom (tbprobe.c).

// Loads the tables from `path` (several folders: ':' separated, ';' on Windows). Returns true if at
// least one table was found. An empty path unloads everything. Never call while a search is running.
bool tbInit(const std::string& path);

int  tbLargest();                 // most pieces any loaded table covers (0 = none loaded)
bool tbEnabled();                 // tables are loaded and switched on
void tbSetEnabled(bool on);       // temporary switch (bench uses it so node counts stay fixed)

void tbSetProbeDepth(int d);      // don't probe inside the tree below this remaining depth
int  tbProbeDepth();

// True if the position is in range of the loaded tables and may be probed at all.
bool tbCanProbe(const chess::Board& b);

// In-tree probe. wdl: -2 loss, -1 blessed loss, 0 draw, 1 cursed win, 2 win (side to move's view).
// Needs halfmove clock 0 and no castling rights. Returns false if the probe isn't possible.
bool tbProbeWDL(const chess::Board& b, int& wdl);

struct TBRoot {
    chess::Move best;             // Fathom's pick (fastest win / longest resistance / a drawing move)
    int         wdl = 0;          // -2..2 as above
    chess::Movelist keep;         // all legal moves that keep the same wdl as the best move
};
// Root probe (needs the .rtbz DTZ files). Returns false if it isn't possible.
bool tbProbeRoot(const chess::Board& b, TBRoot& out);