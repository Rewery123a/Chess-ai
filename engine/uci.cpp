#include "uci.hpp"
#include "book.hpp"
#include "search.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using namespace chess;

// Runs on its own thread so the main loop can still read "stop" / "quit" while thinking.
static void thinkAndReply(Board work, int depth, int ms) {
    std::string bm = bookMove(work);
    if (!bm.empty()) {
        std::cout << "bestmove " << bm << std::endl;
        return;
    }
    Move m = search(work, depth, ms);
    if (m == Move()) std::cout << "bestmove 0000" << std::endl;
    else std::cout << "bestmove " << uci::moveToUci(m) << std::endl;
}

void uciLoop() {
    Board board(constants::STARTPOS);
    std::string line;
    std::thread worker;
    int moveOverhead = 50;                 // ms kept in reserve for GUI / network lag

    auto finishSearch = [&]() {
        requestStop();
        if (worker.joinable()) worker.join();
    };

    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "uci") {
            std::cout << "id name MyChessAI\nid author me\n"
                      << "option name BookFile type string default Perfect2023.bin\n"
                      << "option name MoveOverhead type spin default 50 min 0 max 5000\n"
                      << "uciok" << std::endl;
        } else if (cmd == "isready") {
            std::cout << "readyok" << std::endl;
        } else if (cmd == "setoption") {
            std::string tok, name, value;
            ss >> tok;                                   // "name"
            while (ss >> tok && tok != "value") name += (name.empty() ? "" : " ") + tok;
            while (ss >> tok) value += (value.empty() ? "" : " ") + tok;
            if (name == "BookFile") {
                if (!loadBook(value)) std::cerr << "Warning: could not load book " << value << std::endl;
            } else if (name == "MoveOverhead") {
                moveOverhead = std::max(0, std::atoi(value.c_str()));
            }
        } else if (cmd == "ucinewgame") {
            finishSearch();
            clearTT();
        } else if (cmd == "position") {
            finishSearch();
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
            finishSearch();
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

            // time per move: about 1/30 of the clock plus most of the increment, minus lag reserve
            int ms;
            if (movetime > 0) {
                ms = movetime;
            } else {
                bool white = board.sideToMove() == Color::WHITE;
                int clock = white ? wtime : btime, inc = white ? winc : binc;
                if (clock < 0) ms = (depthGiven || infinite) ? 1000000000 : 3000;   // fixed depth / analysis: no cut-off
                else ms = std::max(20, std::min(std::min(clock / 30 + inc * 8 / 10, 15000), clock / 2) - moveOverhead);
            }

            resetStop();
            worker = std::thread(thinkAndReply, board, depth, ms);
        } else if (cmd == "stop") {
            finishSearch();                       // the worker prints "bestmove" as it unwinds
        } else if (cmd == "quit") {
            break;
        }
    }
    finishSearch();
}