# Prince of Persia: The Two Thrones in the browser

A static recompilation of the PC game: `POP3.EXE` is translated from x86 to C (Ghidra function list +
`translator/x86toc.py`), compiled, and run on a small Windows API layer (`host/`): kernel32/user32, DirectInput 8,
DirectSound, a Bink stub and **Direct3D 9 on WebGPU** (a recorder/executor renderer). It runs natively on macOS and, built
with Emscripten, in the browser.

**No game files or game-derived code live in this repository.** You need your own legitimately obtained copy of the game
(the executable is `POP3.EXE`, not the small `PrinceOfPersia.exe` launcher). Generated C, binaries and the function list are
gitignored.

## Status

Playable through the first level, natively on macOS: menus, loading, the intro, the ship-deck level with sky, sea and
characters, keyboard control (W/A/S/D, Space to jump), about 26 fps. Menus are English (`RC_LANGUAGE`, default `ENGLISH`;
voices stay in the installed language). Audio goes through CoreAudio (3D positioning, soft limiter, latency-correct play
cursor). Not done or not verified: combat and gamepad, the real Bink movies (a fake 2-frame movie is returned), a full game
run in a real browser, and performance (the CPU vertex-shader interpreter is the hot spot). See `plan.md` for the detailed
log, findings and every debug switch (`RC_*`).

## Build and run (macOS)

Toolchain: Python 3 with `pefile` and `capstone` (`python3 -m venv .venv && .venv/bin/pip install pefile capstone`),
Ghidra 12+ (needs JDK 25+) for the function list, SDL2, wgpu-native in `third_party/wgpu`, Emscripten for the browser build.

```
ghidra/export.sh <POP3.EXE>                       # -> work/functions.json (your copy of the game; not committed)
PY=$PWD/.venv/bin/python PROFILE=work/profile.h ./build.sh <game folder>/POP3.EXE work/functions.json
build/recomp <game folder>                        # native: opens the game window
```

`HOST_OPT` sets the optimisation level of `host/` (default `-O2`). The first run mirrors the game's profile and save data
under `userdata/`.

## Browser

```
OPT=-O1 PROFILE=work/profile.h ./build_web.sh     # -> build_web/www and build_web/node
./make_site.sh                                    # -> site/ (index.html, loader.js, build/recomp.{js,wasm}; no game files)
cd site && python3 serve.py                       # http://localhost:8007/  (choose your game folder in the page)
cd site && python3 serve.py --game "<your game folder>"   # local dev: the page copies it itself, no file dialog
```

Open the page and choose your own game folder; the browser copies it into its private storage (OPFS) once and runs
`POP3.EXE`. The server must send the cross-origin-isolation headers (`serve.py` does); threads need SharedArrayBuffer.
Controls: W/A/S/D move, Space jump, Enter select, Alt+Enter fullscreen.

## Debugging

Useful environment switches (full list in `plan.md`): `RC_LOG`, `RC_LOG_FPS`, `RC_SCREENSHOTS="frame,..."`, `RC_CAPTURE=frame`,
`RC_INPUT="200:key=1c,..."` (scripted input by frame), `RC_SDL_PUSH=1` (synthetic key events through the real event path),
`RC_NO_CULL`, `RC_DUMP_DARE`, `RC_LOG_SYNC`, `RC_LANGUAGE`. Keep logs short: `RC_LOG_IMPORTS` can write gigabytes.
