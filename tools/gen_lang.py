#!/usr/bin/env python3
"""host/lang_en.txt (Section.Key=English text) -> host/lang_en.h. Run after editing the text file."""
import os
here = os.path.dirname(os.path.abspath(__file__))
rows = [l.rstrip("\n") for l in open(os.path.join(here, "..", "host", "lang_en.txt")) if "=" in l]
out = ["/* generated from lang_en.txt by tools/gen_lang.py: English text for the dialogs and menu strings of a Spanish edition's POPData.bf */\n", "#define LANG_EN_TABLE \\\n"]
for r in rows:
    k, v = r.split("=", 1)
    out.append('    { "%s", "%s" }, \\\n' % (k, v.replace('"', '\\"')))
out.append("    { 0, 0 }\n")
open(os.path.join(here, "..", "host", "lang_en.h"), "w").write("".join(out))
