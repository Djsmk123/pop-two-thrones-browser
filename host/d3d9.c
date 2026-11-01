/* Direct3D 9: IDirect3D9, IDirect3DDevice9 and resources, with complete state tracking.
 * Drawing goes to g_render (headless by default). */
#include "d3d9.h"
#include "render.h"

#include <math.h>
#include <pthread.h>
#include <unistd.h>

#define W(name) static void name(CPU *c)
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTFOUND 0x88760866u

RenderBackend *g_render = &g_render_headless;
uint8_t g_palettes[1024][1024];   /* PALETTEENTRY r, g, b, flags (alpha) x 256 per palette */

/* ------------------------------------------------------------------ formats */

int format_is_dxt(uint32_t f) { return f >= FMT_DXT1 && f <= FMT_DXT5; }

uint32_t format_bpp(uint32_t f) {
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_D32: case FMT_D24S8: case FMT_D24X8: case FMT_INDEX32: return 32;
    case FMT_R8G8B8: return 24;
    case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5: case FMT_A4R4G4B4: case FMT_A8L8: case FMT_V8U8:
    case FMT_D16: case FMT_INDEX16: return 16;
    case FMT_A8: case FMT_P8: case FMT_L8: return 8;
    case FMT_DXT1: return 4;
    case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5: return 8;
    default: return 32;
    }
}

static uint32_t level_pitch(uint32_t f, uint32_t w) {
    if (format_is_dxt(f)) return ((w + 3) / 4) * (f == FMT_DXT1 ? 8 : 16);
    return (w * format_bpp(f) / 8 + 3) & ~3u;
}

static uint32_t level_rows(uint32_t f, uint32_t h) { return format_is_dxt(f) ? (h + 3) / 4 : h; }

static int supported_format(uint32_t f) {
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5:
    case FMT_A4R4G4B4: case FMT_A8: case FMT_L8: case FMT_A8L8: case FMT_P8: case FMT_V8U8:
    case FMT_DXT1: case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5:
    case FMT_D16: case FMT_D24S8: case FMT_D24X8: case FMT_D32: return 1;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ interfaces (defined below) */

static ComInterface IDirect3D9_i, IDirect3DDevice9_i, IDirect3DTexture9_i, IDirect3DCubeTexture9_i,
    IDirect3DSurface9_i, IDirect3DVertexBuffer9_i, IDirect3DIndexBuffer9_i, IDirect3DSwapChain9_i,
    IDirect3DVolumeTexture9_i, IDirect3DVertexDeclaration9_i, IDirect3DVertexShader9_i,
    IDirect3DPixelShader9_i, IDirect3DStateBlock9_i;

static uint32_t g_d3d_object;
static uint32_t g_device_object;  /* the most recently created device: what resources report as their device */
Device *g_device;                 /* ...and its state */

/* ------------------------------------------------------------------ IDirect3D9 */

static const struct { uint32_t w, h; } g_modes[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 960 }, { 1280, 1024 },
    { 1600, 1200 },
};
#define NMODES (sizeof g_modes / sizeof g_modes[0])

W(d3d_GetAdapterCount) { RET(1, 1); }
W(d3d_RegisterSoftwareDevice) { RET(2, E_FAIL); }

W(d3d_GetAdapterIdentifier) {   /* D3DADAPTER_IDENTIFIER9: Driver[512] Description[512] DeviceName[32] ... */
    uint32_t p = ARG(3);
    memset(HP(p), 0, 1100);
    strcpy((char *)HP(p), "nv4_disp.dll");
    strcpy((char *)HP(p + 512), "NVIDIA GeForce 6800 GT");
    strcpy((char *)HP(p + 1024), "\\\\.\\DISPLAY1");
    W64(p + 1056, 0x0006000E000A1700ull);  /* driver version 6.14.10.5888 */
    W32(p + 1064, 0x10DE);                 /* vendor: NVIDIA */
    W32(p + 1068, 0x0045);                 /* GeForce 6800 GT: games of this era match vendor/device IDs against their own lists */
    W32(p + 1072, 0);
    W32(p + 1076, 0xA2);
    W32(p + 1096, 1);                      /* WHQL */
    RET(4, S_OK);
}

static int mode_format_ok(uint32_t f) { return f == FMT_X8R8G8B8 || f == FMT_R5G6B5 || f == FMT_A8R8G8B8 || f == 0; }
W(d3d_GetAdapterModeCount) { RET(3, NMODES); }
W(d3d_EnumAdapterModes) {   /* (adapter, format, mode, out) */
    uint32_t i = ARG(3), p = ARG(4);
    if (i >= NMODES) RET(5, D3DERR_INVALIDCALL);
    W32(p, g_modes[i].w); W32(p + 4, g_modes[i].h); W32(p + 8, 60); W32(p + 12, ARG(2) ? ARG(2) : FMT_X8R8G8B8);
    RET(5, S_OK);
}
W(d3d_GetAdapterDisplayMode) {
    uint32_t p = ARG(2);
    W32(p, (uint32_t)g_screen_w); W32(p + 4, (uint32_t)g_screen_h); W32(p + 8, 60); W32(p + 12, FMT_X8R8G8B8);
    RET(3, S_OK);
}
W(d3d_CheckDeviceType) { RET(6, mode_format_ok(ARG(3)) && mode_format_ok(ARG(4)) ? S_OK : D3DERR_NOTAVAILABLE); }
W(d3d_CheckDeviceFormat) {   /* (adapter, devtype, adapterfmt, usage, rtype, checkfmt) */
    uint32_t r = supported_format(ARG(6)) ? S_OK : D3DERR_NOTAVAILABLE;
    if (g_log_d3d) fprintf(stderr, "[d3d] CheckDeviceFormat(usage %x, rtype %u, fmt %x) = %s\n", ARG(4), ARG(5),
                           ARG(6), r ? "no" : "ok");
    RET(7, r);
}
W(d3d_CheckDeviceMultiSampleType) {   /* (adapter, devtype, fmt, windowed, mstype, *quality) */
    if (ARG(6)) W32(ARG(6), 1);
    RET(7, ARG(5) <= 1 ? S_OK : D3DERR_NOTAVAILABLE);
}
W(d3d_CheckDepthStencilMatch) { RET(6, S_OK); }
W(d3d_CheckDeviceFormatConversion) { RET(5, S_OK); }

static void fill_caps(uint32_t p) {
    memset(HP(p), 0, 304);
    W32(p + 0, 1);                         /* D3DDEVTYPE_HAL */
    W32(p + 8, 0x00020000);                /* Caps: READ_SCANLINE */
    W32(p + 12, 0x20000000 | 0x00080000 | 0x00020000);  /* DYNAMICTEXTURES | CANRENDERWINDOWED | FULLSCREENGAMMA */
    W32(p + 16, 0x20);                     /* Caps3 */
    W32(p + 20, 0x80000001u);              /* presentation intervals: IMMEDIATE | ONE */
    W32(p + 24, 1);                        /* CursorCaps */
    W32(p + 28, 0x001BBEF0);               /* DevCaps: HW T&L, HW raster, pure device, ... */
    W32(p + 32, 0x0000FFF2);               /* PrimitiveMiscCaps */
    W32(p + 36, 0x00732191);               /* RasterCaps: dither, ztest, fog, mip LOD bias, aniso, w/z fog */
    W32(p + 40, 0xFF); W32(p + 44, 0x1FFF); W32(p + 48, 0x1FFF); W32(p + 52, 0xFF);
    W32(p + 56, 0x00084208);               /* ShadeCaps */
    W32(p + 60, 0x0001EC45);               /* TextureCaps (no POW2/SQUAREONLY restrictions) */
    W32(p + 64, 0x07030700); W32(p + 68, 0x07030700); W32(p + 72, 0x07030700);
    W32(p + 76, 0x3F); W32(p + 80, 0x3F); W32(p + 84, 0x1F);
    W32(p + 88, 4096); W32(p + 92, 4096); W32(p + 96, 256); W32(p + 100, 8192); W32(p + 104, 4096);
    W32(p + 108, 16);
    WF32(p + 112, 1e10f);
    WF32(p + 116, -8192.f); WF32(p + 120, -8192.f); WF32(p + 124, 8192.f); WF32(p + 128, 8192.f);
    W32(p + 136, 0xFF);                    /* StencilCaps */
    W32(p + 140, 8 | 0x00100000);          /* FVFCaps: 8 texcoord sets, PSIZE */
    W32(p + 144, 0x03FFFFFF);              /* TextureOpCaps */
    W32(p + 148, 8); W32(p + 152, 4);      /* blend stages, simultaneous textures */
    W32(p + 156, 0x7B);                    /* VertexProcessingCaps */
    W32(p + 160, 8); W32(p + 164, 6); W32(p + 168, 4); W32(p + 172, 0);
    WF32(p + 176, 64.f);
    W32(p + 180, 0xFFFFF); W32(p + 184, 0xFFFFF); W32(p + 188, 16); W32(p + 192, 255);
    W32(p + 196, 0xFFFE0101);              /* vs_1_1 */
    W32(p + 200, 96);                      /* MaxVertexShaderConst */
    W32(p + 204, 0xFFFF0101);              /* ps_1_1 */
    WF32(p + 208, 1.f);                    /* PixelShader1xMaxValue */
    W32(p + 236, 0x1FF);                   /* DeclTypes */
    W32(p + 240, 1);                       /* NumSimultaneousRTs */
    W32(p + 244, 0x300);                   /* StretchRectFilterCaps */
}
W(d3d_GetDeviceCaps) { fill_caps(ARG(3)); RET(4, S_OK); }
W(d3d_GetAdapterMonitor) { RET(2, 0x00070001u); }

static uint32_t new_surface(uint32_t fmt, uint32_t w, uint32_t h, int rt, int depth, int with_mem);
W(d3d_CreateDevice) {
    uint32_t pp = ARG(5), out = ARG(6);
    Device *d = calloc(1, sizeof *d);
    d->width = R32(pp) ? R32(pp) : (uint32_t)g_screen_w;
    d->height = R32(pp + 4) ? R32(pp + 4) : (uint32_t)g_screen_h;
    d->backbuffer_format = R32(pp + 8) ? R32(pp + 8) : FMT_X8R8G8B8;
    d->windowed = R32(pp + 32);
    d->depth_format = R32(pp + 36) ? R32(pp + 40) : 0;
    d->hwnd = R32(pp + 28) ? R32(pp + 28) : ARG(3);
    d->d3d = THIS;
    fprintf(stderr, "[d3d] CreateDevice: %ux%u fmt %u, %s, backbuffers %u, multisample %u, swap %u, depth %u, "
            "behavior %x, flags %x\n", d->width, d->height, d->backbuffer_format, d->windowed ? "windowed" : "fullscreen",
            R32(pp + 12), R32(pp + 16), R32(pp + 24), d->depth_format, ARG(4), R32(pp + 44));
    if (R32(pp) == 0) { W32(pp, d->width); W32(pp + 4, d->height); }
    d->backbuffer = new_surface(d->backbuffer_format, d->width, d->height, 1, 0, 0);
    if (d->depth_format) d->depth = new_surface(d->depth_format, d->width, d->height, 0, 1, 0);
    d->st.render_target = d->backbuffer;
    d->st.depth_stencil = d->depth;
    /* Direct3D 9's documented default states (D3DRENDERSTATETYPE / D3DTEXTURESTAGESTATETYPE numbering) */
    uint32_t *rs = d->st.rs;
    rs[7] = d->depth ? 1 : 0;            /* ZENABLE */
    rs[8] = 3;                           /* FILLMODE: solid */
    rs[9] = 2;                           /* SHADEMODE: gouraud */
    rs[14] = 1;                          /* ZWRITEENABLE */
    rs[19] = 2; rs[20] = 1;              /* SRCBLEND one, DESTBLEND zero */
    rs[22] = 3;                          /* CULLMODE: ccw */
    rs[23] = 4;                          /* ZFUNC: lessequal */
    rs[25] = 8;                          /* ALPHAFUNC: always */
    rs[53] = rs[54] = rs[55] = 1;        /* STENCILFAIL / ZFAIL / PASS: keep */
    rs[56] = 8;                          /* STENCILFUNC: always */
    rs[58] = rs[59] = 0xFFFFFFFFu;       /* STENCILMASK / STENCILWRITEMASK */
    rs[60] = 0xFFFFFFFFu;                /* TEXTUREFACTOR */
    rs[136] = 1;                         /* CLIPPING */
    rs[137] = 1;                         /* LIGHTING */
    rs[141] = 1;                         /* COLORVERTEX */
    rs[145] = 1; rs[146] = 2;            /* DIFFUSE/SPECULARMATERIALSOURCE: color1 / color2 */
    rs[154] = 0x3F800000;                /* POINTSIZE 1.0 */
    rs[155] = 0x3F800000;                /* POINTSIZE_MIN 1.0 */
    rs[161] = 1;                         /* MULTISAMPLEANTIALIAS */
    rs[162] = 0xFFFFFFFFu;               /* MULTISAMPLEMASK */
    rs[168] = 0xF;                       /* COLORWRITEENABLE */
    rs[171] = 1;                         /* BLENDOP: add */
    for (int s = 0; s < 8; s++) {
        uint32_t *t = d->st.tss[s];
        t[1] = s == 0 ? 4 : 1;           /* COLOROP: modulate / disable */
        t[2] = 2; t[3] = 1;              /* COLORARG1 texture, COLORARG2 current */
        t[4] = s == 0 ? 2 : 1;           /* ALPHAOP: selectarg1 / disable */
        t[5] = 2; t[6] = 1;              /* ALPHAARG1 texture, ALPHAARG2 current */
        t[11] = (uint32_t)s;             /* TEXCOORDINDEX */
        t[13] = t[14] = t[25] = 1;       /* ADDRESSU/V/W: wrap */
        t[16] = t[17] = 1; t[18] = 0;    /* MAG/MINFILTER point, MIPFILTER none */
        t[26] = 1;                       /* COLORARG0 current */
    }
    for (int i = 0; i < MAX_TS; i++)
        for (int k = 0; k < 4; k++) d->st.transform[i][k * 5] = 1.f;
    d->st.viewport.w = d->width; d->st.viewport.h = d->height; d->st.viewport.maxz = 1.f;
    if (g_render->init) g_render->init(d);
    g_device_object = com_new(&IDirect3DDevice9_i, RT_DEVICE, d);
    g_device = d;
    W32(out, g_device_object);
    RET(7, S_OK);
}

static const ComMethod d3d_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "RegisterSoftwareDevice", 1, d3d_RegisterSoftwareDevice }, { "GetAdapterCount", 0, d3d_GetAdapterCount },
    { "GetAdapterIdentifier", 3, d3d_GetAdapterIdentifier }, { "GetAdapterModeCount", 2, d3d_GetAdapterModeCount },
    { "EnumAdapterModes", 4, d3d_EnumAdapterModes }, { "GetAdapterDisplayMode", 2, d3d_GetAdapterDisplayMode },
    { "CheckDeviceType", 5, d3d_CheckDeviceType }, { "CheckDeviceFormat", 6, d3d_CheckDeviceFormat },
    { "CheckDeviceMultiSampleType", 6, d3d_CheckDeviceMultiSampleType },
    { "CheckDepthStencilMatch", 5, d3d_CheckDepthStencilMatch },
    { "CheckDeviceFormatConversion", 4, d3d_CheckDeviceFormatConversion }, { "GetDeviceCaps", 3, d3d_GetDeviceCaps },
    { "GetAdapterMonitor", 1, d3d_GetAdapterMonitor }, { "CreateDevice", 6, d3d_CreateDevice },
};
static ComInterface IDirect3D9_i = { "IDirect3D9", d3d_methods, 17, 0 };

void w_Direct3DCreate9(CPU *c) {
    fprintf(stderr, "[d3d] Direct3DCreate9(%u) -> headless Direct3D 9 (%s backend)\n", ARG(0), g_render->name);
    g_d3d_object = com_new(&IDirect3D9_i, RT_D3D, NULL);
    RET(1, g_d3d_object);
}

/* ------------------------------------------------------------------ resources */

static Device *dev(CPU *c) { return com_data(THIS); }

uint32_t gpu_new_id(void) {
    static uint32_t next = 1;
    return __atomic_fetch_add(&next, 1, __ATOMIC_SEQ_CST);
}

static uint32_t new_surface(uint32_t fmt, uint32_t w, uint32_t h, int rt, int depth, int with_mem) {
    Surface *s = calloc(1, sizeof *s);
    s->gpu_id = gpu_new_id();
    s->format = fmt; s->width = w; s->height = h;
    s->pitch = level_pitch(fmt, w);
    s->is_rt = rt; s->is_depth = depth;
    if (with_mem || rt || depth) s->mem = heap_alloc(s->pitch * level_rows(fmt, h) + 16, 1);
    return com_new(&IDirect3DSurface9_i, RT_SURFACE, s);
}

static uint32_t create_texture(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt,
                               uint32_t pool, int cube) {
    Texture *t = calloc(1, sizeof *t);
    t->gpu_id = gpu_new_id();
    t->sent_version = 0xFFFFFFFFu;
    t->type = cube ? RT_CUBETEXTURE : RT_TEXTURE;
    t->format = fmt; t->usage = usage; t->pool = pool; t->width = w; t->height = h;
    t->faces = cube ? 6 : 1;
    uint32_t maxlev = 1;
    for (uint32_t x = w > h ? w : h; x > 1; x >>= 1) maxlev++;
    t->levels = (levels == 0 || levels > maxlev) ? maxlev : levels;
    for (uint32_t l = 0; l < t->levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        t->lw[l] = lw; t->lh[l] = lh;
        t->pitch[l] = level_pitch(fmt, lw);
        t->size[l] = t->pitch[l] * level_rows(fmt, lh);
        for (uint32_t f = 0; f < t->faces; f++) t->mem[f][l] = heap_alloc(t->size[l] + 16, 1);
    }
    uint32_t g = com_new(cube ? &IDirect3DCubeTexture9_i : &IDirect3DTexture9_i, t->type, t);
    if (g_log_d3d) fprintf(stderr, "[d3d] Create%sTexture %ux%u levels %u fmt %x usage %x pool %u -> %08x\n",
                           cube ? "Cube" : "", w, h, t->levels, fmt, usage, pool, g);
    return g;
}

/* ------------------------------------------------------------------ IDirect3DDevice9 */

W(dev_TestCooperativeLevel) { RET(1, S_OK); }
W(dev_GetAvailableTextureMem) { RET(1, 256u << 20); }
W(dev_EvictManagedResources) { RET(1, S_OK); }
W(dev_GetDirect3D) { W32(ARG(1), dev(c)->d3d); RET(2, S_OK); }
W(dev_GetDeviceCaps) { fill_caps(ARG(1)); RET(2, S_OK); }
W(dev_GetDisplayMode) {
    Device *d = dev(c);
    uint32_t p = ARG(2);   /* (swap chain, out) */
    W32(p, d->width); W32(p + 4, d->height); W32(p + 8, 60); W32(p + 12, d->backbuffer_format);
    RET(3, S_OK);
}
W(dev_GetCreationParameters) {
    uint32_t p = ARG(1);
    W32(p, 0); W32(p + 4, 1); W32(p + 8, dev(c)->hwnd); W32(p + 12, 0x40);
    RET(2, S_OK);
}
W(dev_Reset) {
    Device *d = dev(c);
    uint32_t pp = ARG(1);
    fprintf(stderr, "[d3d] Reset to %ux%u\n", R32(pp), R32(pp + 4));
    d->lost = 0;
    uint32_t nw = R32(pp) ? R32(pp) : d->width, nh = R32(pp + 4) ? R32(pp + 4) : d->height;
    d->backbuffer_format = R32(pp + 8) ? R32(pp + 8) : d->backbuffer_format;
    if (nw != d->width || nh != d->height) {   /* a new back buffer (and depth buffer) of the new size */
        uint32_t old_bb = d->backbuffer, old_ds = d->depth;
        d->width = nw; d->height = nh;
        d->backbuffer = new_surface(d->backbuffer_format, nw, nh, 1, 0, 0);
        if (old_ds) d->depth = new_surface(d->depth_format, nw, nh, 0, 1, 0);
        if (d->st.render_target == old_bb) d->st.render_target = d->backbuffer;
        if (d->st.depth_stencil == old_ds) d->st.depth_stencil = d->depth;
        d->st.viewport.x = d->st.viewport.y = 0; d->st.viewport.w = nw; d->st.viewport.h = nh;
    }
    RET(2, S_OK);
}
/* Determinism checks: RC_HASH_LOG=<file> writes a hash of the game's global data (the executable's
   writable sections, g_state_lo-g_state_hi) every frame; RC_SNAPSHOT=<frame>,... writes that memory raw to
   <RC_SNAPSHOT_DIR or /tmp>/rc_state_<frame>.bin, to diff two runs at their first differing frame. */
static uint64_t state_hash_globals(void) {
    uint64_t h = 1469598103934665603ull;   /* FNV-1a over 8-byte words */
    for (uint32_t a = g_state_lo; a + 8 <= g_state_hi; a += 8) { h ^= R64(a); h *= 1099511628211ull; }
    return h;
}
static void state_hash_tick(uint32_t frame) {
    static FILE *log = (FILE *)1;
    static const char *snap = (const char *)1;
    if (log == (FILE *)1) { const char *p = getenv("RC_HASH_LOG"); log = p ? fopen(p, "w") : NULL; }
    if (snap == (const char *)1) snap = getenv("RC_SNAPSHOT");
    if (log) {
        uint64_t h = state_hash_globals();
        extern uint32_t g_det_sleeps, g_det_waits, g_det_reads;
        fprintf(log, "%u %016llx sleeps=%u waits=%u reads=%u t=%.6f\n", frame, (unsigned long long)h, g_det_sleeps,
                g_det_waits, g_det_reads, guest_peek_seconds());
        /* RC_HASH_ALL=N: every N frames also the heap and VirtualAlloc'd memory (thread stacks, game pools) */
        static int every = -1;
        if (every < 0) every = getenv("RC_HASH_ALL") ? atoi(getenv("RC_HASH_ALL")) : 0;
        if (every > 0 && frame % (uint32_t)every == 0) {
            extern void guest_mem_ranges(uint32_t r[4]);
            uint32_t r[4];
            guest_mem_ranges(r);
            uint64_t hh = 1469598103934665603ull, hv = hh;
            for (uint32_t a = r[0]; a + 8 <= r[1]; a += 8) { hh ^= R64(a); hh *= 1099511628211ull; }
            for (uint32_t a = r[2]; a + 8 <= r[3]; a += 8) { hv ^= R64(a); hv *= 1099511628211ull; }
            fprintf(log, "%u heap %016llx %u MB valloc %016llx %u MB\n", frame, (unsigned long long)hh, (r[1] - r[0]) >> 20,
                    (unsigned long long)hv, (r[3] - r[2]) >> 20);
        }
        fflush(log);
    }
    if (snap) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%u", frame);
        for (const char *p = strstr(snap, tmp); p; p = strstr(p + 1, tmp)) {
            size_t n = strlen(tmp);
            if ((p != snap && p[-1] != ',') || (p[n] && p[n] != ',')) continue;
            char path[512];
            snprintf(path, sizeof path, "%s/rc_state_%u.bin", getenv("RC_SNAPSHOT_DIR") ? getenv("RC_SNAPSHOT_DIR") : "/tmp", frame);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(HP(g_state_lo), 1, g_state_hi - g_state_lo, f); fclose(f); }
            if (getenv("RC_SNAPSHOT_HEAP")) {   /* also the heap, as rc_heap_<frame>.bin */
                extern void guest_mem_ranges(uint32_t r[4]);
                uint32_t r[4];
                guest_mem_ranges(r);
                snprintf(path, sizeof path, "%s/rc_heap_%u.bin", getenv("RC_SNAPSHOT_DIR") ? getenv("RC_SNAPSHOT_DIR") : "/tmp", frame);
                if ((f = fopen(path, "wb"))) { fwrite(HP(r[0]), 1, r[1] - r[0], f); fclose(f); }
            }
            break;
        }
    }
}

W(dev_Present) {
    Device *d = dev(c);
    d->frame++;
    extern void input_script_tick(uint32_t frame);
    extern void dump_tick(uint32_t frame);
    input_script_tick(d->frame);
    dump_tick(d->frame);
    extern void netplay_frame(uint32_t frame, uint64_t (*state_hash)(void));
    netplay_frame(d->frame, state_hash_globals);   /* lockstep: exchange and apply pad inputs */
    vclock_frame();
    state_hash_tick(d->frame);
    static int exit_frame = -1;   /* RC_EXIT_FRAME=N: quit after frame N (benchmarks, determinism checks) */
    if (exit_frame < 0) exit_frame = getenv("RC_EXIT_FRAME") ? atoi(getenv("RC_EXIT_FRAME")) : 0;
    if (exit_frame && d->frame >= (uint32_t)exit_frame) {
        fprintf(stderr, "[exit] frame %u after %.2f s\n", d->frame, host_seconds() - 60.0);
        exit(0);
    }
    static double last_t;
    static uint32_t last_f;
    static int fps_log = -1;
    if (fps_log < 0) fps_log = getenv("RC_LOG_FPS") != NULL;
    double now = host_seconds();
    if (fps_log && now - last_t >= 2.0) {
        if (last_t) fprintf(stderr, "[fps] %.1f  (frame %u, %u draws in the last frame)\n",
                            (d->frame - last_f) / (now - last_t), d->frame, d->draws_this_frame);
        extern void guest_mem_stats(uint32_t *, uint32_t *);
        uint32_t hu, vu;
        guest_mem_stats(&hu, &vu);
        extern size_t g_gpu_tex_bytes;
        extern uint32_t g_gpu_bind_groups_made;
        fprintf(stderr, "[mem] guest heap %u MB, virtual allocations %u MB, GPU textures %zu MB, bind groups made %u\n",
                hu >> 20, vu >> 20, g_gpu_tex_bytes >> 20, g_gpu_bind_groups_made);
        last_t = now; last_f = d->frame;
    }
    if (g_render->present) g_render->present(d);
    if (g_log_d3d && (d->frame <= 3 || d->frame % 300 == 0))
        fprintf(stderr, "[d3d] Present: frame %u, %u draws\n", d->frame, d->draws_this_frame);
    d->draws_this_frame = 0;
    RET(5, S_OK);
}
W(dev_GetBackBuffer) {   /* (swap chain, index, type, out) */
    Device *d = dev(c);
    ComObj *o = com_get(d->backbuffer);
    if (o) o->refs++;
    W32(ARG(4), d->backbuffer);
    RET(5, S_OK);
}
W(dev_GetRasterStatus) { W32(ARG(2), 1); W32(ARG(2) + 4, 0); RET(3, S_OK); }
W(dev_SetDialogBoxMode) { RET(2, S_OK); }
W(dev_SetGammaRamp) { RET(4, S_OK); }
W(dev_GetGammaRamp) {
    for (int i = 0; i < 256; i++) for (int k = 0; k < 3; k++) W16(ARG(2) + 512u * k + 2u * i, (uint16_t)(i * 257));
    RET(3, S_OK);
}
W(dev_GetSwapChain) { RET(3, D3DERR_INVALIDCALL); }
W(dev_GetNumberOfSwapChains) { RET(1, 1); }
W(dev_CreateTexture) {   /* (w, h, levels, usage, fmt, pool, out, shared) */
    W32(ARG(7), create_texture(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 0));
    RET(9, S_OK);
}
W(dev_CreateCubeTexture) {   /* (edge, levels, usage, fmt, pool, out, shared) */
    W32(ARG(6), create_texture(ARG(1), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 1));
    RET(8, S_OK);
}
W(dev_CreateVolumeTexture) {
    fprintf(stderr, "[d3d] CreateVolumeTexture is not supported\n");
    RET(10, D3DERR_NOTAVAILABLE);
}
static uint32_t create_buffer(int index, uint32_t len, uint32_t usage, uint32_t fvf_fmt, uint32_t pool) {
    Buffer *b = calloc(1, sizeof *b);
    b->index = index; b->size = len; b->usage = usage; b->fvf_or_format = fvf_fmt; b->pool = pool;
    b->mem = heap_alloc(len + 16, 1);
    return com_new(index ? &IDirect3DIndexBuffer9_i : &IDirect3DVertexBuffer9_i,
                   index ? RT_INDEXBUFFER : RT_VERTEXBUFFER, b);
}
W(dev_CreateVertexBuffer) { W32(ARG(5), create_buffer(0, ARG(1), ARG(2), ARG(3), ARG(4))); RET(7, S_OK); }
W(dev_CreateIndexBuffer) { W32(ARG(5), create_buffer(1, ARG(1), ARG(2), ARG(3), ARG(4))); RET(7, S_OK); }
W(dev_CreateRenderTarget) {   /* (w, h, fmt, ms, msq, lockable, out, shared) */
    W32(ARG(7), new_surface(ARG(3), ARG(1), ARG(2), 1, 0, 1)); RET(9, S_OK);
}
W(dev_CreateDepthStencilSurface) { W32(ARG(7), new_surface(ARG(3), ARG(1), ARG(2), 0, 1, 0)); RET(9, S_OK); }
W(dev_CreateOffscreenPlainSurface) { W32(ARG(5), new_surface(ARG(3), ARG(1), ARG(2), 0, 0, 1)); RET(7, S_OK); }
W(dev_CreateAdditionalSwapChain) { RET(3, D3DERR_NOTAVAILABLE); }

static void touch_container(Surface *s) {
    Texture *t = s->container ? com_data(s->container) : NULL;
    if (t) t->version++;
}
static int is_gpu_surface(Device *d, Surface *s) { return s && (s->is_rt || com_data(d->backbuffer) == s); }

/* rects: n source RECTs (or NULL = whole surface), pts: n destination POINTs (or NULL = same position) */
static void copy_rects(Device *dv, Surface *src, Surface *dst, uint32_t n, uint32_t rects, uint32_t pts) {
    if (is_gpu_surface(dv, src) && g_render->readback) g_render->readback(dv, src);   /* GPU contents -> memory */
    if (dst && !dst->mem) dst->mem = heap_alloc(dst->pitch * level_rows(dst->format, dst->height) + 16, 1);
    if (!src || !dst || !src->mem || !dst->mem ||
        (src->format != dst->format && format_bpp(src->format) != format_bpp(dst->format)))
        return;
    uint32_t bpp = format_bpp(src->format);
    int dxt = format_is_dxt(src->format);
    for (uint32_t i = 0; i < (n ? n : 1); i++) {
        uint32_t x0 = 0, y0 = 0, x1 = src->width, y1 = src->height, dx = 0, dy = 0;
        if (rects) { uint32_t r = rects + 16 * i; x0 = R32(r); y0 = R32(r + 4); x1 = R32(r + 8); y1 = R32(r + 12); }
        if (pts) { dx = R32(pts + 8 * i); dy = R32(pts + 8 * i + 4); } else { dx = x0; dy = y0; }
        if (dxt) { x0 /= 4; x1 = (x1 + 3) / 4; y0 /= 4; y1 = (y1 + 3) / 4; dx /= 4; dy /= 4; bpp *= 16; }
        uint32_t row = (x1 - x0) * bpp / 8;
        for (uint32_t y = y0; y < y1; y++)
            memcpy(HP(dst->mem + (dy + y - y0) * dst->pitch + dx * bpp / 8), HP(src->mem + y * src->pitch + x0 * bpp / 8), row);
    }
    touch_container(dst);
    if (dst->is_rt && g_render->upload_surface) g_render->upload_surface(dv, dst);
}
W(dev_UpdateSurface) {   /* (src, srcrect, dst, dstpoint) */
    if (g_log_d3d) {
        Surface *a = com_data(ARG(1)), *b = com_data(ARG(3));
        static int n;
        if (a && b && n++ < 40) fprintf(stderr, "[d3d] UpdateSurface frame %u: %ux%u (mem %d) -> %ux%u (rt %d) at %d,%d\n", dev(c)->frame, a->width, a->height, a->mem != 0,
                                        b->width, b->height, b->is_rt, ARG(4) ? (int)R32(ARG(4)) : 0, ARG(4) ? (int)R32(ARG(4) + 4) : 0);
    }
    copy_rects(dev(c), com_data(ARG(1)), com_data(ARG(3)), 1, ARG(2), ARG(4));
    RET(5, S_OK);
}
W(dev_GetRenderTargetData) { copy_rects(dev(c), com_data(ARG(1)), com_data(ARG(2)), 1, 0, 0); RET(3, S_OK); }
Surface *g_last_stretch_dst;   /* debugging: the destination of the latest StretchRect draw */
/* Scaled copy between render targets, done as a draw: a textured quad covering the destination rectangle. */
static void stretch_by_draw(Device *d, uint32_t src_obj, Surface *src, uint32_t dst_obj, Surface *dst, uint32_t srect, uint32_t drect,
                            uint32_t filter) {
    {   /* RC_DUMP_BB_PRE=frame: write the back buffer (read back to memory) as /tmp/rc_bbpre_<frame>.ppm at the first bb->texture copy */
        static long want = -2; static uint32_t done;
        if (want == -2) want = getenv("RC_DUMP_BB_PRE") ? atol(getenv("RC_DUMP_BB_PRE")) : -1;
        if (want >= 0 && src_obj == d->backbuffer && d->frame == (uint32_t)want && done != d->frame && g_render->readback) {
            done = d->frame;
            g_render->readback(d, src);
            char path[64]; snprintf(path, sizeof path, "/tmp/rc_bbpre_%u.ppm", d->frame);
            FILE *f = fopen(path, "wb");
            if (f && src->mem) {
                fprintf(f, "P6\n%u %u\n255\n", src->width, src->height);
                for (uint32_t y = 0; y < src->height; y++)
                    for (uint32_t x = 0; x < src->width; x++) {
                        uint32_t px = R32(src->mem + y * src->pitch + x * 4);
                        fputc((px >> 16) & 255, f); fputc((px >> 8) & 255, f); fputc(px & 255, f);
                    }
            }
            if (f) fclose(f);
        }
    }
    uint32_t tex = src->container;
    if (!tex) {   /* a bare render target: wrap it in a texture object the renderer can sample */
        if (!src->stretch_tex) {
            Texture *t = calloc(1, sizeof *t);
            t->type = RT_TEXTURE; t->usage = 1; t->format = src->format; t->width = src->width; t->height = src->height;
            t->levels = 1; t->faces = 1; t->lw[0] = src->width; t->lh[0] = src->height; t->gpu_id = src->gpu_id;
            t->surfaces[0][0] = src_obj;
            src->stretch_tex = com_new(&IDirect3DTexture9_i, RT_TEXTURE, t);
        }
        tex = src->stretch_tex;
    }
    float sx0 = 0, sy0 = 0, sx1 = (float)src->width, sy1 = (float)src->height, dx0 = 0, dy0 = 0, dx1 = (float)dst->width, dy1 = (float)dst->height;
    if (srect) { sx0 = (float)(int)R32(srect); sy0 = (float)(int)R32(srect + 4); sx1 = (float)(int)R32(srect + 8); sy1 = (float)(int)R32(srect + 12); }
    if (drect) { dx0 = (float)(int)R32(drect); dy0 = (float)(int)R32(drect + 4); dx1 = (float)(int)R32(drect + 8); dy1 = (float)(int)R32(drect + 12); }
    float v[4][5] = {   /* x, y, z, rhw, then u, v packed after: stride 24 below */
        { dx0 - .5f, dy0 - .5f, 0, 1, 0 }, { dx1 - .5f, dy0 - .5f, 0, 1, 0 },
        { dx0 - .5f, dy1 - .5f, 0, 1, 0 }, { dx1 - .5f, dy1 - .5f, 0, 1, 0 } };
    float q[4][6];
    for (int k = 0; k < 4; k++) {
        memcpy(q[k], v[k], 16);
        q[k][4] = ((k & 1) ? sx1 : sx0) / (float)src->width;
        q[k][5] = ((k & 2) ? sy1 : sy0) / (float)src->height;
    }
    g_last_stretch_dst = dst;
    DevState saved = d->st;
    DevState *st = &d->st;
    st->render_target = dst_obj; st->depth_stencil = 0;
    st->viewport.x = st->viewport.y = 0; st->viewport.w = dst->width; st->viewport.h = dst->height;
    st->viewport.minz = 0.f; st->viewport.maxz = 1.f;
    st->vs = 0x104;   /* XYZRHW | TEX1 */
    st->ps = 0;
    for (int t = 0; t < 8; t++) st->texture[t] = 0;
    st->texture[0] = tex;
    uint32_t *rs = st->rs;
    rs[7] = 0; rs[14] = 0; rs[27] = 0; rs[15] = 0; rs[22] = 1; rs[137] = 0; rs[28] = 0; rs[168] = 0xF;
    st->tss[0][1] = 2; st->tss[0][2] = 2; st->tss[0][4] = 2; st->tss[0][5] = 2;
    st->tss[0][13] = st->tss[0][14] = 3;
    st->tss[0][16] = st->tss[0][17] = (filter == 1 || getenv("RC_STRETCH_POINT")) ? 1 : 2;
    st->tss[0][18] = 0;   /* no mip filter */
    st->tss[1][1] = 1; st->tss[1][4] = 1;
    if (getenv("RC_STRETCH_WHITE") && g_render->clear) g_render->clear(d, 0, 0, 1, 0xFF00FF00u, 1.f, 0);   /* debugging: pre-clear the destination to green */
    if (g_render->draw) g_render->draw(d, 5, 2, 0, 4, (const uint8_t *)q, sizeof q[0], NULL, 0, 0);
    d->st = saved;
    {   /* RC_DUMP_STRETCH_OUT=frame: write the destination right after the first copy of that frame as /tmp/rc_stretchout_<frame>.ppm */
        static long want = -2; static uint32_t done;
        if (want == -2) want = getenv("RC_DUMP_STRETCH_OUT") ? atol(getenv("RC_DUMP_STRETCH_OUT")) : -1;
        if (want >= 0 && d->frame == (uint32_t)want && done != d->frame && g_render->readback && dst->mem) {
            done = d->frame;
            g_render->readback(d, dst);
            FILE *f = fopen("/tmp/rc_stretchout.ppm", "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", dst->width, dst->height);
                for (uint32_t y = 0; y < dst->height; y++)
                    for (uint32_t x = 0; x < dst->width; x++) {
                        uint32_t px = R32(dst->mem + y * dst->pitch + x * 4);
                        fputc((px >> 16) & 255, f); fputc((px >> 8) & 255, f); fputc(px & 255, f);
                    }
                fclose(f);
            }
        }
    }
}
W(dev_StretchRect) {   /* (src, srcrect, dst, dstrect, filter) */
    Device *dv = dev(c);
    Surface *src = com_data(ARG(1)), *dst = com_data(ARG(3));
    if (!src || !dst) RET(6, D3DERR_INVALIDCALL);
    if (g_log_d3d) {
        static int n;
        if (n++ < 30 || (dv->frame >= 298 && dv->frame <= 301)) fprintf(stderr, "[d3d] StretchRect frame %u (draws %u): %ux%u (id %u, container %d, bb %d) rect %d,%d,%d,%d -> %ux%u (id %u, bb %d) rect %d,%d,%d,%d filter %u\n", dv->frame, dv->draws_this_frame,
                              src->width, src->height, src->gpu_id, src->container != 0, ARG(1) == dv->backbuffer,
                              ARG(2) ? (int)R32(ARG(2)) : -1, ARG(2) ? (int)R32(ARG(2) + 4) : -1, ARG(2) ? (int)R32(ARG(2) + 8) : -1, ARG(2) ? (int)R32(ARG(2) + 12) : -1,
                              dst->width, dst->height, dst->gpu_id, ARG(3) == dv->backbuffer,
                              ARG(4) ? (int)R32(ARG(4)) : -1, ARG(4) ? (int)R32(ARG(4) + 4) : -1, ARG(4) ? (int)R32(ARG(4) + 8) : -1, ARG(4) ? (int)R32(ARG(4) + 12) : -1, ARG(5));
    }
    static int warned;
    int gpu_pair = is_gpu_surface(dv, src) && is_gpu_surface(dv, dst);
    {   /* debugging: RC_STRETCH_SKIP=<hex mask>: skip the Nth StretchRect of each frame (bit N) */
        static uint32_t last_frame = ~0u, idx; static long mask = -1;
        if (mask < 0) mask = getenv("RC_STRETCH_SKIP") ? strtol(getenv("RC_STRETCH_SKIP"), NULL, 16) : 0;
        if (dv->frame != last_frame) { last_frame = dv->frame; idx = 0; }
        if (mask && idx < 31 && (mask >> idx++ & 1)) RET(6, S_OK);
    }
    if (gpu_pair && ARG(3) != dv->backbuffer + 0 * 1 && getenv("RC_NO_STRETCH_DRAW") == NULL) {
        stretch_by_draw(dv, ARG(1), src, ARG(3), dst, ARG(2), ARG(4), ARG(5));   /* any source (also the back buffer) -> a render target */
        RET(6, S_OK);
    }
    if (gpu_pair && ARG(1) != dv->backbuffer && getenv("RC_NO_STRETCH_DRAW") == NULL) {
        stretch_by_draw(dv, ARG(1), src, ARG(3), dst, ARG(2), ARG(4), ARG(5));   /* a render target -> the back buffer */
        RET(6, S_OK);
    }
    if (gpu_pair) {   /* back buffer -> back buffer: nothing to do */
        RET(6, S_OK);
    }
    if (src->width != dst->width || src->height != dst->height) {
        if (!warned++)
            fprintf(stderr, "[d3d] StretchRect %ux%u (rt %d, backbuffer %d, mem %d) -> %ux%u (rt %d, backbuffer %d, mem %d): not implemented (skipped)\n",
                    src->width, src->height, src->is_rt, ARG(1) == dv->backbuffer, src->mem != 0,
                    dst->width, dst->height, dst->is_rt, ARG(3) == dv->backbuffer, dst->mem != 0);
    } else
        copy_rects(dv, src, dst, 1, ARG(2), ARG(4));
    RET(6, S_OK);
}
W(dev_ColorFill) {   /* (surface, rect, color): only memory-backed 32-bit surfaces for now */
    Surface *s = com_data(ARG(1));
    if (s && s->mem && format_bpp(s->format) == 32) {
        uint32_t x0 = 0, y0 = 0, x1 = s->width, y1 = s->height;
        if (ARG(2)) { x0 = R32(ARG(2)); y0 = R32(ARG(2) + 4); x1 = R32(ARG(2) + 8); y1 = R32(ARG(2) + 12); }
        for (uint32_t y = y0; y < y1; y++) for (uint32_t x = x0; x < x1; x++) W32(s->mem + y * s->pitch + x * 4, ARG(3));
        touch_container(s);
        if (s->is_rt && g_render->upload_surface) g_render->upload_surface(dev(c), s);
    }
    RET(4, S_OK);
}
W(dev_UpdateTexture) {
    Texture *s = com_data(ARG(1)), *d = com_data(ARG(2));
    if (s && d && s->format == d->format)
        for (uint32_t f = 0; f < d->faces; f++)
            for (uint32_t l = 0; l < d->levels && l < s->levels; l++)
                memcpy(HP(d->mem[f][l]), HP(s->mem[f][l]), d->size[l] < s->size[l] ? d->size[l] : s->size[l]);
    if (d) d->version++;
    RET(3, S_OK);
}
W(dev_GetFrontBufferData) { RET(3, S_OK); }
W(dev_SetRenderTarget) {   /* (index, surface) */
    Device *d = dev(c);
    if (ARG(1) == 0 && ARG(2)) {
        d->st.render_target = ARG(2);
        Surface *rt = com_data(ARG(2));   /* Direct3D resets the viewport to the whole new target */
        if (rt) { d->st.viewport.x = d->st.viewport.y = 0; d->st.viewport.w = rt->width; d->st.viewport.h = rt->height;
                  d->st.viewport.minz = 0.f; d->st.viewport.maxz = 1.f; }
    }
    RET(3, S_OK);
}
W(dev_SetDepthStencilSurface) { dev(c)->st.depth_stencil = ARG(1); RET(2, S_OK); }
static void out_addref(uint32_t out, uint32_t obj) {
    ComObj *o = com_get(obj);
    if (o) o->refs++;
    W32(out, obj);
}
W(dev_GetRenderTarget) { out_addref(ARG(2), dev(c)->st.render_target); RET(3, ARG(1) == 0 ? S_OK : D3DERR_INVALIDCALL); }
W(dev_GetDepthStencilSurface) {
    uint32_t ds = dev(c)->st.depth_stencil;
    out_addref(ARG(1), ds);
    RET(2, ds ? S_OK : D3DERR_NOTFOUND);
}
W(dev_BeginScene) { RET(1, S_OK); }
W(dev_EndScene) { RET(1, S_OK); }
W(dev_Clear) {
    if (g_log_d3d >= 3) fprintf(stderr, "[d3d] Clear frame %u: count %u flags %x color %08x rt %x viewport %u,%u %ux%u\n", dev(c)->frame, ARG(1), ARG(3), ARG(4),
                                dev(c)->st.render_target, dev(c)->st.viewport.x, dev(c)->st.viewport.y, dev(c)->st.viewport.w, dev(c)->st.viewport.h);
    if (g_render->clear) {
        float z;
        uint32_t zbits = ARG(5);
        memcpy(&z, &zbits, 4);
        g_render->clear(dev(c), ARG(1), ARG(2), ARG(3), ARG(4), z, ARG(6));
    }
    RET(7, S_OK);
}
static uint32_t ts_index(uint32_t t) { return t < MAX_TS ? t : MAX_TS - 1; }
W(dev_SetTransform) { memcpy(dev(c)->st.transform[ts_index(ARG(1))], HP(ARG(2)), 64); RET(3, S_OK); }
W(dev_GetTransform) { memcpy(HP(ARG(2)), dev(c)->st.transform[ts_index(ARG(1))], 64); RET(3, S_OK); }
W(dev_MultiplyTransform) {
    float *m = dev(c)->st.transform[ts_index(ARG(1))], r[16], b[16];
    memcpy(b, HP(ARG(2)), 64);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += m[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s;
    }
    memcpy(m, r, 64);
    RET(3, S_OK);
}
W(dev_SetViewport) { memcpy(&dev(c)->st.viewport, HP(ARG(1)), 24); RET(2, S_OK); }
W(dev_GetViewport) { memcpy(HP(ARG(1)), &dev(c)->st.viewport, 24); RET(2, S_OK); }
W(dev_SetMaterial) { memcpy(dev(c)->st.material, HP(ARG(1)), 68); RET(2, S_OK); }
W(dev_GetMaterial) { memcpy(HP(ARG(1)), dev(c)->st.material, 68); RET(2, S_OK); }
W(dev_SetLight) { if (ARG(1) < 8) memcpy(dev(c)->st.light[ARG(1)], HP(ARG(2)), 104); RET(3, S_OK); }
W(dev_GetLight) { if (ARG(1) < 8) memcpy(HP(ARG(2)), dev(c)->st.light[ARG(1)], 104); RET(3, S_OK); }
W(dev_LightEnable) { if (ARG(1) < 8) dev(c)->st.light_enabled[ARG(1)] = ARG(2); RET(3, S_OK); }
W(dev_GetLightEnable) { W32(ARG(2), ARG(1) < 8 ? dev(c)->st.light_enabled[ARG(1)] : 0); RET(3, S_OK); }
W(dev_SetClipPlane) { if (ARG(1) < 6) memcpy(dev(c)->st.clip_plane[ARG(1)], HP(ARG(2)), 16); RET(3, S_OK); }
W(dev_GetClipPlane) { if (ARG(1) < 6) memcpy(HP(ARG(2)), dev(c)->st.clip_plane[ARG(1)], 16); RET(3, S_OK); }
W(dev_SetRenderState) { if (ARG(1) < 256) dev(c)->st.rs[ARG(1)] = ARG(2); RET(3, S_OK); }
W(dev_GetRenderState) { W32(ARG(2), ARG(1) < 256 ? dev(c)->st.rs[ARG(1)] : 0); RET(3, S_OK); }

/* State blocks: recorded blocks capture a full snapshot; good enough until a game relies on partial blocks. */
static uint32_t new_block(Device *d) {
    DevState *b = malloc(sizeof(DevState));
    *b = d->st;
    return com_new(&IDirect3DStateBlock9_i, RT_STATEBLOCK, b);
}
W(dev_BeginStateBlock) { dev(c)->recording_block = 1; RET(1, S_OK); }
W(dev_EndStateBlock) { Device *d = dev(c); d->recording_block = 0; W32(ARG(1), new_block(d)); RET(2, S_OK); }
W(dev_CreateStateBlock) { W32(ARG(2), new_block(dev(c))); RET(3, S_OK); }
W(sb_GetDevice) { out_addref(ARG(1), g_device_object); RET(2, S_OK); }
W(sb_Capture) { DevState *b = com_data(THIS); if (b && g_device) *b = g_device->st; RET(1, S_OK); }
W(sb_Apply) {
    DevState *b = com_data(THIS);
    Device *d = g_device;
    if (b && d) {
        uint32_t rt = d->st.render_target, ds = d->st.depth_stencil;
        d->st = *b;
        d->st.render_target = rt; d->st.depth_stencil = ds;
    }
    RET(1, S_OK);
}
static const ComMethod sb_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetDevice", 1, sb_GetDevice }, { "Capture", 0, sb_Capture }, { "Apply", 0, sb_Apply },
};
static void destroy_plain(void *p) { free(p); }
static ComInterface IDirect3DStateBlock9_i = { "IDirect3DStateBlock9", sb_methods, 6, 0, destroy_plain };
W(dev_SetClipStatus) { RET(2, S_OK); }
W(dev_GetClipStatus) { memset(HP(ARG(1)), 0, 8); RET(2, S_OK); }
W(dev_GetTexture) { out_addref(ARG(2), ARG(1) < 8 ? dev(c)->st.texture[ARG(1)] : 0); RET(3, S_OK); }
W(dev_SetTexture) { if (ARG(1) < 8) dev(c)->st.texture[ARG(1)] = ARG(2); RET(3, S_OK); }
W(dev_GetTextureStageState) { W32(ARG(3), ARG(1) < 8 && ARG(2) < 32 ? dev(c)->st.tss[ARG(1)][ARG(2)] : 0); RET(4, S_OK); }
W(dev_SetTextureStageState) { if (ARG(1) < 8 && ARG(2) < 32) dev(c)->st.tss[ARG(1)][ARG(2)] = ARG(3); RET(4, S_OK); }
/* Sampler states live in the same per-stage table under their Direct3D 8 texture-stage numbers, which the
   renderer reads: ADDRESSU/V/W/BORDERCOLOR, MAG/MIN/MIPFILTER, MIPMAPLODBIAS, MAXMIPLEVEL, MAXANISOTROPY */
static const uint8_t k_sampler_slot[11] = { 0, 13, 14, 25, 15, 16, 17, 18, 19, 20, 21 };
W(dev_GetSamplerState) { W32(ARG(3), ARG(1) < 8 && ARG(2) >= 1 && ARG(2) <= 10 ? dev(c)->st.tss[ARG(1)][k_sampler_slot[ARG(2)]] : 0); RET(4, S_OK); }
W(dev_SetSamplerState) { if (ARG(1) < 8 && ARG(2) >= 1 && ARG(2) <= 10) dev(c)->st.tss[ARG(1)][k_sampler_slot[ARG(2)]] = ARG(3); RET(4, S_OK); }
W(dev_SetScissorRect) { memcpy(dev(c)->st.scissor, HP(ARG(1)), 16); RET(2, S_OK); }
W(dev_GetScissorRect) { memcpy(HP(ARG(1)), dev(c)->st.scissor, 16); RET(2, S_OK); }
W(dev_SetSoftwareVertexProcessing) { RET(2, S_OK); }
W(dev_GetSoftwareVertexProcessing) { RET(1, 0); }
W(dev_SetNPatchMode) { RET(2, S_OK); }
W(dev_GetNPatchMode) { RET(1, 0); }
W(dev_ValidateDevice) { if (ARG(1)) W32(ARG(1), 1); RET(2, S_OK); }
W(dev_SetPaletteEntries) { if (ARG(1) < 1024) memcpy(g_palettes[ARG(1)], HP(ARG(2)), 1024); RET(3, S_OK); }
W(dev_GetPaletteEntries) { if (ARG(1) < 1024) memcpy(HP(ARG(2)), g_palettes[ARG(1)], 1024); RET(3, S_OK); }
W(dev_SetCurrentTexturePalette) { dev(c)->st.palette_current = ARG(1); RET(2, S_OK); }
W(dev_GetCurrentTexturePalette) { W32(ARG(1), dev(c)->st.palette_current); RET(2, S_OK); }

/* primitive count -> vertex count */
static uint32_t prim_vertices(uint32_t prim, uint32_t n) {
    switch (prim) {
    case 1: return n;          /* point list */
    case 2: return n * 2;      /* line list */
    case 3: return n + 1;      /* line strip */
    case 4: return n * 3;      /* triangle list */
    case 5: case 6: return n + 2;  /* strip / fan */
    default: return 0;
    }
}

W(dev_DrawPrimitive) {
    Device *d = dev(c);
    d->draws_this_frame++;
    if (g_render->draw) g_render->draw(d, ARG(1), ARG(3), ARG(2), prim_vertices(ARG(1), ARG(3)), NULL, 0, NULL, 0, 0);
    RET(4, S_OK);
}
W(dev_DrawIndexedPrimitive) {
    Device *d = dev(c);
    d->draws_this_frame++;
    Buffer *ib = com_data(d->st.ib);
    if (g_render->draw && ib) {   /* (type, base vertex, min index, num vertices, start index, primitive count) */
        int i32 = ib->fvf_or_format == FMT_INDEX32;
        d->st.base_vertex = ARG(2);
        g_render->draw(d, ARG(1), ARG(6), 0, ARG(4), NULL, 0, HP(ib->mem), i32, ARG(5));
    }
    RET(7, S_OK);
}
W(dev_DrawPrimitiveUP) {
    Device *d = dev(c);
    d->draws_this_frame++;
    if (g_render->draw)
        g_render->draw(d, ARG(1), ARG(2), 0, prim_vertices(ARG(1), ARG(2)), HP(ARG(3)), ARG(4), NULL, 0, 0);
    RET(5, S_OK);
}
W(dev_DrawIndexedPrimitiveUP) {
    Device *d = dev(c);
    d->draws_this_frame++;
    if (g_render->draw)
        g_render->draw(d, ARG(1), ARG(4), ARG(2), ARG(3), HP(ARG(7)), ARG(8), HP(ARG(5)), ARG(6) == FMT_INDEX32, 0);
    RET(9, S_OK);
}
W(dev_ProcessVertices) { RET(7, S_OK); }

/* Shaders. A Shader is the host copy of a vertex declaration, a vertex shader or a pixel shader; the
   renderer works with "handles" (odd numbers) that name a table slot, and with a vertex declaration and a
   vertex shader bound together the handle names a combined Shader (see bind_vertex). */
#define MAX_SHADERS 4096
static Shader *g_shaders[MAX_SHADERS];
Shader *shader_get(uint32_t h) {
    uint32_t i = (h - 0x10001u) / 2;
    return (h & 1) && h >= 0x10001u && i < MAX_SHADERS ? g_shaders[i] : NULL;
}
static uint32_t copy_tokens(uint32_t p, uint32_t end_token, uint32_t **out) {
    if (!p) { *out = NULL; return 0; }
    uint32_t n = 0;
    while (R32(p + 4 * n) != end_token && n < 65536) n++;
    n++;
    *out = malloc(4 * n);
    memcpy(*out, HP(p), 4 * n);
    return n;
}
static uint32_t new_shader(Shader *s) {
    for (uint32_t i = 0; i < MAX_SHADERS; i++) if (!g_shaders[i]) { g_shaders[i] = s; return s->handle = 0x10001u + 2 * i; }
    return 0;
}
static Shader *shader_obj(uint32_t obj) { return obj ? com_data(obj) : NULL; }

/* D3DVERTEXELEMENT9 {WORD stream, offset; BYTE type, method, usage, usage_index} -> Direct3D 8 declaration tokens.
   Input registers for vs_1_x follow the fixed usage mapping: position v0, blend weight v1, blend indices v2,
   normal v3, point size v4, color0 v5, color1 v6, texcoord0-7 v7-v14, position1 v15. */
static int element_register(uint32_t usage, uint32_t idx) {
    switch (usage) {
    case 0: return idx ? 15 : 0;       /* POSITION */
    case 1: return 1;                  /* BLENDWEIGHT */
    case 2: return 2;                  /* BLENDINDICES */
    case 3: return 3;                  /* NORMAL */
    case 4: return 4;                  /* PSIZE */
    case 9: return 0;                  /* POSITIONT: pre-transformed position, bound like POSITION */
    case 5: return 7 + (int)(idx & 7); /* TEXCOORD */
    case 10: return 5 + (int)(idx & 1);/* COLOR */
    default: return -1;
    }
}
/* usage -> input register overrides taken from a shader's dcl instructions; -1 = the fixed vs_1_x mapping */
typedef struct { int8_t reg[16][16]; } DclMap;
static uint32_t declaration_tokens(const uint32_t *el, const DclMap *map, uint32_t **out) {
    uint32_t cap = 64, n = 0;
    uint32_t *t = malloc(4 * cap);
    for (uint32_t stream = 0; stream < 16; stream++) {
        uint32_t off = 0;
        int first = 1;
        /* elements of this stream in offset order */
        for (;;) {
            int best = -1; uint32_t best_off = 0xFFFFFFFFu;
            for (int i = 0; ((const uint16_t *)el)[4 * i] != 0xFF; i++) {
                const uint16_t *q = (const uint16_t *)el + 4 * i;
                if (q[0] != stream || q[1] < off) continue;
                if (q[1] < best_off) { best_off = q[1]; best = i; }
            }
            if (best < 0) break;
            const uint8_t *q = (const uint8_t *)el + 8 * best;
            uint32_t type = q[4], usage = q[6], uidx = q[7];
            static const uint8_t size[17] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8 };   /* D3DDECLTYPE sizes; the types carry over unchanged */
            uint32_t mu = usage == 9 ? 0 : usage;   /* a shader's dcl_position also covers POSITIONT */
            int reg = map && mu < 16 && uidx < 16 && map->reg[mu][uidx] >= 0 ? map->reg[mu][uidx] : element_register(usage, uidx);
            if (n + 8 >= cap) { cap *= 2; t = realloc(t, 4 * cap); }
            if (first) { t[n++] = 0x20000000u | stream; first = 0; }
            if (best_off > off) { t[n++] = 0x50000000u | (((best_off - off) / 4) << 16); off = best_off; }  /* D3DVSD_SKIP */
            if (reg >= 0 && type < 17) t[n++] = 0x40000000u | (type << 16) | (uint32_t)reg;  /* D3DVSD_REG (type widened to 5 bits) */
            off += type < 17 ? size[type] : 4;   /* next element must start at or after this */
        }
    }
    t[n++] = 0xFFFFFFFFu;
    *out = t;
    return n;
}
W(dev_CreateVertexDeclaration) {
    Shader *s = calloc(1, sizeof *s);
    uint32_t n = 0;
    while (R16(ARG(1) + 8 * n) != 0xFF && n < 64) n++;
    n++;   /* include the D3DDECL_END element */
    s->elems = malloc(8 * n); s->n_elems = n;
    memcpy(s->elems, HP(ARG(1)), 8 * n);
    s->decl_len = declaration_tokens(s->elems, NULL, &s->decl);
    new_shader(s);
    if (g_log_d3d) {
        fprintf(stderr, "[d3d] CreateVertexDeclaration:");
        for (uint32_t k = 0; k + 1 < n; k++) {
            const uint8_t *q = (const uint8_t *)s->elems + 8 * k;
            fprintf(stderr, " {s%u +%u t%u u%u.%u}", q[0] | q[1] << 8, q[2] | q[3] << 8, q[4], q[6], q[7]);
        }
        fprintf(stderr, "\n");
    }
    W32(ARG(2), com_new(&IDirect3DVertexDeclaration9_i, RT_SHADER, s));
    if (g_log_d3d) fprintf(stderr, "[d3d] CreateVertexDeclaration -> %u tokens\n", s->decl_len);
    RET(3, S_OK);
}
W(dev_CreateVertexShader) {
    Shader *s = calloc(1, sizeof *s);
    s->code_len = copy_tokens(ARG(1), 0x0000FFFFu, &s->code);
    new_shader(s);
    if (g_log_d3d) fprintf(stderr, "[d3d] CreateVertexShader: %u tokens (version %08x)\n", s->code_len, s->code ? s->code[0] : 0);
    W32(ARG(2), com_new(&IDirect3DVertexShader9_i, RT_SHADER, s));
    RET(3, S_OK);
}
W(dev_CreatePixelShader) {
    Shader *s = calloc(1, sizeof *s);
    s->pixel = 1;
    s->code_len = copy_tokens(ARG(1), 0x0000FFFFu, &s->code);
    new_shader(s);
    if (g_log_d3d) fprintf(stderr, "[d3d] CreatePixelShader: %u tokens (version %08x)\n", s->code_len, s->code ? s->code[0] : 0);
    W32(ARG(2), com_new(&IDirect3DPixelShader9_i, RT_SHADER, s));
    RET(3, S_OK);
}

/* Binds what the renderer calls st.vs: a shader handle when a declaration is set, otherwise the FVF. */
static Shader *g_combos[256];
static int g_ncombos;
static void bind_vertex(Device *d) {
    Shader *decl = shader_obj(d->st.vdecl), *code = shader_obj(d->st.vshader);
    if (!decl) { d->st.vs = d->st.fvf; return; }
    {
        /* A declaration with POSITIONT is pre-transformed: Direct3D 9 skips the vertex shader for it (even when one is set), so describe
           it to the renderer as an FVF (XYZRHW, diffuse, specular, texture coordinate count). */
        uint32_t fvf = 0, ntex = 0;
        for (uint32_t k = 0; decl->elems && k + 1 < decl->n_elems; k++) {
            const uint8_t *q = (const uint8_t *)decl->elems + 8 * k;
            if (q[6] == 9) fvf |= 0x4;
            else if (q[6] == 10 && q[7] == 0) fvf |= 0x40;
            else if (q[6] == 10 && q[7] == 1) fvf |= 0x80;
            else if (q[6] == 5 && q[7] + 1 > ntex) ntex = q[7] + 1;
        }
        if (fvf & 0x4) { d->st.vs = fvf | (ntex << 8); return; }
    }
    if (!code) { d->st.vs = decl->handle; return; }
    for (int i = 0; i < g_ncombos; i++)
        if (g_combos[i]->elems == decl->elems && g_combos[i]->code == code->code) { d->st.vs = g_combos[i]->handle; return; }
    Shader *s = calloc(1, sizeof *s);
    DclMap map;
    memset(&map, 0xFF, sizeof map);
    int have_dcl = 0;
    for (uint32_t i = 1; i < code->code_len;) {   /* dcl_usage v#: {31, usage (bits 0-4) | index (16-19), register} */
        uint32_t tok = code->code[i++], op = tok & 0xFFFF;
        if (tok == 0x0000FFFFu) break;
        if (op == 0xFFFE) { i += (tok >> 16) & 0x7FFF; continue; }
        if (op == 31 && i + 1 < code->code_len) {
            uint32_t usage = code->code[i] & 0x1F, idx = (code->code[i] >> 16) & 0xF, reg = code->code[i + 1] & 0x7FF;
            if (usage < 16 && reg < 16) { map.reg[usage][idx] = (int8_t)reg; have_dcl = 1; }
            i += 2;
            continue;
        }
        if (op == 81) { i += 5; continue; }
        /* other vs_1_x instructions: skip by operand count (destination + sources) */
        static const uint8_t np[26] = { 0, 2, 3, 3, 4, 3, 2, 2, 3, 3, 3, 3, 3, 3, 2, 2, 2, 3, 4, 2, 3, 3, 3, 3, 3, 0 };
        i += op < 26 ? np[op] : 0;
    }
    s->code = code->code; s->code_len = code->code_len;
    s->elems = decl->elems; s->n_elems = decl->n_elems;   /* (the cache key above) */
    if (have_dcl && decl->elems) { s->decl_len = declaration_tokens(decl->elems, &map, &s->decl); }
    else { s->decl = decl->decl; s->decl_len = decl->decl_len; }
    new_shader(s);
    if (g_log_d3d) {
        for (uint32_t k = 0; decl->elems && k + 1 < decl->n_elems; k++)
            if (((const uint8_t *)decl->elems)[8 * k + 6] == 9) { fprintf(stderr, "[d3d] shader %x uses POSITIONT (screen-space UI)\n", s->handle); break; }
    }
    if (g_ncombos < 256) g_combos[g_ncombos++] = s;
    d->st.vs = s->handle;
}
W(dev_SetVertexDeclaration) { Device *d = dev(c); d->st.vdecl = ARG(1); bind_vertex(d); RET(2, S_OK); }
W(dev_GetVertexDeclaration) { out_addref(ARG(1), dev(c)->st.vdecl); RET(2, S_OK); }
W(dev_SetFVF) {   /* SetFVF(0) changes nothing: the vertex declaration stays */
    Device *d = dev(c);
    if (ARG(1)) { d->st.fvf = ARG(1); d->st.vdecl = 0; bind_vertex(d); }
    RET(2, S_OK);
}
W(dev_GetFVF) { W32(ARG(1), dev(c)->st.fvf); RET(2, S_OK); }
W(dev_SetVertexShader) { Device *d = dev(c); d->st.vshader = ARG(1); bind_vertex(d); RET(2, S_OK); }
W(dev_GetVertexShader) { out_addref(ARG(1), dev(c)->st.vshader); RET(2, S_OK); }
W(dev_SetVertexShaderConstantF) {
    uint32_t r = ARG(1), n = ARG(3);
    if (r < 96) memcpy(dev(c)->st.vs_const[r], HP(ARG(2)), 16 * (r + n > 96 ? 96 - r : n));
    RET(4, S_OK);
}
W(dev_GetVertexShaderConstantF) {
    uint32_t r = ARG(1), n = ARG(3);
    if (r < 96) memcpy(HP(ARG(2)), dev(c)->st.vs_const[r], 16 * (r + n > 96 ? 96 - r : n));
    RET(4, S_OK);
}
W(dev_ShaderConstantIB) { RET(4, S_OK); }   /* integer / boolean constants: vs_2_0 and later, not used with vs_1_1 */
W(dev_SetStreamSource) {   /* (stream, vb, offset, stride) */
    if (ARG(1) < 16) { Device *d = dev(c); d->st.stream_vb[ARG(1)] = ARG(2); d->st.stream_offset[ARG(1)] = ARG(3); d->st.stream_stride[ARG(1)] = ARG(4); }
    RET(5, S_OK);
}
W(dev_GetStreamSource) {
    if (ARG(1) < 16) { out_addref(ARG(2), dev(c)->st.stream_vb[ARG(1)]); W32(ARG(3), dev(c)->st.stream_offset[ARG(1)]); W32(ARG(4), dev(c)->st.stream_stride[ARG(1)]); }
    RET(5, S_OK);
}
W(dev_SetStreamSourceFreq) { RET(3, S_OK); }
W(dev_GetStreamSourceFreq) { W32(ARG(2), 1); RET(3, S_OK); }
W(dev_SetIndices) { dev(c)->st.ib = ARG(1); RET(2, S_OK); }
W(dev_GetIndices) { out_addref(ARG(1), dev(c)->st.ib); RET(2, S_OK); }
W(dev_SetPixelShader) { Shader *sh = shader_obj(ARG(1)); dev(c)->st.pixel_shader_obj = ARG(1); dev(c)->st.ps = sh ? sh->handle : 0; RET(2, S_OK); }
W(dev_GetPixelShader) { out_addref(ARG(1), dev(c)->st.pixel_shader_obj); RET(2, S_OK); }
W(dev_SetPixelShaderConstantF) {
    uint32_t r = ARG(1), n = ARG(3);
    if (r < 8) memcpy(dev(c)->st.ps_const[r], HP(ARG(2)), 16 * (r + n > 8 ? 8 - r : n));
    RET(4, S_OK);
}
W(dev_GetPixelShaderConstantF) {
    uint32_t r = ARG(1), n = ARG(3);
    if (r < 8) memcpy(HP(ARG(2)), dev(c)->st.ps_const[r], 16 * (r + n > 8 ? 8 - r : n));
    RET(4, S_OK);
}
W(dev_CreateQuery) { RET(3, D3DERR_NOTAVAILABLE); }
W(dev_SetCursorProperties) { RET(4, S_OK); }
W(dev_SetCursorPosition) { RET(4, S_OK); }
W(dev_ShowCursor) { RET(2, 0); }

static const ComMethod dev_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "TestCooperativeLevel", 0, dev_TestCooperativeLevel }, { "GetAvailableTextureMem", 0, dev_GetAvailableTextureMem },
    { "EvictManagedResources", 0, dev_EvictManagedResources }, { "GetDirect3D", 1, dev_GetDirect3D },
    { "GetDeviceCaps", 1, dev_GetDeviceCaps }, { "GetDisplayMode", 2, dev_GetDisplayMode },
    { "GetCreationParameters", 1, dev_GetCreationParameters }, { "SetCursorProperties", 3, dev_SetCursorProperties },
    { "SetCursorPosition", 3, dev_SetCursorPosition }, { "ShowCursor", 1, dev_ShowCursor },
    { "CreateAdditionalSwapChain", 2, dev_CreateAdditionalSwapChain }, { "GetSwapChain", 2, dev_GetSwapChain },
    { "GetNumberOfSwapChains", 0, dev_GetNumberOfSwapChains }, { "Reset", 1, dev_Reset },
    { "Present", 4, dev_Present }, { "GetBackBuffer", 4, dev_GetBackBuffer },
    { "GetRasterStatus", 2, dev_GetRasterStatus }, { "SetDialogBoxMode", 1, dev_SetDialogBoxMode },
    { "SetGammaRamp", 3, dev_SetGammaRamp }, { "GetGammaRamp", 2, dev_GetGammaRamp },
    { "CreateTexture", 8, dev_CreateTexture }, { "CreateVolumeTexture", 9, dev_CreateVolumeTexture },
    { "CreateCubeTexture", 7, dev_CreateCubeTexture }, { "CreateVertexBuffer", 6, dev_CreateVertexBuffer },
    { "CreateIndexBuffer", 6, dev_CreateIndexBuffer }, { "CreateRenderTarget", 8, dev_CreateRenderTarget },
    { "CreateDepthStencilSurface", 8, dev_CreateDepthStencilSurface }, { "UpdateSurface", 4, dev_UpdateSurface },
    { "UpdateTexture", 2, dev_UpdateTexture }, { "GetRenderTargetData", 2, dev_GetRenderTargetData },
    { "GetFrontBufferData", 2, dev_GetFrontBufferData }, { "StretchRect", 5, dev_StretchRect },
    { "ColorFill", 3, dev_ColorFill }, { "CreateOffscreenPlainSurface", 6, dev_CreateOffscreenPlainSurface },
    { "SetRenderTarget", 2, dev_SetRenderTarget }, { "GetRenderTarget", 2, dev_GetRenderTarget },
    { "SetDepthStencilSurface", 1, dev_SetDepthStencilSurface }, { "GetDepthStencilSurface", 1, dev_GetDepthStencilSurface },
    { "BeginScene", 0, dev_BeginScene }, { "EndScene", 0, dev_EndScene }, { "Clear", 6, dev_Clear },
    { "SetTransform", 2, dev_SetTransform }, { "GetTransform", 2, dev_GetTransform },
    { "MultiplyTransform", 2, dev_MultiplyTransform }, { "SetViewport", 1, dev_SetViewport },
    { "GetViewport", 1, dev_GetViewport }, { "SetMaterial", 1, dev_SetMaterial }, { "GetMaterial", 1, dev_GetMaterial },
    { "SetLight", 2, dev_SetLight }, { "GetLight", 2, dev_GetLight }, { "LightEnable", 2, dev_LightEnable },
    { "GetLightEnable", 2, dev_GetLightEnable }, { "SetClipPlane", 2, dev_SetClipPlane },
    { "GetClipPlane", 2, dev_GetClipPlane }, { "SetRenderState", 2, dev_SetRenderState },
    { "GetRenderState", 2, dev_GetRenderState }, { "CreateStateBlock", 2, dev_CreateStateBlock },
    { "BeginStateBlock", 0, dev_BeginStateBlock }, { "EndStateBlock", 1, dev_EndStateBlock },
    { "SetClipStatus", 1, dev_SetClipStatus }, { "GetClipStatus", 1, dev_GetClipStatus },
    { "GetTexture", 2, dev_GetTexture }, { "SetTexture", 2, dev_SetTexture },
    { "GetTextureStageState", 3, dev_GetTextureStageState }, { "SetTextureStageState", 3, dev_SetTextureStageState },
    { "GetSamplerState", 3, dev_GetSamplerState }, { "SetSamplerState", 3, dev_SetSamplerState },
    { "ValidateDevice", 1, dev_ValidateDevice }, { "SetPaletteEntries", 2, dev_SetPaletteEntries },
    { "GetPaletteEntries", 2, dev_GetPaletteEntries }, { "SetCurrentTexturePalette", 1, dev_SetCurrentTexturePalette },
    { "GetCurrentTexturePalette", 1, dev_GetCurrentTexturePalette }, { "SetScissorRect", 1, dev_SetScissorRect },
    { "GetScissorRect", 1, dev_GetScissorRect }, { "SetSoftwareVertexProcessing", 1, dev_SetSoftwareVertexProcessing },
    { "GetSoftwareVertexProcessing", 0, dev_GetSoftwareVertexProcessing }, { "SetNPatchMode", 1, dev_SetNPatchMode },
    { "GetNPatchMode", 0, dev_GetNPatchMode }, { "DrawPrimitive", 3, dev_DrawPrimitive },
    { "DrawIndexedPrimitive", 6, dev_DrawIndexedPrimitive }, { "DrawPrimitiveUP", 4, dev_DrawPrimitiveUP },
    { "DrawIndexedPrimitiveUP", 8, dev_DrawIndexedPrimitiveUP }, { "ProcessVertices", 6, dev_ProcessVertices },
    { "CreateVertexDeclaration", 2, dev_CreateVertexDeclaration }, { "SetVertexDeclaration", 1, dev_SetVertexDeclaration },
    { "GetVertexDeclaration", 1, dev_GetVertexDeclaration }, { "SetFVF", 1, dev_SetFVF }, { "GetFVF", 1, dev_GetFVF },
    { "CreateVertexShader", 2, dev_CreateVertexShader }, { "SetVertexShader", 1, dev_SetVertexShader },
    { "GetVertexShader", 1, dev_GetVertexShader }, { "SetVertexShaderConstantF", 3, dev_SetVertexShaderConstantF },
    { "GetVertexShaderConstantF", 3, dev_GetVertexShaderConstantF }, { "SetVertexShaderConstantI", 3, dev_ShaderConstantIB },
    { "GetVertexShaderConstantI", 3, dev_ShaderConstantIB }, { "SetVertexShaderConstantB", 3, dev_ShaderConstantIB },
    { "GetVertexShaderConstantB", 3, dev_ShaderConstantIB }, { "SetStreamSource", 4, dev_SetStreamSource },
    { "GetStreamSource", 4, dev_GetStreamSource }, { "SetStreamSourceFreq", 2, dev_SetStreamSourceFreq },
    { "GetStreamSourceFreq", 2, dev_GetStreamSourceFreq }, { "SetIndices", 1, dev_SetIndices },
    { "GetIndices", 1, dev_GetIndices }, { "CreatePixelShader", 2, dev_CreatePixelShader },
    { "SetPixelShader", 1, dev_SetPixelShader }, { "GetPixelShader", 1, dev_GetPixelShader },
    { "SetPixelShaderConstantF", 3, dev_SetPixelShaderConstantF }, { "GetPixelShaderConstantF", 3, dev_GetPixelShaderConstantF },
    { "SetPixelShaderConstantI", 3, dev_ShaderConstantIB }, { "GetPixelShaderConstantI", 3, dev_ShaderConstantIB },
    { "SetPixelShaderConstantB", 3, dev_ShaderConstantIB }, { "GetPixelShaderConstantB", 3, dev_ShaderConstantIB },
    { "DrawRectPatch", 3, NULL }, { "DrawTriPatch", 3, NULL }, { "DeletePatch", 1, NULL },
    { "CreateQuery", 2, dev_CreateQuery },
};
static ComInterface IDirect3DDevice9_i = { "IDirect3DDevice9", dev_methods, 119, 0 };

/* ------------------------------------------------------------------ common resource methods */

static void out_addref(uint32_t out, uint32_t obj);
W(res_GetDevice) { out_addref(ARG(1), g_device_object); RET(2, g_device_object ? S_OK : D3DERR_INVALIDCALL); }
W(res_SetPrivateData) { RET(5, S_OK); }
W(res_GetPrivateData) { RET(4, 0x88760866u /* D3DERR_NOTFOUND */); }
W(res_FreePrivateData) { RET(2, S_OK); }
W(res_SetPriority) { RET(2, 0); }
W(res_GetPriority) { RET(1, 0); }
W(res_PreLoad) { RET(1, 0); }
W(res_GetType) { ComObj *o = com_get(THIS); RET(1, o ? (uint32_t)o->type : 0); }
W(tex_SetLOD) { RET(2, 0); }
W(tex_GetLOD) { RET(1, 0); }
W(tex_GetLevelCount) { Texture *t = com_data(THIS); RET(1, t ? t->levels : 1); }

#define RESOURCE_METHODS \
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release }, \
    { "GetDevice", 1, res_GetDevice }, { "SetPrivateData", 4, res_SetPrivateData }, \
    { "GetPrivateData", 3, res_GetPrivateData }, { "FreePrivateData", 1, res_FreePrivateData }, \
    { "SetPriority", 1, res_SetPriority }, { "GetPriority", 0, res_GetPriority }, { "PreLoad", 0, res_PreLoad }, \
    { "GetType", 0, res_GetType }
W(tex_SetAutoGenFilterType) { RET(2, S_OK); }
W(tex_GetAutoGenFilterType) { RET(1, 2 /* linear */); }
W(tex_GenerateMipSubLevels) { RET(1, 0); }
#define BASETEXTURE_METHODS RESOURCE_METHODS, { "SetLOD", 1, tex_SetLOD }, { "GetLOD", 0, tex_GetLOD }, \
    { "GetLevelCount", 0, tex_GetLevelCount }, { "SetAutoGenFilterType", 1, tex_SetAutoGenFilterType }, \
    { "GetAutoGenFilterType", 0, tex_GetAutoGenFilterType }, { "GenerateMipSubLevels", 0, tex_GenerateMipSubLevels }

static void write_desc(uint32_t p, uint32_t fmt, uint32_t type, uint32_t usage, uint32_t pool, uint32_t size,
                       uint32_t w, uint32_t h) {
    (void)size;   /* D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality, Width, Height */
    W32(p, fmt); W32(p + 4, type); W32(p + 8, usage); W32(p + 12, pool); W32(p + 16, 0); W32(p + 20, 0);
    W32(p + 24, w); W32(p + 28, h);
}

static uint32_t texture_surface(uint32_t tex_obj, Texture *t, uint32_t face, uint32_t level) {
    if (!t->surfaces[face][level]) {
        Surface *s = calloc(1, sizeof *s);
        s->gpu_id = gpu_new_id();
        s->format = t->format; s->width = t->lw[level]; s->height = t->lh[level]; s->pitch = t->pitch[level];
        s->mem = t->mem[face][level]; s->container = tex_obj; s->face = (int)face; s->level = (int)level;
        s->is_rt = (t->usage & 1) != 0; s->is_depth = (t->usage & 2) != 0;
        t->surfaces[face][level] = com_new(&IDirect3DSurface9_i, RT_SURFACE, s);
    }
    ComObj *o = com_get(t->surfaces[face][level]);
    if (o) o->refs++;
    return t->surfaces[face][level];
}

static void destroy_texture(void *p) {
    Texture *t = p;
    if (g_render->destroy_texture) g_render->destroy_texture(t);
    for (uint32_t f = 0; f < t->faces; f++)
        for (uint32_t l = 0; l < t->levels; l++) {
            heap_free(t->mem[f][l]);
            /* level surfaces handed out earlier point into this memory: detach them */
            Surface *s = t->surfaces[f][l] ? com_data(t->surfaces[f][l]) : NULL;
            if (s) { s->mem = 0; s->container = 0; }
        }
    free(t);
}
static void destroy_surface(void *p) {
    Surface *s = p;
    if (g_render->destroy_surface) g_render->destroy_surface(s);
    if (!s->container && s->mem) heap_free(s->mem);  /* level surfaces don't own their memory */
    if (s->container) {
        Texture *t = com_data(s->container);
        if (t) t->surfaces[s->face][s->level] = 0;
    }
    free(s);
}
static void destroy_buffer(void *p) { Buffer *b = p; heap_free(b->mem); free(b); }

/* IDirect3DTexture9 */
W(tx_GetLevelDesc) {
    Texture *t = com_data(THIS);
    uint32_t l = ARG(1);
    if (!t || l >= t->levels) RET(3, D3DERR_INVALIDCALL);
    write_desc(ARG(2), t->format, RT_SURFACE, t->usage, t->pool, t->size[l], t->lw[l], t->lh[l]);
    RET(3, S_OK);
}
W(tx_GetSurfaceLevel) {
    Texture *t = com_data(THIS);
    if (!t || ARG(1) >= t->levels) RET(3, D3DERR_INVALIDCALL);
    W32(ARG(2), texture_surface(THIS, t, 0, ARG(1)));
    RET(3, S_OK);
}
static void lock_rect(uint32_t out, uint32_t mem, uint32_t pitch, uint32_t fmt, uint32_t rect) {
    uint32_t off = 0;
    if (rect) {
        uint32_t x = R32(rect), y = R32(rect + 4);
        if (format_is_dxt(fmt)) off = (y / 4) * pitch + (x / 4) * (fmt == FMT_DXT1 ? 8 : 16);
        else off = y * pitch + x * format_bpp(fmt) / 8;
    }
    W32(out, pitch);
    W32(out + 4, mem + off);
}
W(tx_LockRect) {
    Texture *t = com_data(THIS);
    uint32_t l = ARG(1);
    if (!t || l >= t->levels) RET(5, D3DERR_INVALIDCALL);
    lock_rect(ARG(2), t->mem[0][l], t->pitch[l], t->format, ARG(3));
    RET(5, S_OK);
}
W(tx_UnlockRect) { Texture *t = com_data(THIS); if (t) t->version++; RET(2, S_OK); }
W(tx_AddDirtyRect) { RET(2, S_OK); }

static const ComMethod tex_methods[] = {
    BASETEXTURE_METHODS, { "GetLevelDesc", 2, tx_GetLevelDesc }, { "GetSurfaceLevel", 2, tx_GetSurfaceLevel },
    { "LockRect", 4, tx_LockRect }, { "UnlockRect", 1, tx_UnlockRect }, { "AddDirtyRect", 1, tx_AddDirtyRect },
};
static ComInterface IDirect3DTexture9_i = { "IDirect3DTexture9", tex_methods, 22, 0, destroy_texture };

/* IDirect3DCubeTexture9 */
W(cube_GetCubeMapSurface) {
    Texture *t = com_data(THIS);
    if (!t || ARG(1) > 5 || ARG(2) >= t->levels) RET(4, D3DERR_INVALIDCALL);
    W32(ARG(3), texture_surface(THIS, t, ARG(1), ARG(2)));
    RET(4, S_OK);
}
W(cube_LockRect) {
    Texture *t = com_data(THIS);
    uint32_t f = ARG(1), l = ARG(2);
    if (!t || f > 5 || l >= t->levels) RET(6, D3DERR_INVALIDCALL);
    lock_rect(ARG(3), t->mem[f][l], t->pitch[l], t->format, ARG(4));
    RET(6, S_OK);
}
W(cube_UnlockRect) { Texture *t = com_data(THIS); if (t) t->version++; RET(3, S_OK); }
W(cube_AddDirtyRect) { RET(3, S_OK); }
static const ComMethod cube_methods[] = {
    BASETEXTURE_METHODS, { "GetLevelDesc", 2, tx_GetLevelDesc }, { "GetCubeMapSurface", 3, cube_GetCubeMapSurface },
    { "LockRect", 5, cube_LockRect }, { "UnlockRect", 2, cube_UnlockRect }, { "AddDirtyRect", 2, cube_AddDirtyRect },
};
static ComInterface IDirect3DCubeTexture9_i = { "IDirect3DCubeTexture9", cube_methods, 22, 0, destroy_texture };

static const ComMethod vol_methods[] = {
    BASETEXTURE_METHODS, { "GetLevelDesc", 2, NULL }, { "GetVolumeLevel", 2, NULL }, { "LockBox", 4, NULL },
    { "UnlockBox", 1, NULL }, { "AddDirtyBox", 1, NULL },
};
static ComInterface IDirect3DVolumeTexture9_i __attribute__((unused)) = { "IDirect3DVolumeTexture9", vol_methods, 22, 0 };

/* IDirect3DSurface9 */
W(surf_GetDevice) { out_addref(ARG(1), g_device_object); RET(2, g_device_object ? S_OK : D3DERR_INVALIDCALL); }
W(surf_GetContainer) {
    Surface *s = com_data(THIS);
    if (!s || !s->container) RET(3, D3DERR_INVALIDCALL);
    out_addref(ARG(2), s->container);
    RET(3, S_OK);
}
W(surf_GetDesc) {
    Surface *s = com_data(THIS);
    if (!s) RET(2, D3DERR_INVALIDCALL);
    write_desc(ARG(1), s->format, RT_SURFACE, s->is_rt ? 1 : s->is_depth ? 2 : 0, 0,
               s->pitch * level_rows(s->format, s->height), s->width, s->height);
    RET(2, S_OK);
}
W(surf_LockRect) {
    Surface *s = com_data(THIS);
    if (!s) RET(4, D3DERR_INVALIDCALL);
    if (g_device && is_gpu_surface(g_device, s) && g_render->readback) g_render->readback(g_device, s);
    if (!s->mem) s->mem = heap_alloc(s->pitch * level_rows(s->format, s->height) + 16, 1);
    lock_rect(ARG(1), s->mem, s->pitch, s->format, ARG(2));
    RET(4, S_OK);
}
W(surf_UnlockRect) {
    Surface *s = com_data(THIS);
    if (s) touch_container(s);
    if (s && s->is_rt && g_device && g_render->upload_surface) g_render->upload_surface(g_device, s);
    RET(1, S_OK);
}
W(surf_GetDC) { RET(3, D3DERR_INVALIDCALL); }
W(surf_ReleaseDC) { RET(2, D3DERR_INVALIDCALL); }
static const ComMethod surf_methods[] = {
    RESOURCE_METHODS, { "GetContainer", 2, surf_GetContainer }, { "GetDesc", 1, surf_GetDesc },
    { "LockRect", 3, surf_LockRect }, { "UnlockRect", 0, surf_UnlockRect }, { "GetDC", 1, surf_GetDC },
    { "ReleaseDC", 1, surf_ReleaseDC },
};
static ComInterface IDirect3DSurface9_i = { "IDirect3DSurface9", surf_methods, 17, 0, destroy_surface };

/* Vertex / index buffers */
W(buf_Lock) {
    Buffer *b = com_data(THIS);
    if (!b) RET(5, D3DERR_INVALIDCALL);
    W32(ARG(3), b->mem + ARG(1));
    RET(5, S_OK);
}
W(buf_Unlock) { Buffer *b = com_data(THIS); if (b) b->version++; RET(1, S_OK); }
W(buf_GetDesc) {
    Buffer *b = com_data(THIS);
    uint32_t p = ARG(1);
    if (!b) RET(2, D3DERR_INVALIDCALL);
    W32(p, b->index ? b->fvf_or_format : 100 /* D3DFMT_VERTEXDATA */);
    W32(p + 4, b->index ? RT_INDEXBUFFER : RT_VERTEXBUFFER);
    W32(p + 8, b->usage); W32(p + 12, b->pool); W32(p + 16, b->size);
    if (!b->index) W32(p + 20, b->fvf_or_format);
    RET(2, S_OK);
}
static const ComMethod vb_methods[] = {
    RESOURCE_METHODS, { "Lock", 4, buf_Lock }, { "Unlock", 0, buf_Unlock }, { "GetDesc", 1, buf_GetDesc },
};
static ComInterface IDirect3DVertexBuffer9_i = { "IDirect3DVertexBuffer9", vb_methods, 14, 0, destroy_buffer };
static ComInterface IDirect3DIndexBuffer9_i = { "IDirect3DIndexBuffer9", vb_methods, 14, 0, destroy_buffer };

/* vertex declarations and shaders: devices objects the game only holds and passes back */
W(shobj_GetDevice) { out_addref(ARG(1), g_device_object); RET(2, S_OK); }
W(shobj_GetFunction) { RET(3, D3DERR_INVALIDCALL); }
static const ComMethod shobj_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetDevice", 1, shobj_GetDevice }, { "GetFunction", 2, shobj_GetFunction },
};
static ComInterface IDirect3DVertexDeclaration9_i = { "IDirect3DVertexDeclaration9", shobj_methods, 5, 0 };
static ComInterface IDirect3DVertexShader9_i = { "IDirect3DVertexShader9", shobj_methods, 5, 0 };
static ComInterface IDirect3DPixelShader9_i = { "IDirect3DPixelShader9", shobj_methods, 5, 0 };

static const ComMethod sc_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "Present", 4, NULL }, { "GetBackBuffer", 3, NULL },
};
static ComInterface IDirect3DSwapChain9_i __attribute__((unused)) = { "IDirect3DSwapChain9", sc_methods, 5, 0 };

/* ------------------------------------------------------------------ headless backend */

/* Headless "vsync": pace Present to 60 Hz like a real display would. RC_NO_PACE=1 skips the wait, so a
   deterministic run (virtual clock) goes as fast as the host allows: a benchmark of the CPU-side work. */
static void hl_present(Device *d) {
    (void)d;
    static double next;
    static int nopace = -1;
    if (nopace < 0) nopace = getenv("RC_NO_PACE") != NULL;
    if (nopace) return;
    double now = host_seconds();
    if (next == 0 || now - next > 0.1) next = now;
    next += 1.0 / 60.0;
    if (next > now) usleep((useconds_t)((next - now) * 1e6));
}
RenderBackend g_render_headless = { "headless", NULL, NULL, NULL, hl_present, NULL };
