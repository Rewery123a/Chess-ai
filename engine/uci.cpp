#include "uci.hpp"
#include "book.hpp"
#include "search.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using namespace chess;

static const char* BENCH_FENS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "2r3k1/pp3ppp/8/3P4/8/P4P2/1P4PP/2R3K1 b - - 0 1",
    "8/5pk1/6p1/8/3P4/6P1/5PK1/8 w - - 0 1",
    "r1b2rk1/2q1bppp/p2p1n2/np2p3/3PP3/1BN2N1P/PPB2PP1/R1BQR1K1 w - - 0 1",
    "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1",
    "3r2k1/p4ppp/1p6/8/2P5/1P3P2/P5PP/3R2K1 w - - 0 1"};

void runBench(int depth) {
    setSearchInfo(false);
    uint64_t total = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (const char* fen : BENCH_FENS) {
        clearTT();                                   // every position starts from a clean slate
        Board b(fen);
        search(b, depth, 1000000000);
        total += searchNodes();
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "Nodes searched: " << total << "\nTime: " << (long long)ms << " ms\nNPS: "
              << (long long)(total / (ms / 1000.0)) << std::endl;
    setSearchInfo(true);
}

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
            
        } else if (cmd == "bench") {
            int d = 0;
            ss >> d;
            if (d <= 0) d = 9;
            finishSearch();
            runBench(d); 
               
        } else if (cmd == "stop") {
            finishSearch();                       // the worker prints "bestmove" as it unwinds
        } else if (cmd == "quit") {
            break;
        }
    }
    finishSearch();
}