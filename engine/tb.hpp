#pragma once 
#include <string>
#include "chess.hpp"

bool tbInit(const std::string& path);

int tbLargest();  
bool tbEnabled();   
void tbSetEnabled(bool on); 
void tbSetProbeDepth(int d);
int tbProbeDepth();

bool tbCanProbe(const chess::Board& b);
bool tbProbeWDL(const chess::Board& b, int& wdl);

struct TBRoot {
    chess::Move best;
    int wdl = 0;
    chess::Movelist keep;
};

bool tbProbeRoot(const chess::Board& b, TBRoot& out);