# Scope this routine to the spec both boards use. A message tagged "button"
# from one board queues "led:blink" for the other one.

# Your two boards' device ids, from each board's device page. Until both are
# set, the routine stops with an error instead of sending anything.
BOARD_A = "put your first board's id here"
BOARD_B = "put your second board's id here"

def routine(event, state, mem):
    if " " in BOARD_A or " " in BOARD_B:
        fail("pair.star: set BOARD_A and BOARD_B to your two device ids")
    if event["tag"] != "button":
        return
    if event["hw_id"] == BOARD_A:
        send(BOARD_B, b"led:blink")
    elif event["hw_id"] == BOARD_B:
        send(BOARD_A, b"led:blink")
