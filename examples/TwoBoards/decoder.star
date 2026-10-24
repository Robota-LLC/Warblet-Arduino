# Paste this Starlark decoder into the device spec. sendNumber() sends the
# number as text ("21.53", "101325", "-5.0"); this reads it back.

def decode(payload, meta):
    return {"value": float(str(payload))}
