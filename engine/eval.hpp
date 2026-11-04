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