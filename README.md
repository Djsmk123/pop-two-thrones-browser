# Prince of Persia: The Two Thrones in the browser (scaffold)

Work in progress, started from the generic parts of the cricket07-browser project: x86-to-C translator,
runtime, Windows API layer and WebGPU renderer. No game files or game code live here; you need your own
legitimately obtained copy of the game (the executable is `POP3.EXE`, not the small `PrinceOfPersia.exe` launcher).

Open work: the Windows layer in `host/` implements Direct3D 8; Two Thrones uses Direct3D 9, Bink video,
EAX/DirectSound, so `host/d3d8.*` and the shader paths need a D3D9 port. First step once you have a clean
copy: `python3 tools/imports.py <POP3.EXE>` to list the imports and size that gap.

Toolchain (installed): Emscripten, SDL2, Ghidra 12.2 (`~/ghidra/ghidra_12.2_PUBLIC`), wgpu-native in
`third_party/wgpu`, Python `capstone` and `pefile`. Build steps are as in the Cricket 07 README
(`ghidra/export.sh`, `build.sh`, `build_web.sh`, `web/serve.py`), with `PROFILE=work/profile.h`.
