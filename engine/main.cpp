#include <iostream>
#include <string>
#include "book.hpp"
#include "play.hpp"
#include "uci.hpp"

int main(int argc, char** argv) {
    if (!loadBook("Perfect2023.bin"))
        std::cerr << "Warning: Perfect2023.bin not found, playing without a book\n";
    if (argc > 1 && std::string(argv[1]) == "play") play();
    else uciLoop();
    return 0;
}