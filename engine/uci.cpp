#include "uci.hpp"
#include "book.hpp"
#include "search.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using namespace chess;

static int moveOverhead = 30;   // ms reserved for GUI / network / process latency (UCI option "Move Overhead")

void uciLoop() {
    Board board(constants::STARTPOS);
    std::string line;

    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "uci") {
            std::cout << "id name MyChessAI\nid author me\n"
                      << "option name Move Overhead type spin default 30 min 0 max 5000\n"
                      << "uciok" << std::endl;
        } else if (cmd == "isready") {
            std::cout << "readyok" << std::endl;
        } else if (cmd == "setoption") {
            std::string tok, name, val;
            ss >> tok;                                   // "name"
            while (ss >> tok && tok != "value") name += (name.empty() ? "" : " ") + tok;
            ss >> val;
            if (name == "Move Overhead" && !val.empty()) moveOverhead = std::max(0, std::atoi(val.c_str()));
        } else if (cmd == "ucinewgame") {
            clearTT();
        } else if (cmd == "position") {
            std::string t;
            ss >> t;
            if (t == "startpos") {
                board = Board(constants::STARTPOS);
                ss >> t;
            } else {
                std::string fen;
                while (ss >> t && t != "moves") fen += (fen.empty() ? "" : " ") + t;
                board = Board(fen);
            }
            if (t == "moves") {
                std::string mv;
                while (ss >> mv) board.makeMove(uci::uciToMove(board, mv));
            }
        } else if (cmd == "go") {
            int wtime = -1, btime = -1, winc = 0, binc = 0, movetime = -1, depth = 64;
            bool depthGiven = false, infinite = false;
            std::string tok;
            while (ss >> tok) {
                if (tok == "wtime") ss >> wtime;
                else if (tok == "btime") ss >> btime;
                else if (tok == "winc") ss >> winc;
                else if (tok == "binc") ss >> binc;
                else if (tok == "movetime") ss >> movetime;
                else if (tok == "depth") { ss >> depth; depthGiven = true; }
                else if (tok == "infinite") infinite = true;
            }

            // time per move: about 1/30 of the clock plus most of the increment
            int ms;
            if (movetime > 0) {
                ms = movetime;
            } else {
                bool white = board.sideToMove() == Color::WHITE;
                int clock = white ? wtime : btime, inc = white ? winc : binc;
                if (clock < 0) ms = (depthGiven || infinite) ? 1000000000 : 3000;   // fixed depth: don't cut it off
                else {
                    ms = std::max(20, std::min(std::min(clock / 30 + inc * 8 / 10, 15000), clock / 2));
                    ms = std::min(ms, std::max(5, clock - moveOverhead));   // never plan past the clock
                }
            }
            if (movetime > 0 || wtime >= 0 || btime >= 0) ms = std::max(5, ms - moveOverhead);

            // opening book first
            std::string bm = bookMove(board);
            if (!bm.empty()) {
                std::cout << "bestmove " << bm << std::endl;
                continue;
            }

            Board work = board;
            Move m = search(work, depth, ms);
            if (m == Move()) std::cout << "bestmove 0000" << std::endl;
            else std::cout << "bestmove " << uci::moveToUci(m) << std::endl;
        } else if (cmd == "quit") {
            break;
        }
    }
}