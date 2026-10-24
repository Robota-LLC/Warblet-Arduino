"""Run each example's decoder.star on what that example sent in the simulation.

  python test/run_decoders.py EXAMPLE_DIR RECORD_FILE

RECORD_FILE comes from test/build/example_run (record): one line per message,
"tag<TAB>payload hex<TAB>value the decoder must give". The decoder is run with
CPython, given a payload that behaves as the platform's sandbox
(go.starlark.net) treats bytes in the two ways these decoders use:
str(payload) is its text, and int(payload[0]) is the value of one byte (the
contract's sandbox note: "int() additionally accepts a 1-byte bytes value").
This catches a decoder that does not fit what its example sends; it does not
run the platform's interpreter.
"""
import sys
from pathlib import Path


class OneByte(bytes):
    def __int__(self):
        return bytes.__getitem__(self, 0)


class Payload(bytes):
    def __str__(self):
        return self.decode("utf-8", "replace")

    def __getitem__(self, i):
        v = bytes.__getitem__(self, i)
        return Payload(v) if isinstance(i, slice) else OneByte(bytes([v]))


def main(example_dir, record_file):
    source = (Path(example_dir) / "decoder.star").read_text(encoding="utf-8")
    scope = {}
    exec(compile(source, "decoder.star", "exec"), scope)
    decode = scope["decode"]
    lines = [l for l in Path(record_file).read_text(encoding="utf-8").splitlines() if l.strip()]
    failures = 0
    for n, line in enumerate(lines, 1):
        tag, payload_hex, want = line.split("\t")
        payload = Payload(bytes.fromhex(payload_hex))
        try:
            got = decode(payload, {"tag": tag})
            value = got.get("value") if isinstance(got, dict) else None
            ok = isinstance(value, (int, float)) and abs(value - float(want)) < 1e-6
            detail = "decoded {!r}".format(got)
        except Exception as err:  # a decoder that raises stores the message raw, with an error
            ok, detail = False, "raised {}: {}".format(type(err).__name__, err)
        print("{}  {}: message {} ({} bytes) gives {}: {}".format(
            "ok  " if ok else "FAIL", Path(example_dir).name, n, len(payload), want, detail))
        failures += not ok
    if not lines:
        print("FAIL  {}: nothing was recorded".format(Path(example_dir).name))
        failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
