#pragma once
#include <string>
#include "chess.hpp"

bool loadBook(const std::string& path);              // false if the file can't be read
std::string bookMove(const chess::Board& board);     // "" if the position isn't in the book