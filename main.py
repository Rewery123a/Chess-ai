import chess
from ai import best_move

def get_human_move(board):
    while True:
        text = input("Your move : ").strip()
        try:
            return board.parse_san(text)
        except ValueError:
            print("Illegal move, try again.")

def print_result(board):
    if board.is_checkmate():
        winner = "Black" if board.turn == chess.WHITE else "White"
        print(f"Checkmate! {winner} wins.")
    elif board.is_stalemate():
        print("Stalemate, it's a draw")
    else:
        print ("Draw.")


def main():
    board = chess.Board()
    human_is_white = input("Play as white (y/n) : ").lower()

    while not board.is_game_over():
        print("\n" + str(board) + "\n")

        if board.turn == human_is_white:
            move = get_human_move(board)
        else:
            print("Thinking....")
            move = best_move(board, depth = 3)
            print(f"I play {board.san(move)}")

        board.push(move)

    print("\n" + str(board))
    print_result(board)

if __name__ == "__main__":
    main()