#!/usr/bin/env python3
"""Small source patches applied to the translator's output (build.sh runs this after translation).

- FUN_0080dd00 is the game's SetLanguage(index): the stored index picks the text tables (new-profile screen, prompts) and the
  Menu\\<LANGUAGE> folder. This copy of the game is a Spanish edition, so the host overrides the index (default English;
  RC_LANGUAGE=SPANISH|FRENCH|ITALIAN|GERMAN|ENGLISH, see host_language_override in host/core.c).
- The video settings default to 800x600 (a width/height pair of immediate stores: 0x320 then 0x258). The host supplies the size
  instead (RC_RESOLUTION=WxH, default 1920x1080; the game's own resolution list gets 1920x1080 added, see host/kernel32.c).
Usage: patch_generated.py <out dir>
"""
import glob
import re
import sys

OLD = "W32((uint32_t)(0xe3e8c0u), (uint32_t)(r->eax)); /* mov dword ptr [0xe3e8c0], eax */"
NEW = "{ extern uint32_t host_language_override(uint32_t); W32((uint32_t)(0xe3e8c0u), host_language_override((uint32_t)(r->eax))); } /* mov dword ptr [0xe3e8c0], eax (language patched) */"
done = 0
for f in glob.glob(sys.argv[1] + "/*.c"):
    s = open(f).read()
    if OLD in s and "void f_0080dd00(" in s:
        i = s.index("void f_0080dd00(")
        j = s.index(OLD, i)
        if j - i < 600:
            open(f, "w").write(s[:j] + NEW + s[j + len(OLD):])
            done += 1
def pair_re(w, h):
    return re.compile(
        r"(W32\(\(uint32_t\)\([^;\n]*\), \(uint32_t\)\(\(uint32_t\))0x%xu"
        r"(\)\); /\* mov dword ptr \[[^\]\n]*\], 0x%x \*/\n\s*W32\(\(uint32_t\)\([^;\n]*\), \(uint32_t\)\(\(uint32_t\))0x%xu"
        r"(\)\); /\* mov dword ptr \[[^\]\n]*\], 0x%x \*/)" % (w, w, h, h))

# the game's mode table sets [esi]=width, [esi+4]=height per mode: 800x600 (the default) and 1024x768 (the in-game default)
for w, h in ((0x320, 0x258), (0x400, 0x300)):
    PAIR = pair_re(w, h)
    for f in glob.glob(sys.argv[1] + "/*.c"):
        s = open(f).read()
        s2, n = PAIR.subn(lambda m: m.group(1) + "({ extern uint32_t host_video_width(void); host_video_width(); })" + m.group(2)
                          + "({ extern uint32_t host_video_height(void); host_video_height(); })" + m.group(3), s)
        if n:
            open(f, "w").write(s2)
            done += n
print(f"patch_generated: {done} patch(es)")
