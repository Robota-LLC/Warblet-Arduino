# Paste this Starlark decoder into the device spec. sendTenths() sends the
# value times ten as a signed 16-bit number, high byte first: 00 c9 is 20.1.
# It is the same two-byte format the public temperature demos send.

def decode(payload, meta):
    t = int(payload[0]) << 8 | int(payload[1])
    if t >= 0x8000:          # sign-extend: the board sends int16, not uint16
        t -= 0x10000
    return {"value": t / 10.0}
