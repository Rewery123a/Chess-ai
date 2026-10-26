#pragma once

// Talks to GUIs / lichess-bot over stdin and stdout using the UCI protocol.
void uciLoop();


// Fixed-depth search over a fixed set of positions; prints nodes, time and speed.
void runBench(int depth);