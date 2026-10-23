#pragma once
#include "chess.hpp"

int evaluate(const chess::Board& b);   // score from White's point of view
int evalSide(const chess::Board& b);   // score from the side to move's point of view