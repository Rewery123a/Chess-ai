#include "book.hpp"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <random>
#include <utility>
#include <vector>

using namespace chess;

namespace {
struct BookEntry { uint64_t key; uint16_t move; uint16_t weight; };
std::vector<BookEntry> book;                 // sorted by key (Polyglot format)
std::mt19937 rng{std::random_device{}()};

uint64_t be(const unsigned char* p, int n) { // big-endian read
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}
}

bool loadBook(const std::string& path) {
    book.clear()
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    book.clear();
    for (size_t i = 0; i + 16 <= data.size(); i += 16)   // 16 bytes per entry
        book.push_back({be(&data[i], 8), (uint16_t)be(&data[i + 8], 2),
                        (uint16_t)be(&data[i + 10], 2)});
    return !book.empty();
}

std::string bookMove(const Board& board) {
    if (book.empty()) return "";
    uint64_t key = board.hash();
    auto it = std::lower_bound(book.begin(), book.end(), key,
        [](const BookEntry& e, uint64_t k) { return e.key < k; });

    Movelist legal;
    movegen::legalmoves(legal, board);

    std::vector<std::pair<std::string, int>> options;
    int total = 0;
    for (; it != book.end() && it->key == key; ++it) {
        int to    = (it->move & 7) + 8 * ((it->move >> 3) & 7);
        int from  = ((it->move >> 6) & 7) + 8 * ((it->move >> 9) & 7);
        int promo = (it->move >> 12) & 7;
        for (const Move& m : legal) {
            if ((int)m.from().index() != from || (int)m.to().index() != to) continue;
            bool isPromo = m.typeOf() == Move::PROMOTION;
            if (isPromo ? (int)m.promotionType() != promo : promo != 0) continue;
            int w = std::max<int>(1, it->weight);
            options.push_back({uci::moveToUci(m), w});
            total += w;
            break;
        }
    }
    if (options.empty()) return "";

    int pick = (int)(rng() % total);             // weighted random choice
    for (auto& o : options) {
        if (pick < o.second) return o.first;
        pick -= o.second;
    }
    return options.back().first;
}