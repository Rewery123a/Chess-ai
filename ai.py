import chess
import random
import time
import chess.ployglot

VALUES = {chess.PAWN:1, chess.KNIGHT:3, chess.BISHOP:3, chess.ROOK:5, chess.QUEEN:9, chess.KING:0}
PIECE_VALUE = {chess.PAWN:100, chess.KNIGHT:320, chess.BISHOP:330,
chess.ROOK:500, chess.QUEEN:900, chess.KING:0}

PAWN_PST = [
0, 0, 0, 0, 0, 0, 0, 0,
50, 50, 50, 50, 50, 50, 50, 50,
10, 10, 20, 30, 30, 20, 10, 10,
5, 5, 10, 25, 25, 10, 5, 5,
0, 0, 0, 20, 20, 0, 0, 0,
5, -5,-10, 0, 0,-10, -5, 5,
5, 10, 10,-20,-20, 10, 10, 5,
0, 0, 0, 0, 0, 0, 0, 0]

KNIGHT_PST = [
-50,-40,-30,-30,-30,-30,-40,-50,
-40,-20, 0, 0, 0, 0,-20,-40,
-30, 0, 10, 15, 15, 10, 0,-30,
-30, 5, 15, 20, 20, 15, 5,-30,
-30, 0, 15, 20, 20, 15, 0,-30,
-30, 5, 10, 15, 15, 10, 5,-30,
-40,-20, 0, 5, 5, 0,-20,-40,
-50,-40,-30,-30,-30,-30,-40,-50]

BISHOP_PST = [
-20,-10,-10,-10,-10,-10,-10,-20,
-10, 0, 0, 0, 0, 0, 0,-10,
-10, 0, 5, 10, 10, 5, 0,-10,
-10, 5, 5, 10, 10, 5, 5,-10,
-10, 0, 10, 10, 10, 10, 0,-10,
-10, 10, 10, 10, 10, 10, 10,-10,
-10, 5, 0, 0, 0, 0, 5,-10,
-20,-10,-10,-10,-10,-10,-10,-20]

ROOK_PST = [
0, 0, 0, 0, 0, 0, 0, 0,
5, 10, 10, 10, 10, 10, 10, 5,
-5, 0, 0, 0, 0, 0, 0, -5,
-5, 0, 0, 0, 0, 0, 0, -5,
-5, 0, 0, 0, 0, 0, 0, -5,
-5, 0, 0, 0, 0, 0, 0, -5,
-5, 0, 0, 0, 0, 0, 0, -5,
0, 0, 0, 5, 5, 0, 0, 0]

QUEEN_PST = [
-20,-10,-10, -5, -5,-10,-10,-20,
-10, 0, 0, 0, 0, 0, 0,-10,
-10, 0, 5, 5, 5, 5, 0,-10,
-5, 0, 5, 5, 5, 5, 0, -5,
0, 0, 5, 5, 5, 5, 0, -5,
-10, 5, 5, 5, 5, 5, 0,-10,
-10, 0, 5, 0, 0, 0, 0,-10,
-20,-10,-10, -5, -5,-10,-10,-20]

KING_MID_PST = [
-30,-40,-40,-50,-50,-40,-40,-30,
-30,-40,-40,-50,-50,-40,-40,-30,
-30,-40,-40,-50,-50,-40,-40,-30,
-30,-40,-40,-50,-50,-40,-40,-30,
-20,-30,-30,-40,-40,-30,-30,-20,
-10,-20,-20,-20,-20,-20,-20,-10,
20, 20, 0, 0, 0, 0, 20, 20,
20, 30, 10, 0, 0, 10, 30, 20]

KING_END_PST = [
-50,-40,-30,-20,-20,-30,-40,-50,
-30,-20,-10, 0, 0,-10,-20,-30,
-30,-10, 20, 30, 30, 20,-10,-30,
-30,-10, 30, 40, 40, 30,-10,-30,
-30,-10, 30, 40, 40, 30,-10,-30,
-30,-10, 20, 30, 30, 20,-10,-30,
-30,-30, 0, 0, 0, 0,-30,-30,
-50,-30,-30,-30,-30,-30,-30,-50]

PST = {chess.PAWN:PAWN_PST, chess.KNIGHT:KNIGHT_PST, chess.BISHOP:BISHOP_PST,
chess.ROOK:ROOK_PST, chess.QUEEN:QUEEN_PST}

MATE = 100000
INF = 10**9
EXACT, LOWER, UPPER = 0, 1, 2

TT = {}
_deadline = 0
_nodes = 0

class Timeout(Exception):
    pass

def check_time():
    global _nodes
    _nodes += 1
    if _nodes & 2047 == 0 and time.time() > _deadline:
        raise Timeout

def eval_side(board):
    """evaluate() is White-positive; search needs side-to-move."""
    s = evaluate(board)
    return s if board.turn == chess.WHITE else -s

def order_moves(board, moves, first = None):
    def score(m):
        if m == first:
            return 10000
        s = 0
        if board.is_capture(m):
            victim = board.piece_type_at(m.to_square) or chess.PAWN
            attacker = board.piece_type_at(m.from_square)
            s +=100 + 10 * VALUES[victim] - VALUES[attacker]
        if m.promotion:
            s += 80
        return s
    return sorted(moves, key=score, reverse=True)

#Quiescence search
def quiesce(board, alpha, beta):
    check_time()
    stand = eval_side(board)
    if stand >= beta:
        return beta
    alpha = max(alpha, stand)
    for m in order_moves(board, board.generate_legal_captures()):
        board.push(m)
        score = -quiesce(board, -beta, -alpha)
        board.pop()
        if score >= beta:
            return beta
        alpha = max(alpha, score)
    return alpha

def negamax (board, depth, alpha, beta, ply):
    check_time()

    # Use fastest mate on board
    if board.is_checkmate():
        return -(MATE - ply)
    if (board.is_stalemate() or board.is_insufficient_material() or board.is_fifty_moves() or board.is_repetition(2)):
        return 0
    if depth <=0:
        return quiesce(board, alpha, beta)

    # Transposition table lookup
    key = chess.polyglot.zobrist_hash(board)
    alpha_orig = alpha
    ttm_move = None
    entry = TT.get(key)
    if entry:
        e_depth, e_score, e_flag, tt_move = entry
        if e_depth >= depth:
            if e_flag == EXACT:
                return e_score
            if e_flag == LOWER:
                alpha = max(alpha, escore)
            else:
                beta = min (beta, e_score)
            if alpha >= beta:
                return e_score
    
    best, best_m = -INF, None
    for m in order_moves(board, board.legalmoves, tt_move):
        board.push(m)
        score = -negamax(board, depth - 1, -beta, -alpha, ply + 1)
        board.pop(m)
        if score > best:
            best, best_m = score, m
        alpha = max(alpha, score)
        if alpha >= beta:
            break

    flag = EXACT
    if best <= alpha_orig:
        flag = UPPER
    elif best >= beta:
        flag = LOWER
    TT[key] = (depth, best, flag, best_m)
    return best

# Iterative deepening defined by time limit
def best_move(board, depth=8, time_limit=5):
    global _deadline, _nodes

    book = book_move(board)
    if book:
        return book

    if len(TT) > 1_000_000:
        TT.clear()
    _deadline = time.time() + time_limit
    _nodes = 0
    base = len(board.move_stack)
    moves = list(board.legal_moves)
    best = None

    for d in range(1, depth + 1):
        try:
            alpha, cur_best, cur_score = -INF, None, -INF
            for m in order_moves(board, moves, best):
                board.push(m)
                score = -negamax(board, d - 1, -INF, -alpha, 1)
                board.pop()
                if score > cur_score:
                    cur_score, cur_best = score, m
                alpha = max(alpha, score)
            best = cur_best
            print(f"depth {d}, eval {cur_score}")
            if cur_score > MATE - 100: #forced mate found
                break 
        except Timeout:
            while len(board.move_stack) > base:
                board.pop()
            break

    return best or order_moves(board,moves)[0]


OPENING_LINES = [
"e4 e5 Nf3 Nc6 Bb5 a6 Ba4 Nf6 O-O Be7", # Ruy Lopez
"e4 e5 Nf3 Nc6 Bc4 Bc5 c3 Nf6 d3", # Italian
"e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 a6", # Sicilian Najdorf
"e4 e6 d4 d5 Nc3 Nf6 Bg5 Be7", # French
"e4 c6 d4 d5 Nc3 dxe4 Nxe4 Bf5", # Caro-Kann
"d4 d5 c4 e6 Nc3 Nf6 Bg5 Be7 e3 O-O Nf3", # Queen's Gambit Declined
"d4 Nf6 c4 g6 Nc3 Bg7 e4 d6 Nf3 O-O Be2", # King's Indian
"d4 Nf6 c4 e6 Nc3 Bb4 e3 O-O Bd3 d5 Nf3", # Nimzo-Indian
"c4 e5 Nc3 Nf6 Nf3 Nc6 g3 d5 cxd5 Nxd5", # English
"Nf3 d5 g3 Nf6 Bg2 e6 O-O Be7 d3", # Reti
]

def _parse(line):
    b = chess.Board()
    return [b.push_san(san) for san in line.split()]

BOOK = [_parse(l) for l in OPENING_LINES]

def book_move (board):
    n = len(board.move_stack)
    options = []
    for line in BOOK:
        if len(line) > n and line[:n] == board.move_stack:
            options.append(line[n])
        return random.choice(options) if options else None

def best_move (board, depth=3):
    book = book_move(board)
    if book:
        return book
    white = board.turn == chess.WHITE
    best, best_m = (-10000 if white else 10000), None
    for m in order(board):
        board.push(m)
        s = minimax(board, depth-1, -10000, 10000)
        board.pop()
        if (white and s > best) or (not white and s < best):
            best, best_m = s, m
    return best_m
     
