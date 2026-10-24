# Desk display: the board sends one small JSON object, and each of its keys
# becomes a field as it is:
#   {"wifi_dbm": -52}             every 30 seconds; the first after a start is tagged "hello"
#   {"seen": "Dinner is ready"}   when the screen is tapped; tagged "seen"

def decode(payload, meta):
    return json.decode(payload)
