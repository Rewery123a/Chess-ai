import chess

VALUES = {chess.PAWN:1, chess.KNIGHT:3, chess.BISHOP:3, chess.ROOK:5, chess.QUEEN:9, chess.KING:0}

def evaluate(board):
    if board.is_checkmate():
        return -9999 if board.turn else 9999
    score = 0
    for piece in board.piece_map().values():
        v = VALUES[piece.piece_type]
        score += v if piece.color==chess.WHITE else -v
    return score

def minimax(board, depth, alpha, beta):
    if depth == 0 or board.is_game_over():
        return evaluate(board)
    if board.turn == chess.WHITE:
        best = -10000
        for m in board.legal_moves:
            board.push(m)
            best = max(best, minimax(board, depth-1, alpha, beta))
            board.pop()
            alpha=max(alpha,best)
            if beta <= alpha: break
        return best
    best = 10000
    for m in board.legal_moves:
        board.push(m)
        best = min(best, minimax(board, depth-1, alpha, beta))
        board.pop()
        beta = min(beta, best)
        if beta <= alpha: break
    return best


def best_move (board, depth=3):
    white = board.turn == chess.WHITE
    best, best_m = (-10000 if white else 10000), None
    for m in board.legal_moves:
        board.push(m)
        s = minimax(board, depth-1, -10000)
        board.pop()
        if (white and s > best) or (not white and s < best):
            best, best_m = s, m
    return best_m
     
    