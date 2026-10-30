# Plan: Prince of Persia: The Two Thrones in the browser

Reference project: `../cricket07-browser` (Cricket 07 port; "C07" below). This repo was scaffolded from its
generic layers. Technique: translate the x86 executable to C (Ghidra function list + `translator/x86toc.py`),
compile to WebAssembly, and provide the Windows APIs the game calls in `host/`.

Requirement: your own legitimately obtained copy of the game (`POP3.EXE` is the game, `PrinceOfPersia.exe`
is a launcher). No game files or code are committed. Everything marked BLOCKED needs that copy.

## Status
- [x] Toolchain: Emscripten, SDL2, Ghidra 12.2, wgpu-native, capstone/pefile
- [x] Scaffold: translator, runtime, host layer, web loader copied from C07; profile set for POP3.EXE
- [ ] Everything below

## Reused from C07 as-is (verify, don't rewrite)
| Area | C07 files | Notes |
|---|---|---|
| Translator | `translator/x86toc.py`, `tests/difftest.py` | Same 32-bit x86; check for SSE/3DNow! opcodes the game uses |
| Runtime / memory model | `runtime/*`, README "Technical notes" | Original-address load, real stack, 1.25 GB heap window |
| Threads, timers, files | `host/kernel32.c`, `host/sched.c`, `host/core.c` | Add APIs as the import survey shows |
| Window/input | `host/user32.c`, `host/input.c`, `host/dinput.c` | Keyboard, mouse, gamepad (`Gamepads.DAT`) |
| Web shell | `web/`, `build_web.sh` | OPFS game files, picker, COOP/COEP server |

## Needs new work (Two Thrones differs from C07)
1. **Direct3D 9 layer.** C07 is D3D8 (`host/d3d8.c`, `d3d8.h`). Port to D3D9 (`IDirect3D9` / `IDirect3DDevice9`,
   vertex declarations, `SetSamplerState`, render-target and format differences). Keep the command-recording
   design (`render_rec.c` -> `render_wgpu.c`) since WebGPU is page-thread only.
2. **Shaders.** C07 handles `vs.1.1` on the CPU (`host/vshader.c`) and `ps.1.1` -> WGSL (`host/pshader.c`).
   Check which shader models the game uses (`directx9tests.dll` hints at ps/vs 2.0). If 2.x, add a
   translator from the D3D9 bytecode to WGSL and run vertex shaders on the GPU, not the CPU.
3. **Video: Bink.** `BinkW32.DLL` plays `Video/*.bik`. Options: stub and skip movies (first milestone),
   then decode Bink in WASM (open decoder such as FFmpeg's) and feed frames to a texture.
4. **Audio: DirectSound + EAX + OpenAL.** C07 has `host/dsound.c` and an AudioWorklet. EAX/reverb: stub.
   Check `oalinst.exe` presence: the game may use OpenAL, in which case add an OpenAL layer over Web Audio.
5. **Game-specific DLLs.** `base.dll`, `local.dll`, `detectionapi.dll`, `directx9tests.dll`, `EAX.DLL`,
   MFC71/msvcr71/msvcp71. Decide per DLL: translate (like the exe) or stub. Needs multi-module support in the
   translator and loader; C07 only has one module.
6. **Data archives.** `pop3.bf` (~500 MB) and `POPData.bf`. Fits OPFS; C07's `tools/ea_assets.py` is the
   model for an asset checker. Streamed reads through the file layer.
7. **Copy protection / launcher.** Check `detectionapi.dll`, registry reads and the launcher handoff; satisfy
   through `PROFILE_REGISTRY` in `host/profile.h` like C07.
8. **Drop netplay.** The game is single player. Keep `host/sched.c` (deterministic scheduler) only if useful;
   remove `host/netplay.c` and the netplay hooks in `core.c`, `input.c`, `d3d8.c`, `web/`.

## Milestones
1. **Survey (BLOCKED on a clean copy).** `tools/imports.py` and `tools/pe_survey.py` on `POP3.EXE` and each
   DLL: imports, shader models, D3D9 calls, threads, SSE use. Output: gap list with sizes.
2. **Translate.** `ghidra/export.sh` -> `work/functions.json` (C07 used `RecoverRttiClasses.java`,
   `NameFromAsserts.java`; reuse if the exe has RTTI/asserts). Run `build.sh`; run `difftest.py` on samples.
3. **Boots native.** `build/recomp <game folder>` reaches the first window with a stubbed renderer.
4. **D3D9 on native WebGPU.** Menus render, then 3D scene; fix shaders in order of failures.
5. **Browser build.** `build_web.sh`, `web/serve.py`; OPFS copy, memory window, stack sizes as in C07.
6. **Audio, video, input.** In that order of playability value.
7. **Polish.** Save games (`POP3Profiles` into OPFS), performance, Firefox/Safari checks.

## Risks
- D3D9 + SM2.x shaders are a much larger surface than C07's D3D8 + SM1.1; expect most of the effort here.
- Multi-DLL translation may need translator and loader changes.
- 4 GB WASM limit: game memory use must fit with the 500 MB archive streamed, not loaded.
- Threads: Two Thrones may rely on real thread timing; C07's deterministic scheduler may help or hurt.

## Legal
Code here is MIT like C07 and contains no Ubisoft code or assets. Users supply their own copy. Prince of
Persia and Ubisoft are trademarks of Ubisoft, which is not involved.
