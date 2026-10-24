"""Write test/prov_vectors.txt: what the public demos' chirp_prov.py decides.

chirp_prov.py (Warblet-IoT v1.0.2, esp32-generic/, shared byte for byte by the
four MicroPython demos) is imported read-only and run on CPython, with
stand-ins for the MicroPython-only modules it imports (machine, select, and
time.sleep_ms). For each case, its apply_config_push() answers a push against
a stored config; the answer and, on ok, the config it saves are recorded.

The library must agree, except where test/README.md says it is stricter on
purpose. Those cases are marked "stricter" here, from the documented rules
alone, and the test asserts the library refuses them with bad-json.

Usage: python test/prov_oracle.py <Warblet-IoT v1.0.2 clone> [out]
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import time
import types

ref = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "prov_vectors.txt")
cfg_dir = tempfile.mkdtemp()


class Reset(Exception):
    pass


def _reset():
    raise Reset()


machine = types.ModuleType("machine")
machine.reset = _reset
select = types.ModuleType("select")
select.POLLIN = 1


class _Poll:
    def register(self, *args):
        pass

    def poll(self, *args):
        return []


select.poll = _Poll
sys.modules["machine"] = machine
sys.modules["select"] = select
time.sleep_ms = lambda ms: None
time.ticks_ms = lambda: int(time.monotonic() * 1000)
time.ticks_add = lambda a, b: a + b
time.ticks_diff = lambda a, b: a - b

sys.path.insert(0, os.path.join(ref, "esp32-generic"))
import chirp_prov  # noqa: E402  the demo's module, unmodified

KEY = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="
LIMITS = {"ssid": 32, "pass": 64, "token": 128, "claim": 128, "hwid": 128, "host": 128}


def demo(stored, push):
    """chirp_prov.apply_config_push(push, stored): (answer, saved config or None)."""
    chirp_prov.CFG_PATH = os.path.join(cfg_dir, "chirp_cfg.json")
    if os.path.exists(chirp_prov.CFG_PATH):
        os.remove(chirp_prov.CFG_PATH)
    printed = io.StringIO()
    with contextlib.redirect_stdout(printed):
        try:
            chirp_prov.apply_config_push(push, dict(stored))
        except Reset:
            pass
    answer = printed.getvalue().strip().splitlines()[-1]
    saved = None
    if answer == "CHIRP= ok":
        with open(chirp_prov.CFG_PATH) as f:
            saved = json.load(f)
    return answer, saved


def canonical(cfg):
    aps = None
    if "aps" in cfg:
        aps = [[e.get("ssid") or "", e.get("pass") or ""] for e in cfg["aps"]]
    return json.dumps([cfg.get("ssid") or "", cfg.get("pass") or "", cfg.get("token") or "",
                       cfg.get("hwid") or "", cfg.get("host") or "", cfg.get("key") or "", aps],
                      separators=(",", ":"), ensure_ascii=False)


def strict_json(text):
    def refuse(name):
        raise ValueError(name)
    try:
        json.loads(text, parse_constant=refuse)
        return True
    except ValueError:
        return False


def stricter(push_text):
    """True when the documented stricter rules refuse a push the demo may accept."""
    if not strict_json(push_text):
        return True  # NaN and Infinity are not JSON
    push = json.loads(push_text)
    if not isinstance(push, dict):
        return False
    strings = [(k, v) for k, v in push.items() if k in LIMITS and isinstance(v, str)]
    for entry in push.get("aps") or []:
        if isinstance(entry, dict):
            strings += [("ssid", entry.get("ssid")), ("pass", entry.get("pass"))]
    for name, value in strings:
        if not isinstance(value, str):
            continue
        if len(value.encode("utf-8")) > LIMITS[name]:
            return True
        if any(ord(c) < 0x20 or ord(c) == 0x7F for c in value):
            return True
    key = push.get("key")
    if isinstance(key, str) and key:
        import base64
        import binascii
        try:
            ok = len(key) == 44 and key.endswith("=") and len(base64.b64decode(key, validate=True)) == 32
        except (binascii.Error, ValueError):
            ok = False
        if not ok:
            return True
    return False


provisioned = {"ssid": "workshop", "pass": "hunter2", "token": "TOK-ONE", "hwid": "board-1"}
with_aps = dict(provisioned, aps=[{"ssid": "workshop", "pass": "hunter2"}, {"ssid": "van", "pass": ""}])
claimed = {"ssid": "s", "token": "CHIRP-OLD", "hwid": "h"}
empty = {}

cases = [
    (empty, '{"ssid":"workshop","pass":"hunter2","claim":"CHIRP-ABCDEFGH","hwid":"sunny-otter-7c2f"}'),
    (empty, '{"ssid":"s","pass":"","token":"T","hwid":"h","level":2,"thread":{"name":"n"},"unknown":{"deep":[1,{"a":"}"},3]}}'),
    (empty, '{"ssid":"caf\\u00e9 \\"guest\\"","pass":"p","token":"T","hwid":"h"}'),
    (empty, '{"ssid":"s","token":"T","hwid":"h","key":"' + KEY + '"}'),
    (empty, '{"ssid":"s","token":"T","hwid":"h","key":""}'),
    (empty, '{"ssid":"s","token":"T","hwid":"h"'),
    (empty, '[1,2,3]'),
    (empty, '{"ssid":"s",}'),
    (empty, "{'ssid':'s'}"),
    (empty, '{"ssid":"s","claim":"C","token":"T","hwid":"h"}'),
    (empty, '{"ssid":5,"token":"T","hwid":"h"}'),
    (empty, '{"ssid":"s","claim":123,"hwid":"h"}'),
    (empty, '{"ssid":"s","token":"T","hwid":"two words"}'),
    (empty, '{"ssid":"s","token":"T","hwid":"a/b"}'),
    (empty, '{"ssid":"s","token":"T","hwid":7}'),
    (empty, '{"ssid":"s","token":"T","hwid":""}'),
    (empty, '{"ssid":"s","token":"T"}'),
    (empty, '{"host":"h"}'),
    (empty, '{"ssid":"s","pass":"p","hwid":"h"}'),
    (empty, '{"key":"' + KEY + '"}'),
    (empty, '{"ssid":"s","token":"T","hwid":"h","key":42}'),
    (empty, '{"ssid":"s","token":"T","hwid":"h","host":"my-core.example.com"}'),
    (empty, '{"ssid":"old","pass":"p","aps":[{"ssid":"old","pass":"p"},{"ssid":"van","pass":""},{"ssid":"phone"}],"token":"T","hwid":"h"}'),
    (empty, '{"token":"T","hwid":"h","ssid":"a","aps":[{"pass":"x"}]}'),
    (empty, '{"token":"T","hwid":"h","ssid":"a","aps":["a"]}'),
    (empty, '{"token":"T","hwid":"h","ssid":"a","aps":{}}'),
    (empty, '{"token":"T","hwid":"h","ssid":"a","aps":[{"ssid":"a","pass":5}]}'),
    (empty, '{"token":"T","hwid":"h","ssid":"a","aps":[' + ",".join('{"ssid":"n%d"}' % i for i in range(9)) + ']}'),
    (empty, '{"token":"T","hwid":"h","ssid":"a","aps":[]}'),
    (provisioned, '{"key":"' + KEY + '"}'),
    (provisioned, '{"token":"TOK-TWO"}'),
    (provisioned, '{"claim":"CHIRP-NEWCODE"}'),
    (provisioned, '{"ssid":"new","pass":"n"}'),
    (provisioned, '{"hwid":""}'),
    (provisioned, '{}'),
    (with_aps, '{"ssid":"new","pass":"n"}'),
    (with_aps, '{"token":"T2"}'),
    (with_aps, '{"aps":[{"ssid":"x","pass":"y"}]}'),
    (claimed, '{"token":"TOK-NEW"}'),
    # The documented stricter cases.
    (empty, '{"ssid":"' + "s" * 33 + '","token":"T","hwid":"h"}'),
    (empty, '{"ssid":"s","pass":"' + "p" * 65 + '","token":"T","hwid":"h"}'),
    (empty, '{"ssid":"s","token":"' + "t" * 129 + '","hwid":"h"}'),
    (empty, '{"ssid":"s","token":"T","hwid":"h","key":"c2hvcnQ="}'),
    (empty, '{"ssid":"s","token":"T","hwid":"h","key":"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"}'),
    (empty, '{"ssid":"s","token":"T","hwid":"h","host":"evil.example\\r\\nX-Injected: 1"}'),
    (empty, '{"ssid":"s","token":"T\\r\\nX-Injected: 1","hwid":"h"}'),
    (empty, '{"ssid":"s\\t","token":"T","hwid":"h"}'),
    (empty, '{"ssid":"s","token":"T","hwid":"h","level":NaN}'),
    # Fields exactly at the limits are accepted by both.
    (empty, '{"ssid":"' + "s" * 32 + '","pass":"' + "p" * 64 + '","token":"' + "t" * 128 + '","hwid":"' + "h" * 128 + '"}'),
]

lines = ["# generated by test/prov_oracle.py from the demos' chirp_prov.py; do not edit",
         "# stored<TAB>push<TAB>demo answer<TAB>demo saved config (canonical) or -<TAB>same|stricter"]
counts = {"same": 0, "stricter": 0}
for stored, push in cases:
    answer, saved = demo(stored, push)
    kind = "stricter" if stricter(push) else "same"
    counts[kind] += 1
    lines.append("\t".join([json.dumps(stored, separators=(",", ":")), push, answer,
                            canonical(saved) if saved is not None else "-", kind]))
    assert "\t" not in push and "\n" not in push

with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print("prov oracle: wrote {} cases to {} ({} where the library must match chirp_prov.py, "
      "{} documented stricter)".format(len(cases), out, counts["same"], counts["stricter"]))
