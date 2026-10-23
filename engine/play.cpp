#include "play.hpp"
#include "book.hpp"
#include "search.hpp"
#include <iostream>
#include <string>
#include <vector>

using namespace chess;

void play() {
    setSearchInfo(false);
    Board b(constants::STARTPOS);
    std::vector<std::string> hist;
    std::string in;
    std::cout << "Play as white? (y/n): ";
    std::getline(std::cin, in);
    bool humanWhite = !in.empty() && (in[0] == 'y' || in[0] == 'Y');

    while (true) {
        for (int r = 7; r >= 0; --r) {
            for (int f = 0; f < 8; ++f) std::cout << std::string(b.at(Square(r * 8 + f))) << ' ';
            std::cout << '\n';
        }
        Movelist legal;
        movegen::legalmoves(legal, b);
        if (legal.empty()) {
            if (b.inCheck())
                std::cout << "Checkmate! " << (b.sideToMove() == Color::WHITE ? "Black" : "White") << " wins.\n";
            else
                std::cout << "Stalemate, draw.\n";
            return;
        }
        if (b.isInsufficientMaterial() || b.isHalfMoveDraw() || b.isRepetition()) {
            std::cout << "Draw.\n";
            return;
        }

        Move chosen;
        if ((b.sideToMove() == Color::WHITE) == humanWhite) {
            while (true) {
                std::cout << "Your move (e.g. e2e4, e7e8q): ";
                if (!std::getline(std::cin, in)) return;
                bool ok = false;
                for (const Move& m : legal)
                    if (uci::moveToUci(m) == in) { chosen = m; ok = true; break; }
                if (ok) break;
                std::cout << "Illegal move, try again.\n";
            }
        } else {
            std::string bm = bookMove(b);
            bool found = false;
            if (!bm.empty())
                for (const Move& m : legal)
                    if (uci::moveToUci(m) == bm) { chosen = m; found = true; break; }
            if (!found) {
                Board work = b;
                chosen = search(work, 64, 5000);
            }
            std::cout << "AI plays: " << uci::moveToUci(chosen) << "\n";
        }
        hist.push_back(uci::moveToUci(chosen));
        b.makeMove(chosen);
    }
}