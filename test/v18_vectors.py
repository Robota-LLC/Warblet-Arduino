"""Write the CHIRP-PROV v1.8 vectors the library is tested against as plain
lines the C++ tests read, from test/chirp-prov-v1.8.json, the shared vector
file (a byte copy of the one the Warblet demos and the setup page are
tested against; their checks compare the two).

    python test/v18_vectors.py test/chirp-prov-v1.8.json OUT_DIR

OUT_DIR/v18_pushes.txt  stored<TAB>push<TAB>answer<TAB>stores<TAB>after, the pushes of
                        the "token" profile (the library's: it takes a key
                        over USB and sends at Token and Signed)
OUT_DIR/v18_caps.txt    name<TAB>caps, the valid capability objects
"""
import json
import os
import sys

doc = json.load(open(sys.argv[1], encoding="utf-8"))
out = sys.argv[2]
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "v18_pushes.txt"), "w", encoding="utf-8", newline="\n") as f:
    for v in doc["pushes"]:
        if v["profile"] != "token":
            continue
        stored = json.dumps(v["stored"], separators=(",", ":"), ensure_ascii=False)
        # after: the settings held afterwards, in the order settingsToJson() writes them
        after = json.dumps(v["after"], separators=(",", ":"), ensure_ascii=False)
        # A raw NUL cannot ride in a text line: <NUL> stands for it, and the
        # test puts the byte back.
        push = v["push"].replace("\x00", "<NUL>")
        f.write("\t".join([stored, push, v["answer"], "1" if v["stores"] else "0", after]) + "\n")
with open(os.path.join(out, "v18_caps.txt"), "w", encoding="utf-8", newline="\n") as f:
    for v in doc["caps"]["valid"]:
        f.write(v["name"].replace("\t", " ") + "\t" + v["caps"] + "\n")
print("v18 vectors:", sum(1 for v in doc["pushes"] if v["profile"] == "token"), "pushes,",
      len(doc["caps"]["valid"]), "capability objects")
