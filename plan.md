# Plan: Prince of Persia: The Two Thrones in the browser

Reference project: `../cricket07-browser` (Cricket 07 port; "C07" below). This repo was scaffolded from its
generic layers. Technique: translate the x86 executable to C (Ghidra function list + `translator/x86toc.py`),
compile to WebAssembly, and provide the Windows APIs the game calls in `host/`.

Requirement: your own legitimately obtained copy of the game (`POP3.EXE` is the game, `PrinceOfPersia.exe`
is a launcher). No game files or code are committed. Everything marked BLOCKED needs that copy.

## Status
- [x] Toolchain: Emscripten, SDL2, Ghidra 12.2, wgpu-native, capstone/pefile
- [x] Scaffold: translator, runtime, host layer, web loader copied from C07; profile set for POP3.EXE
- [x] Milestone 1 survey done (game copy at `~/Desktop/Prince of Persia The Two Thrones`; output in `work/survey.txt`, `work/imports_exe.txt`)
- [x] Milestone 2 translate+build: Ghidra found 14,333 functions (needs JDK 25+; `JAVA_HOME=$(brew --prefix openjdk)/libexec/openjdk.jdk/Contents/Home`).
  Translator output: 26,660 functions, 2.1M instructions, 89 files, 297 functions with 5,590 unsupported instructions.
  `build/librecomp.dylib` and `build/recomp` link. Needs a venv: `python3 -m venv .venv && .venv/bin/pip install pefile capstone`,
  then `PY=$PWD/.venv/bin/python PROFILE=work/profile.h ./build.sh <POP3.EXE> work/functions.json`.
- [x] Milestone 3 boots natively (`build/recomp <game folder>`): added 22 kernel32/user32 imports, INI reading, `host/pop3_stubs.c`
  (Bink fails to open, EAX/DInput8 stubs), a pre-existing `POP3Launcher` mutex (`PROFILE_EXISTING_MUTEXES`) and a mirrored
  `userdata/Ubisoft/POP3` folder. The game opens `POPData.BF` and `pop3.bf`, loads the registry/profile, creates its window,
  then calls `Direct3DCreate9` and crashes on the NULL result (call to 0 from 008a8b6a). That is the D3D9 work (next).
  Note: the game writes `POP.LOG`, `POPError.DAT`, `PrinceSM` into the game folder; set `RC_...` or sandbox later (OPFS in the browser).
- [~] Milestone 4 D3D9 layer: `host/d3d9.c` (was d3d8.c) implements IDirect3D9 / IDirect3DDevice9 (119-slot vtable), textures, surfaces,
  buffers, vertex declarations (converted to the D3D8 tokens `vshader.c` reads), vertex/pixel shaders, state blocks, sampler states.
  Run with the WebGPU renderer: the device is created (800x600), caps are accepted, 40 `ps.1.1` shaders and the water vs.1.1 load,
  DirectInput 8 / DirectSound / EAX (alias of DirectSoundCreate) work, `Hardware.INI` (normally written by the launcher) comes from
  `PROFILE_BUILTIN_INIS`, and the engine prints "Init: Complete. Engine: Starting main loop... Loading WOL".
  **Current blocker:** the first level load ("Loading WOL: 0XB015CEA") runs at 100% CPU for over a minute with no Present; the main
  thread is inside `f_00805ad0 -> f_008a0080/f_0089fca0` (allocator / critical-section heavy). Next: find out whether it is an
  endless loop (a wait on a timer/thread/file the host does not satisfy) or just slow; compare with `RC_LOG_IMPORTS=1`.
  Translator/runtime fixes made on the way: entry discovery follows branches inside a function and after code-pointer tables;
  `jump_indirect` now sets `g_import_target` (tail calls into COM methods ran the previous method); per-window longs; `MessageBox` answers No to Yes/No.
  Leftovers to check: 38k unsupported instructions now reported (mostly data decoded as code by the new entries), `StretchRect`
  scaling, DrawIndexedPrimitive base vertex, fixed-function vs shader paths, netplay removal.
  Debug aids: `RC_TRAP_PEEK=addr,...` (words printed at a guest trap), `RC_LOG_JUMPS=1`, `RC_LOG_D3D=2` (every COM call).
  **Update:** the load stall was a thread-timing problem (the main thread's tight lock/unlock loop in the sound engine never let the
  audio-timer / loader threads run). `ReleaseMutex` now yields; mutex waiters are first-in-first-out; message queues are per thread.
  With that the level load finishes and 3 frames are presented, then the game deadlocks: the main thread joins a thread
  (`f_004155a0`, handle 0x10a0) while that thread (`f_0080bce0`) waits on a semaphore handshake (0x108c) nobody releases.
  Tools: `RC_STALL_DUMP=1` (last import per thread every 5 s), `RC_TRACE_RET=addr`, `RC_PEEK=addr,..`, `RC_SLOW_IMPORTS=us [RC_SLOW_MATCH=name]`.
  Run the game from a symlinked copy (`work/gamecopy`, with its own `POP3Profiles/`) so logs and profile files stay out of the original folder.
  **Update 2:** the game now renders its scene with textures at ~12-20 fps (frame 120 shows the burning Babylon rooftops, rugs,
  fire). Fixes: `ExitThread` now marks the thread handle finished (the main thread was joining the loading-screen thread
  forever); `SetFVF(0)` no longer clears the vertex declaration; `dcl` instructions in vs_1_1 are skipped and used to map
  declaration elements to input registers. Screenshot: `RC_SCREENSHOTS=120` -> `/tmp/rc_frame_120.png`.
  Known issues: crash (null `Release` through a stale COM object) right after the game's `Reset` to 1024x768, V8U8 textures
  (format 0x3c) unsupported, parts of the scene missing/black (sky, ground, character), speed.
  **Update 3:** fixed the crash after the first key press (`StretchRect` copied between surfaces of different size and zeroed
  a COM vtable; it now skips mismatched sizes). Scripted input works (`RC_INPUT="150:key=1c,..."`, DIK codes) and the game
  survives 1000+ frames. **Not yet playable:** after the first Enter the game switches 800x600 -> 1024x768 and renders the scene
  into a 800x600 part of the larger target with a mis-scaled foreground (needs `StretchRect` scaling between render targets
  and a consistent back-buffer size); parts of the geometry are missing/black; V8U8 textures unsupported; ~12 fps.
  Debug aids: `RC_POLL_ADDR=addr` (report who changes a guest word), `RC_HEAP_WATCH=addr`.
- [ ] difftest on samples not run yet

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

## Survey findings (Milestone 1)
- `POP3.EXE`: PE32, 2005, MSVC 7.10, image base 0x400000, **no relocations** (loads at original address: matches C07 model),
  not packed (.text entropy 6.5), .text 5.7 MB, .data 4.9 MB virtual (0x2a000 raw), no PDB.
- Imports are small: KERNEL32 (134), USER32 (26), GDI32 (2), ADVAPI32 (4 registry), SHELL32 (ShellExecuteA),
  DINPUT8 (`DirectInput8Create`), DSOUND (ordinal 11 = `DirectSoundCreate8`), WINMM (`timeSetEvent`, `waveOutGetDevCapsA`),
  **d3d9 (`Direct3DCreate9` only)**, EAX.DLL (#6), binkw32 (12 functions).
- **No OpenAL import**: audio is DirectSound (+EAX). Drop the OpenAL item.
- **D3DX is statically linked** (no d3dx9 import). D3DX's own strings (ps_2_0, ps_3_0) are not game shaders.
  The game's shaders are embedded as **text `ps.1.1`** (about 40 variants; `tex/texcoord/lrp/add_sat/mul_x2`).
  So C07's `ps.1.1 -> WGSL` path is reusable. Still to check: vertex shaders (`vs.1.1` text, or fixed function).
  Static D3DX also means texture/mesh/shader-assemble helpers run as translated game code, not host calls.
- `base.dll`, `local.dll`: x86 DLLs with relocations; base.dll imports MSVCP71/MSVCR71 (C++ STL). `local.dll` is
  mostly resources (localisation). These are launcher-framework DLLs (PDB path `POP3Launcher\framework\base`),
  so likely only needed by `PrinceOfPersia.exe`; check whether `POP3.EXE` loads them (`LoadLibraryA` strings).
- Config: `POP3Config.ini` (Mode, Fullscreen, Sound, SpeedMode), `PrinceOfPersia.mcl` launcher manifest.
- Bink 1 API is tiny (12 calls): stubbing is trivial, a real decoder is a bounded job.

## Milestones
1. **Survey (DONE, see findings).** `tools/imports.py` and `tools/pe_survey.py` on `POP3.EXE` and each
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

## Status at end of this session (2026-11-01)
Runs natively with the WebGPU renderer: boots, loads the first level, draws a textured 3D scene (~12 fps), accepts scripted
keyboard input, survives 1000+ frames. Fixed this round: `Reset` recreates the back buffer at the new size, `StretchRect` between
render targets is done as a textured-quad draw, packed vertex types (SHORT4N etc.) are decoded, vertex-type numbering fixed.
**Not playable yet.** Open problems, in order:
1. Large wall/ground areas render black or blocky (lightmap or second-texture layer, or textures uploaded before their data is
   written; check `sync_texture` timing and V8U8 = format 0x3c).
2. No gameplay or menu verified (the first level opens on a cutscene; key presses only tested to not crash).
3. `StretchRect` from the back buffer is done on the CPU (readback), and speed is ~12 fps; the browser build is untested.
4. Bink movies fail to open (skipped); audio plays but is unverified by ear.

### Findings after the last debugging round
- The scene after the level loads is the main-menu background (burning Babylon, camera pan that repeats); scripted keys (Space, Enter,
  Esc) do not change it, and no menu text or buttons are visible.
- Dark/black areas: forcing the diffuse colour (`RC_FORCE_WHITE=1|<vs handle>`) brightens them, so the geometry and textures are right;
  much of the night scene is simply dark (vertex-lit by fire). Pre-transformed (POSITIONT) vertex declarations are now bound as XYZRHW FVFs.
- At frame ~300 a large black rectangle (x 25%-100%, y 0-75% of the window) appears; it is not caused by the `StretchRect` draw
  (`RC_NO_STRETCH_DRAW=1` keeps it). Suspects: a `Clear` with a sub-viewport, or a UI/menu panel whose texture is empty.
- Next: log `Clear` calls (viewport, flags, colour) around frame 300, find out which draw/texture produces the panel, add V8U8 (0x3c),
  verify DirectInput keyboard state reaches the menu code (the pad 0 `GetProperty` errors suggest DI8 property differences),
  then performance.

### Black rectangle (frame ~300) -- what it is
Per-draw captures (`RC_CAPTURE=300`, images in `/tmp/rc_cap/`) show the screen-wide black rectangle is not a UI quad, not a clear
(one full-viewport clear per frame) and not the `StretchRect` round trip (skipped now when same size; `RC_STRETCH_CPU=1` restores
the CPU copy). It is scene geometry: a wall close to the camera whose vertex-lit colour (lighting vertex shaders `1008f`/`10095`,
`m3x3` normal * `dp3` with the light at `c32`) comes out 0, so it renders black. Everything else about those draws (positions,
textures, state) looks right, so the next step is to check the lighting shaders' inputs: normals (`v2`) and the world matrix rows
`c8..c10`, the light constants `c32..c39` and the clamp constants `c20`.

### Latest
- `V8U8` (format 0x3c) textures are now converted (signed U,V shifted to 0..255); speed is ~20 fps natively.
- The sky draws (`100ef`, stream 1 colours `fdxxxxxx` = black with alpha) and some close walls come out black: the vertex colour input is
  black for those meshes and the fixed-function stage then multiplies it in; check which of them are meant to use `TFACTOR`
  (`tss0 a 2,3`) or additive blending, and compare with how the lit draws (`1009b`, colours 0.15-0.3) look.
- DirectInput keyboard/mouse/pad paths exist (`GetDeviceState`, `GetDeviceData`), but no menu is visible yet, so input effects cannot
  be judged. The pad 0 `GetProperty() failed` errors at start-up point at DI8 property differences worth fixing.

### Frame structure found with `RC_LOG_CMDS=1` (important for the next session)
Each frame the game does: clear the back buffer; draw the glow chain into 512x512 render targets (ids 6/8); render the whole scene
(~700 draws) into the 800x600 render-target texture id 4; then composite with `StretchRect` calls between the back buffer (id 1)
and RT texture 4 (sequence: RT4 -> bb, bb -> RT4, RT4 -> bb, all 800x600). Because the executor cannot sample or copy the back
buffer, those copies are currently skipped (same-size) or went through a CPU readback/upload (slow and wrong), which is why the
picture shows only parts of the scene plus a black rectangle (the region the CPU round trip overwrites).
**Fix to implement:** a real GPU copy command (CMD_COPY: texture-to-texture, or a draw that samples the back-buffer texture `g_bb`
with the right logical-to-GPU scale) in `render_wgpu.c`, used by `dev_StretchRect` for every GPU-to-GPU pair; then the post-process
chain works and the frame is complete. Also: BinkOpen now returns a fake 2-frame movie so the logo and loading movies finish.

### Black rectangle = one draw (found)
In the frame-300 capture the black rectangle is draw #458: 12 triangles (a box), `vs 1009b`, no texture (`tss0 op 3` = diffuse only), vertex colours from
stream 1 are `0xfe000000` (black), `cw 7`, z test+write, `cull 2`, drawn near the end of the frame (before the additive 2-triangle
glow quad). All earlier draws leave that screen area with scene content, so removing this box would reveal the scene. Check the
render state around it (stencil: `rs[52..59]`, `rs[168]` colour-write, blend, `rs[26]`/`rs[27]` alpha) -- the game probably draws it
with colour writes off or as a stencil/occlusion helper, and one state we ignore (stencil, `D3DRS_COLORWRITEENABLE1-3`, or the
`CreateQuery` occlusion queries we refuse) is what turns it into a visible black box.
Other facts: GPU sampling of the back buffer now works (`TS_BACKBUF`), so every StretchRect between GPU surfaces is a real draw.

### Exact per-frame pipeline (RC_LOG_CMDS=1, frame ~150)
clear back buffer (bb) -> ~515 scene draws into bb -> StretchRect bb->RT4 (id 4, 800x600) -> RT4->bb -> bb->RT4 -> glow chain
(512x512 targets 6/8) -> ~140 more draws (additive effects, particles, HUD) into RT4 -> final StretchRect RT4->bb -> Present.
The per-draw capture of the bb after the scene draws (`/tmp/rc_cap/NNNN.png`) shows the scene WITHOUT the black rectangle, while the
presented frame has it, so the rectangle is introduced by the StretchRect/RT4 part or by the last ~140 draws, not by the first
515. `RC_SKIP_UNTEXTURED=1` (drops untextured diffuse-only draws) does not remove it, so it is not the 12-triangle black box.
Next: dump RT4 right before the final StretchRect (add a readback screenshot of a given render target) to see whether RT4 or the
final copy is wrong.

### Black rectangle: negative results (do not repeat)
Present with: all `StretchRect`s skipped (`RC_STRETCH_SKIP=3f`), untextured draws dropped (`RC_SKIP_UNTEXTURED=1`), a submit after every
draw (`RC_SUBMIT_EVERY=1`), scratch depth cleared per target (`RC_SCRATCH_CLEAR=1`). Absent in the per-draw capture images
(`RC_CAPTURE=N` writes the back buffer after each draw that targets it; same camera pose as the presented frame). So the draws into the
back buffer are right, and what differs is either the capture path (it reads the back buffer back after every draw, which also
synchronises the queue) or the later draws that target RT4 / the present path. Next experiment: make `exec_present` screenshot RT4
and the back buffer separately, and check `wgpuQueueWriteBuffer` hazards (vertex/uniform ring reused before earlier work ran) by
giving each frame its own buffer region.

### Black rectangle: located (bisect with `RC_MAX_DRAWS=N`, which drops every draw after the Nth of a frame)
With the stretches skipped, the rectangle appears exactly when draw #571 is included: `vs 1007d` (POSITIONT UI/post-process vertex
shader) + pixel shader handle `10059` (ps.1.1), one full-screen quad (n 2), texture slot 0 = a render-target texture (fmt 0x15),
`z 0/1/4`, colour write `f`, target = back buffer. It is the FIRST of the game's post-process passes (the following ones, `10051`,
`10027`, `10037`, `1002b`, all `vs 1007d`). So the black area is the output of a post-process pixel shader, not scene geometry:
either the texture it samples is empty/mis-addressed (a render target that is only filled by the StretchRect round trip), or my
ps.1.1 -> WGSL translation of that program (`host/pshader.c`) or the vertex shader's texture-coordinate mapping is wrong.
Next steps: dump the ps.1.1 bytecode of `10059` (add a `RC_DUMP_PS` like `RC_DUMP_VS`), check which texture stages it reads and
which RT ids they map to, and make the screenshot tool able to dump an arbitrary render target (RT4) to see its content.

### Black rectangle: RT4 itself contains it
`RC_DUMP_RT=4 RC_SCREENSHOTS=300` also writes `/tmp/rc_rt4_300.png` (render target id 4, 800x600): it already has the black rectangle
(x 200-800, y 0-450); the post-process pass `10059` (draw ~571, ps.1.1 `r0.rgb = (c0*2-1) + t0`, c0 = 0.44, i.e. a mild darken) just
copies RT4 to the back buffer. RT4 is filled by my `stretch_by_draw` copies (bb -> RT4) and receives ~140 effect draws, and it keeps
its contents between frames, so a feedback loop (previous frame's RT4 -> bb -> RT4) can sustain a black region once something
paints it. Next: dump RT4 after the first bb->RT4 copy only (stop drawing into RT4 after that copy), and test with the copy done
as `CopyTextureToTexture` (scaled by a draw only when sizes differ) to rule out my quad (`stretch_by_draw`) as the source.

### Post-process chain facts (for the black rectangle; `RC_CAPTURE` + `[psc]`/`[vsout]` logs)
After the ~515 scene draws into the back buffer (bb) the game runs: (1) bb->RT4 copy (my quad, `vs 0x104`), (2) full-screen pass `ps 10059` RT4->bb
(`r0.rgb = (c0*2-1)+t0`, c0 = 0.44 -> slight darken), (3) `ps 10051` RT4 + LUT texture (tex1) -> bb (`lrp t0, LUT(t0.gb), c0.a = 0.15`),
(4) a downsample/blur chain on 512/256/128 render targets with `ps 10027` (bright-pass-like, `v0*t0`) and `ps 10037` (4-tap sum), (5) glow
composite `ps 1002b` (`rgb = t0*v0`, `a = v0.a` = 0.41) alpha-blended into RT4 (blend SRCALPHA/INVSRCALPHA), (6) final RT4->bb.
Measured: bb right before (1) is a complete picture; RT4 at the end of the frame has the black rectangle; including draw #571 (my
bb->RT4 quad) is what makes it appear. Both my GPU quad and the older CPU readback/upload copy produce it, so suspect what the later
RT4 users do with it (steps 5/6, alpha taken from RT4/glow, `ps 10051` LUT sampling with `t0.yz` addressing), not the copy itself.
Try: dump RT4 and bb after each of steps 1-6 (add a `RC_DUMP_AFTER=<draw index>` readback), and review `ps 10051` handling of `texreg2gb`
(`tex t1` with `t0.yz` = D3D9 `texreg2gb`, opcode 0x46 in the token stream above: our WGSL samples tex1 at `t0.yz`, which is right
only if t0 is in 0..1 and the LUT is a 2D texture bound with clamp addressing).

## Level load fixed: gameplay reached (2026-11-01)
- Root cause of the black loading screen: DARE's I/O worker pool resets its job event when the first queued job was cancelled,
  stranding the job behind it (lost wakeup). It only showed because `WaitForMultipleObjects` polled with `usleep(1000)`, making workers
  slower than on Windows. Fix: `WaitForMultipleObjects` waits on a global condition broadcast by SetEvent/ReleaseSemaphore/ReleaseMutex,
  and `SetEvent` yields. Also added IDirectSoundBuffer8 slots (SetFX/AcquireResources/GetObjectInPath), which crashed after loading.
- Result: after profile -> difficulty, the first level loads and renders (~16-18 fps, 350-700 draws): the Prince on the ship deck.
- Debug aids: `RC_DUMP_DARE=1` (pending sound resources + I/O pool state), `RC_LOG_SYNC`, `RC_LOG_READS`, `RC_LOG_DSPOS`; import log has a thread tag.
- Known remaining: sky/background black, ~17 fps, movies fake, gameplay input beyond menus untested, browser build untested.
- Input verified: after the intro cutscene (frames ~1900-2320), scripted W taps (`RC_INPUT`, DIK 0x11) from frame 2400 move the Prince along the
  ledge and the camera follows (screenshots 2390/2500/2620). Menus (Enter) also work. So the first level is controllable.
- Black sky/sea/ground investigation (2026-11-01): `RC_CAPTURE=2400` shows ~350 scene draws, no skipped draws (`[skip]` logs added), only the post passes use pixel
  shaders. `RC_FORCE_WHITE=1` brightens buildings/smoke but sky, sea and some ground stay black, so those surfaces are either not drawn by the
  game at this point or draw black; there is no large far-plane sky draw in the list. Next: compare against the same scene from a real
  D3D9 capture (or a known-good reference image) to learn whether the sea/sky should exist; check the glow/post chain output.
  Scene timing differs per run (cutscene), so MAX_DRAWS bisects need a fixed frame (use `RC_PACE60`).
- Disk: debug logs (esp. `RC_LOG_IMPORTS`) reached GBs and filled the disk; keep them filtered/short.
- Jump verified (DIK 0x39 at frame 2515: the Prince leaps and lands). CoreAudio output starts and the mixer runs (not listened to).
- Browser build compiles: `OPT=-O1 PROFILE=work/profile.h ./build_web.sh` (needed an `execinfo.h` guard in `host/core.c`). The Node build
  (`node build_web/node/recomp.js work/gamecopy`) boots the game and runs for 100 s without a crash (timers + mixer alive). Not yet tried in a real
  browser with WebGPU (`web/serve.py`).
- Combat attempt: Ctrl/click/E/F/Q taps at frames 2460-2580 produced no visible attack (no enemy in reach; later frames show a different camera,
  probably a scripted event). Combat is still untested; it needs a hand-driven session near an enemy (default key bindings are in Gamepads.DAT / profile).

## Black sky/sea/ground FIXED: the cull modes were swapped (2026-11-01)
- `host/render_rec.c` swapped D3DCULL_CW/CCW before handing them to WebGPU (front face CW), so every back-facing surface (sky dome, sea, ground,
  inside faces) was culled. Mapping them straight through restores the full scene: dusk sky, mountains, sea, ground, ships. (`RC_NO_CULL=1` debugging switch kept.)
  The earlier "vertex colours are black" theory was wrong. Speed is now ~14 fps in this scene (more is drawn); optimising is the next job.
- Speed: the CPU vertex-shader interpreter (`vs_run`) was the hot spot and the host code was built at `-O1`. `build.sh` now compiles `host/` with
  `HOST_OPT` (default `-O2`): 14 -> ~26 fps in the first level (`-O3 -ffast-math` gave nothing more). Next: vectorise/cache `vs_run`, or run the vertex
  shaders on the GPU.

## Audio + hosting (2026-11-01)
- Audio "blasting": the DirectSound mixer ignored 3D (positional sounds played at full volume, no distance falloff/pan), used nearest-sample
  resampling and hard clipping. Now: IDirectSound3DBuffer/Listener position, distance, rolloff, orientation drive gain and pan; linear
  interpolation; soft limiter (`host/dsound.c`). Needs an ear test.
- Hosting: `./make_site.sh` builds `site/` (index.html, loader.js, worklet, `build/recomp.{js,wasm}`; no game files); `cd site && python3 serve.py`
  serves it with the COOP/COEP headers threads need. The page is rebranded, defaults to `POP3.EXE`, and shows the verified controls.
  Loaded in Chrome: page renders, no console errors. The game itself needs the player to pick their install folder (copied into OPFS).

## Keyboard focus, English menus, crackling audio (2026-11-01)
- Keyboard: synthetic SDL key events (`RC_SDL_PUSH=1`) drive the menus, so the input path works; a binary started from a terminal is not a regular
  app, so macOS left keyboard focus with the terminal. `window.c` now sets the activation policy to regular, activates the app and takes
  input focus at start and once more after launch. A `keyboard: first key received` line prints when the first key arrives. (If keys still do
  nothing, click the game window once.)
- Language: this install is a Spanish edition and the game takes its language from its own data; `map_path` now maps `Menu\<LANGUAGE>\...` to
  `RC_LANGUAGE` (default ENGLISH), so the menus and text are English. Voices come from `Sound/soundlocal.big` (still the installed language).
- Crackling: the DirectSound play cursor ran ahead of what the speakers play (the output queue was not accounted for), so the game refilled
  streaming buffers over sound not yet heard. The reported cursor now lags by the queued frames (`g_out_latency`; in the browser, the ring
  fill), and CoreAudio uses 4 x 1024-frame buffers instead of 3 x 512 (fewer underruns).
