/* DirectSound: buffers live in guest memory; a mixer thread advances play cursors in real time, fires
 * position notifications and mixes playing buffers to stereo float. Output goes to CoreAudio
 * (macOS; RC_NO_AUDIO=1 to mute). */
#include "com.h"

#include <math.h>
#include <pthread.h>
#include <unistd.h>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif

#define W(name) static void name(CPU *c)
#define DSERR_NODRIVER 0x88780078u
#define DSERR_INVALIDPARAM 0x80070057u
#define MIX_RATE 44100

void kernel_set_event(uint32_t h);  /* kernel32.c */

typedef struct SBuf {
    int primary;
    uint32_t flags, size, mem;
    uint16_t channels, bits;
    uint32_t rate, freq;          /* format rate, current playback frequency */
    int32_t volume, pan;          /* hundredths of a dB */
    int playing, looping;
    float gl_cur, gr_cur;         /* smoothed channel gains: a jump in volume, pan or 3D position is a click */
    int fade;                     /* frames left of the fade-out after Stop (an abrupt cut is a click) */
    double pos;                   /* play cursor in frames (fractional for resampling) */
    uint32_t notify_n, notify_off[64], notify_ev[64];
    int is3d;                     /* DSBCAPS_CTRL3D and not DS3DMODE_DISABLE */
    uint32_t mode3d;              /* DS3DMODE_NORMAL 0, HEADRELATIVE 1, DISABLE 2 */
    float pos3[3], mind, maxd;    /* 3D position and distance range */
    struct SBuf *next;
} SBuf;

static SBuf *g_bufs;
static pthread_mutex_t g_snd_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_mixer_started;

static ComInterface IDirectSound_i, IDirectSoundBuffer_i, IDirectSoundNotify_i, IDirectSound3DBuffer_i,
    IDirectSound3DListener_i;

/* Frames already mixed but not yet audible (the output queue). The play cursor the game sees must lag by that much: it
   refills the part of a streaming buffer behind the cursor, and with the cursor ahead of the speakers it overwrote sound
   that had not played yet (crackling). */
static volatile uint32_t g_out_latency = 0;

/* 3D listener (one per primary buffer; the game has one) */
static float g_lpos[3], g_lfront[3] = { 0, 0, 1 }, g_ltop[3] = { 0, 1, 0 }, g_rolloff = 1.f, g_distfactor = 1.f;
static float rf(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }

/* distance attenuation (DirectSound: inverse distance beyond the minimum, rolloff-scaled) and left/right pan */
static void spatial(const SBuf *b, float *gain, float *pan) {
    *gain = 1.f; *pan = 0.f;
    if (!b->is3d) return;
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = (b->pos3[i] - (b->mode3d == 1 ? 0.f : g_lpos[i])) * g_distfactor;
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float mn = b->mind > 0.f ? b->mind * g_distfactor : g_distfactor, mx = b->maxd > 0.f ? b->maxd * g_distfactor : 1e9f;
    if (dist > mx) dist = mx;
    if (dist > mn) *gain = mn / (mn + g_rolloff * (dist - mn));
    if (dist > 1e-4f) {
        const float *f = g_lfront, *t = g_ltop;   /* left-handed: right = top x front */
        float right[3] = { t[1] * f[2] - t[2] * f[1], t[2] * f[0] - t[0] * f[2], t[0] * f[1] - t[1] * f[0] };
        *pan = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / dist;
        if (*pan > 1.f) *pan = 1.f; else if (*pan < -1.f) *pan = -1.f;
    }
}

/* ------------------------------------------------------------------ mixing */

static uint32_t frame_bytes(SBuf *b) { return (uint32_t)b->channels * b->bits / 8; }

static void sample_at(SBuf *b, uint32_t frame, float *l, float *r) {
    const uint8_t *p = HP(b->mem + frame * frame_bytes(b));
    if (b->bits == 16) {
        int16_t a = (int16_t)(p[0] | p[1] << 8);
        int16_t z = b->channels == 2 ? (int16_t)(p[2] | p[3] << 8) : a;
        *l = a / 32768.f; *r = z / 32768.f;
    } else {
        *l = (p[0] - 128) / 128.f;
        *r = b->channels == 2 ? (p[1] - 128) / 128.f : *l;
    }
}

/* Advances a buffer by `out_frames` output frames, mixing into `mix` (may be NULL), firing notifications. */
#define FADE_FRAMES 192
static void advance(SBuf *b, float *mix, int out_frames) {
    if (!b->playing && b->fade > 0 && mix && !b->primary && b->mem) {   /* tail of a stopped buffer */
        uint32_t fb = frame_bytes(b), nf = fb ? b->size / fb : 0;
        double step = (double)(b->freq ? b->freq : b->rate) / MIX_RATE;
        for (int i = 0; i < out_frames && b->fade > 0 && nf; i++, b->fade--) {
            float l, r, m = (float)b->fade / FADE_FRAMES;
            sample_at(b, (uint32_t)b->pos < nf ? (uint32_t)b->pos : nf - 1, &l, &r);
            mix[2 * i] += l * b->gl_cur * m; mix[2 * i + 1] += r * b->gr_cur * m;
            b->pos += step; if (b->pos >= nf) b->pos -= nf;
        }
        return;
    }
    if (!b->playing || b->primary || !b->mem) return;
    uint32_t fb = frame_bytes(b);
    uint32_t nframes = fb ? b->size / fb : 0;
    if (!nframes) return;
    double step = (double)(b->freq ? b->freq : b->rate) / MIX_RATE;
    float gain = b->volume <= -10000 ? 0.f : powf(10.f, b->volume / 2000.f);
    float sg, sp;
    spatial(b, &sg, &sp);
    gain *= sg;
    float gl = gain * (b->pan > 0 ? powf(10.f, -b->pan / 2000.f) : 1.f) * (sp > 0 ? 1.f - sp : 1.f);
    float gr = gain * (b->pan < 0 ? powf(10.f, b->pan / 2000.f) : 1.f) * (sp < 0 ? 1.f + sp : 1.f);
    for (int i = 0; i < out_frames && b->playing; i++) {
        uint32_t f = (uint32_t)b->pos;
        if (mix) {
            float l, r, l2, r2;
            sample_at(b, f, &l, &r);
            uint32_t f2 = f + 1 < nframes ? f + 1 : (b->looping ? 0 : f);
            sample_at(b, f2, &l2, &r2);
            float fr = (float)(b->pos - f);   /* linear interpolation: no zero-order-hold aliasing */
            l += (l2 - l) * fr; r += (r2 - r) * fr;
            b->gl_cur += (gl - b->gl_cur) * (1.f / 96.f);   /* ~2 ms slew */
            b->gr_cur += (gr - b->gr_cur) * (1.f / 96.f);
            mix[2 * i] += l * b->gl_cur;
            mix[2 * i + 1] += r * b->gr_cur;
        }
        double np = b->pos + step;
        uint32_t ob = f * fb, nb = (uint32_t)np * fb;
        for (uint32_t k = 0; k < b->notify_n; k++) {
            uint32_t off = b->notify_off[k];
            if (off == 0xFFFFFFFFu) continue;  /* DSBPN_OFFSETSTOP */
            if ((ob <= off && off < nb) || (nb >= b->size && off >= ob)) kernel_set_event(b->notify_ev[k]);
        }
        b->pos = np;
        if (b->pos >= nframes) {
            if (b->looping) b->pos -= nframes;
            else {
                b->playing = 0;
                b->pos = 0;
                for (uint32_t k = 0; k < b->notify_n; k++)
                    if (b->notify_off[k] == 0xFFFFFFFFu) kernel_set_event(b->notify_ev[k]);
            }
        }
    }
}

static void mix_frames(float *out, int frames) {
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    pthread_mutex_lock(&g_snd_lock);
    for (SBuf *b = g_bufs; b; b = b->next) advance(b, out, frames);
    pthread_mutex_unlock(&g_snd_lock);
    for (int i = 0; i < 2 * frames; i++) {   /* soft limiter: many loud buffers add up, hard clipping sounds like blasting */
        float x = out[i] * 0.8f, a = fabsf(x);
        if (a > 0.7f) x = copysignf(0.7f + 0.3f * tanhf((a - 0.7f) / 0.3f), x);
        out[i] = x;
    }
    static FILE *dump; static int dump_on = -1;   /* RC_DUMP_AUDIO=file: raw stereo float32 at 44.1 kHz, what the speakers get */
    if (dump_on < 0) { dump_on = getenv("RC_DUMP_AUDIO") != NULL; if (dump_on) dump = fopen(getenv("RC_DUMP_AUDIO"), "wb"); }
    if (dump) fwrite(out, sizeof(float), 2 * (size_t)frames, dump);
}

#define AQ_FRAMES 1024
#define AQ_BUFFERS 4
#ifdef __APPLE__
static void aq_callback(void *ud, AudioQueueRef q, AudioQueueBufferRef buf) {
    (void)ud;
    int frames = (int)(buf->mAudioDataBytesCapacity / 8);
    mix_frames((float *)buf->mAudioData, frames);
    buf->mAudioDataByteSize = (UInt32)frames * 8;
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static int start_coreaudio(void) {
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = MIX_RATE; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 8; f.mFramesPerPacket = 1; f.mBytesPerFrame = 8; f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    AudioQueueRef q;
    if (AudioQueueNewOutput(&f, aq_callback, NULL, NULL, NULL, 0, &q)) return 0;
    for (int i = 0; i < AQ_BUFFERS; i++) {
        AudioQueueBufferRef b;
        AudioQueueAllocateBuffer(q, AQ_FRAMES * 8, &b);  /* ~23 ms per buffer; small ones underran when the game was busy */
        aq_callback(NULL, q, b);
    }
    g_out_latency = AQ_BUFFERS * AQ_FRAMES - AQ_FRAMES / 2;   /* queued, minus the buffer being refilled right now */
    return AudioQueueStart(q, NULL) == 0;
}
#endif

#ifdef __EMSCRIPTEN__
/* Browser: mixed audio goes into a ring in shared memory that the page's AudioWorklet reads
   (web/loader.js). The ring holds stereo float frames; w and r count frames written and read. */
#include <emscripten.h>
#define RING_FRAMES 16384u   /* ~370 ms at 44.1 kHz */
#define RING_TARGET 3528u    /* keep ~80 ms queued: enough to ride out scheduling jitter, short enough to stay in sync */
static struct { volatile uint32_t w, r, rate, frames; float data[RING_FRAMES * 2]; } g_ring = { 0, 0, MIX_RATE, RING_FRAMES, { 0 } };
EMSCRIPTEN_KEEPALIVE uint32_t rc_audio_ring(void) { return (uint32_t)(uintptr_t)&g_ring; }

static void ring_write(const float *f, int frames) {
    uint32_t w = __atomic_load_n(&g_ring.w, __ATOMIC_ACQUIRE), r = __atomic_load_n(&g_ring.r, __ATOMIC_ACQUIRE);
    if (w - r + (uint32_t)frames > RING_FRAMES) return;
    g_out_latency = w - r;   /* frames waiting in the ring */
    for (int i = 0; i < frames; i++) {
        uint32_t k = (w + (uint32_t)i) % RING_FRAMES;
        g_ring.data[2 * k] = f[2 * i]; g_ring.data[2 * k + 1] = f[2 * i + 1];
    }
    __atomic_store_n(&g_ring.w, w + (uint32_t)frames, __ATOMIC_RELEASE);
}
static void ring_write_mix(float *tmp) { mix_frames(tmp, 441); ring_write(tmp, 441); }
#endif

/* Without an audio device, a thread still advances cursors in real time so streaming keeps working
   (in the browser it also feeds the AudioWorklet's ring). Paced by the clock, not by sleeps alone. */
static void *g_mixer_sched;   /* deterministic mode: mixes in virtual time, as a scheduled participant */
static void *silent_mixer(void *arg) {
    (void)arg;
    float tmp[2 * 441];
    if (g_mixer_sched) {
        sched_enter(g_mixer_sched);
        for (uint64_t next = sched_now_us();; ) {
            mix_frames(tmp, 441);             /* 10 ms of game time */
#ifdef __EMSCRIPTEN__
            /* the page plays it; the ring drops what doesn't fit and never touches game state */
            if (!getenv("RC_NO_AUDIO")) ring_write(tmp, 441);
#endif
            next += 10000;
            sched_wait(NULL, NULL, next, NULL);
        }
    }
#ifdef __EMSCRIPTEN__
    /* The page's audio clock drains the ring; this thread tops it up to RING_TARGET. Pacing on the host clock
       instead drifts against the audio clock, and every drift step is an underrun (silence: a click) or a drop. */
    if (!getenv("RC_NO_AUDIO")) {
        for (;;) {
            uint32_t w = __atomic_load_n(&g_ring.w, __ATOMIC_ACQUIRE), r = __atomic_load_n(&g_ring.r, __ATOMIC_ACQUIRE);
            if (w - r < RING_TARGET) ring_write_mix(tmp);
            else usleep(2000);
        }
    }
#endif
    double next = host_seconds();
    for (unsigned n = 0;; n++) {
        mix_frames(tmp, 441);                 /* 10 ms */
        if (n % 500 == 0) LOG("[dsound] mixer alive (%u)\n", n);
        next += 441.0 / MIX_RATE;
        double now = host_seconds();
        if (next < now - 0.2) next = now;     /* fell far behind: don't try to catch up */
        if (next > now) usleep((useconds_t)((next - now) * 1e6));
    }
    return NULL;
}

static void start_mixer(void) {
    if (g_mixer_started) return;
    g_mixer_started = 1;
    g_mixer_sched = sched_new("mixer", NULL, NULL);
#ifdef __APPLE__
    if (!g_mixer_sched && !getenv("RC_NO_AUDIO") && start_coreaudio()) { LOG("[dsound] CoreAudio output started\n"); return; }
#endif
    pthread_t t;
    pthread_create(&t, NULL, silent_mixer, NULL);
    pthread_detach(t);
}

/* ------------------------------------------------------------------ IDirectSoundBuffer */

static SBuf *buf(CPU *c) { return com_data(THIS); }

W(b_QueryInterface) {
    static const uint8_t IID_Notify[16] = { 0x83, 0x07, 0x21, 0xB0, 0xCD, 0x89, 0xD0, 0x11, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16 };
    static const uint8_t IID_3DBuffer[16] = { 0x86, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };
    static const uint8_t IID_3DListener[16] = { 0x84, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };
    const uint8_t *iid = HP(ARG(1));
    uint32_t out = THIS;
    if (!memcmp(iid, IID_Notify, 16)) out = com_new(&IDirectSoundNotify_i, 0, buf(c));
    else if (!memcmp(iid, IID_3DBuffer, 16)) out = com_new(&IDirectSound3DBuffer_i, 0, buf(c));
    else if (!memcmp(iid, IID_3DListener, 16)) out = com_new(&IDirectSound3DListener_i, 0, buf(c));
    W32(ARG(2), out);
    RET(3, S_OK);
}
W(b_GetCaps) {
    SBuf *b = buf(c);
    uint32_t p = ARG(1);
    W32(p + 4, b->flags); W32(p + 8, b->size); W32(p + 12, 0); W32(p + 16, 0);
    RET(2, S_OK);
}
W(b_GetCurrentPosition) {
    SBuf *b = buf(c);
    pthread_mutex_lock(&g_snd_lock);
    double ppos = b->pos;
    {   /* what is audible now is what was mixed g_out_latency output frames ago */
        double lag = (double)g_out_latency * (double)(b->freq ? b->freq : b->rate) / MIX_RATE;
        uint32_t nf = frame_bytes(b) ? b->size / frame_bytes(b) : 0;
        ppos -= lag;
        if (ppos < 0) ppos = b->looping && nf ? ppos + nf * ceil(-ppos / nf) : 0;
    }
    uint32_t play = b->primary ? 0 : (uint32_t)ppos * frame_bytes(b);
    pthread_mutex_unlock(&g_snd_lock);
    uint32_t write = b->size ? (play + 4 * frame_bytes(b) * 64) % b->size : 0;  /* write cursor a little ahead */
    { static unsigned n; static int on = -1; if (on < 0) on = getenv("RC_LOG_DSPOS") != NULL; if (on && n++ % 300 == 0) LOG("[dsound] pos buf %08x play %u write %u size %u playing %d\n", THIS, play, write, b->size, b->playing); }
    if (ARG(1)) W32(ARG(1), play);
    if (ARG(2)) W32(ARG(2), write);
    RET(3, S_OK);
}
W(b_GetFormat) {
    SBuf *b = buf(c);
    if (ARG(1)) {
        uint32_t p = ARG(1);
        W16(p, 1); W16(p + 2, b->channels); W32(p + 4, b->rate); W32(p + 8, b->rate * frame_bytes(b));
        W16(p + 12, (uint16_t)frame_bytes(b)); W16(p + 14, b->bits);
        if (ARG(2) >= 18) W16(p + 16, 0);
    }
    if (ARG(3)) W32(ARG(3), 18);
    RET(4, S_OK);
}
W(b_GetVolume) { W32(ARG(1), (uint32_t)buf(c)->volume); RET(2, S_OK); }
W(b_GetPan) { W32(ARG(1), (uint32_t)buf(c)->pan); RET(2, S_OK); }
W(b_GetFrequency) { W32(ARG(1), buf(c)->freq); RET(2, S_OK); }
W(b_GetStatus) {
    SBuf *b = buf(c);
    W32(ARG(1), b->playing ? (1u | (b->looping ? 4u : 0u)) : 0u);
    RET(2, S_OK);
}
W(b_Initialize) { RET(3, S_OK); }
W(b_Lock) {
    SBuf *b = buf(c);
    uint32_t off = ARG(1), n = ARG(2), flags = ARG(7);
    if (flags & 2 /* DSBLOCK_ENTIREBUFFER */) { off = 0; n = b->size; }
    if (flags & 1 /* DSBLOCK_FROMWRITECURSOR */) off = 0;
    if (!b->mem || n > b->size) RET(8, DSERR_INVALIDPARAM);
    off %= b->size;
    uint32_t first = off + n > b->size ? b->size - off : n;
    W32(ARG(3), b->mem + off); W32(ARG(4), first);
    if (ARG(5)) W32(ARG(5), first < n ? b->mem : 0);
    if (ARG(6)) W32(ARG(6), n - first);
    RET(8, S_OK);
}
W(b_Play) {
    SBuf *b = buf(c);
    start_mixer();
    pthread_mutex_lock(&g_snd_lock);
    if (!b->playing) { b->gl_cur = b->gr_cur = 0.f; b->fade = 0; }
    b->playing = 1;
    b->looping = (ARG(3) & 1) != 0;
    pthread_mutex_unlock(&g_snd_lock);
    RET(4, S_OK);
}
W(b_SetCurrentPosition) {
    SBuf *b = buf(c);
    pthread_mutex_lock(&g_snd_lock);
    if (frame_bytes(b)) b->pos = ARG(1) / frame_bytes(b);
    pthread_mutex_unlock(&g_snd_lock);
    RET(2, S_OK);
}
W(b_SetFormat) {
    SBuf *b = buf(c);
    uint32_t p = ARG(1);
    LOG("[dsound] primary format: %u Hz, %u ch, %u bit\n", R32(p + 4), R16(p + 2), R16(p + 14));
    if (b->primary) { b->channels = R16(p + 2); b->rate = R32(p + 4); b->bits = R16(p + 14); }
    RET(2, S_OK);
}
W(b_SetVolume) { buf(c)->volume = (int32_t)ARG(1); RET(2, S_OK); }
W(b_SetPan) { buf(c)->pan = (int32_t)ARG(1); RET(2, S_OK); }
W(b_SetFrequency) { SBuf *b = buf(c); b->freq = ARG(1) ? ARG(1) : b->rate; RET(2, S_OK); }
W(b_Stop) { SBuf *b = buf(c); pthread_mutex_lock(&g_snd_lock); if (b->playing) b->fade = FADE_FRAMES; b->playing = 0; pthread_mutex_unlock(&g_snd_lock); RET(1, S_OK); }
W(b_Unlock) { RET(5, S_OK); }
W(b_Restore) { RET(1, S_OK); }

static const ComMethod buf_methods[] = {
    { "QueryInterface", 2, b_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetCaps", 1, b_GetCaps }, { "GetCurrentPosition", 2, b_GetCurrentPosition }, { "GetFormat", 3, b_GetFormat },
    { "GetVolume", 1, b_GetVolume }, { "GetPan", 1, b_GetPan }, { "GetFrequency", 1, b_GetFrequency },
    { "GetStatus", 1, b_GetStatus }, { "Initialize", 2, b_Initialize }, { "Lock", 7, b_Lock }, { "Play", 3, b_Play },
    { "SetCurrentPosition", 1, b_SetCurrentPosition }, { "SetFormat", 1, b_SetFormat },
    { "SetVolume", 1, b_SetVolume }, { "SetPan", 1, b_SetPan }, { "SetFrequency", 1, b_SetFrequency },
    { "Stop", 0, b_Stop }, { "Unlock", 4, b_Unlock }, { "Restore", 0, b_Restore },
    /* IDirectSoundBuffer8 */
    { "SetFX", 3, NULL }, { "AcquireResources", 3, NULL }, { "GetObjectInPath", 4, NULL },
};
static ComInterface IDirectSoundBuffer_i = { "IDirectSoundBuffer", buf_methods, 24, 0 };

/* IDirectSoundNotify */
W(n_SetNotificationPositions) {
    SBuf *b = com_data(THIS);
    uint32_t n = ARG(1), p = ARG(2);
    pthread_mutex_lock(&g_snd_lock);
    b->notify_n = n > 64 ? 64 : n;
    for (uint32_t i = 0; i < b->notify_n; i++) { b->notify_off[i] = R32(p + 8 * i); b->notify_ev[i] = R32(p + 8 * i + 4); }
    pthread_mutex_unlock(&g_snd_lock);
    LOG("[dsound] %u notification positions\n", n);
    RET(3, S_OK);
}
static const ComMethod notify_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "SetNotificationPositions", 2, n_SetNotificationPositions },
};
static ComInterface IDirectSoundNotify_i = { "IDirectSoundNotify", notify_methods, 4, 0 };

/* 3D interfaces: accepted, ignored for now (sound stays 2D) */
W(b3_SetPosition) { SBuf *b = com_data(THIS); for (int i = 0; i < 3; i++) b->pos3[i] = rf(ARG(1 + i)); RET(5, S_OK); }
W(b3_SetMinDistance) { SBuf *b = com_data(THIS); b->mind = rf(ARG(1)); RET(3, S_OK); }
W(b3_SetMaxDistance) { SBuf *b = com_data(THIS); b->maxd = rf(ARG(1)); RET(3, S_OK); }
W(b3_SetMode) { SBuf *b = com_data(THIS); b->mode3d = ARG(1); b->is3d = ARG(1) != 2; RET(3, S_OK); }
W(b3_SetAllParameters) {
    SBuf *b = com_data(THIS); uint32_t p = ARG(1);
    for (int i = 0; i < 3; i++) b->pos3[i] = rf(R32(p + 4 + 4 * i));
    b->mind = rf(R32(p + 52)); b->maxd = rf(R32(p + 56)); b->mode3d = R32(p + 60); b->is3d = b->mode3d != 2;
    RET(3, S_OK);
}
W(l3_SetPosition) { for (int i = 0; i < 3; i++) g_lpos[i] = rf(ARG(1 + i)); RET(5, S_OK); }
W(l3_SetOrientation) { for (int i = 0; i < 3; i++) { g_lfront[i] = rf(ARG(1 + i)); g_ltop[i] = rf(ARG(4 + i)); } RET(8, S_OK); }
W(l3_SetRolloffFactor) { g_rolloff = rf(ARG(1)); RET(3, S_OK); }
W(l3_SetDistanceFactor) { g_distfactor = rf(ARG(1)) > 0.f ? rf(ARG(1)) : 1.f; RET(3, S_OK); }
W(l3_SetAllParameters) {
    uint32_t p = ARG(1);
    for (int i = 0; i < 3; i++) { g_lpos[i] = rf(R32(p + 4 + 4 * i)); g_lfront[i] = rf(R32(p + 28 + 4 * i)); g_ltop[i] = rf(R32(p + 40 + 4 * i)); }
    g_distfactor = rf(R32(p + 52)) > 0.f ? rf(R32(p + 52)) : 1.f; g_rolloff = rf(R32(p + 56));
    RET(3, S_OK);
}
static const ComMethod b3d_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetAllParameters", 1, NULL }, { "GetConeAngles", 2, NULL }, { "GetConeOrientation", 1, NULL },
    { "GetConeOutsideVolume", 1, NULL }, { "GetMaxDistance", 1, NULL }, { "GetMinDistance", 1, NULL },
    { "GetMode", 1, NULL }, { "GetPosition", 1, NULL }, { "GetVelocity", 1, NULL },
    { "SetAllParameters", 2, b3_SetAllParameters }, { "SetConeAngles", 3, NULL }, { "SetConeOrientation", 4, NULL },
    { "SetConeOutsideVolume", 2, NULL }, { "SetMaxDistance", 2, b3_SetMaxDistance }, { "SetMinDistance", 2, b3_SetMinDistance },
    { "SetMode", 2, b3_SetMode }, { "SetPosition", 4, b3_SetPosition }, { "SetVelocity", 4, NULL },
};
static ComInterface IDirectSound3DBuffer_i = { "IDirectSound3DBuffer", b3d_methods, 21, 0 };
static const ComMethod l3d_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetAllParameters", 1, NULL }, { "GetDistanceFactor", 1, NULL }, { "GetDopplerFactor", 1, NULL },
    { "GetOrientation", 2, NULL }, { "GetPosition", 1, NULL }, { "GetRolloffFactor", 1, NULL },
    { "GetVelocity", 1, NULL }, { "SetAllParameters", 2, l3_SetAllParameters }, { "SetDistanceFactor", 2, l3_SetDistanceFactor },
    { "SetDopplerFactor", 2, NULL }, { "SetOrientation", 7, l3_SetOrientation }, { "SetPosition", 4, l3_SetPosition },
    { "SetRolloffFactor", 2, l3_SetRolloffFactor }, { "SetVelocity", 4, NULL }, { "CommitDeferredSettings", 0, NULL },
};
static ComInterface IDirectSound3DListener_i = { "IDirectSound3DListener", l3d_methods, 18, 0 };

/* ------------------------------------------------------------------ IDirectSound */

static uint32_t new_buffer(uint32_t desc) {
    SBuf *b = calloc(1, sizeof *b);
    b->flags = R32(desc + 4);
    b->primary = (b->flags & 1) != 0;
    b->size = R32(desc + 8);
    uint32_t wfx = R32(desc + 16);
    b->channels = 2; b->bits = 16; b->rate = MIX_RATE;
    if (wfx) { b->channels = R16(wfx + 2); b->rate = R32(wfx + 4); b->bits = R16(wfx + 14); }
    b->freq = b->rate;
    b->is3d = !b->primary && (b->flags & 0x10) != 0; b->mind = 1.f; b->maxd = 1e9f;
    if (!b->primary && b->size) b->mem = heap_alloc(b->size + 16, 1);
    pthread_mutex_lock(&g_snd_lock);
    b->next = g_bufs;
    g_bufs = b;
    pthread_mutex_unlock(&g_snd_lock);
    LOG("[dsound] CreateSoundBuffer: %s %u bytes, %u Hz %u ch %u bit, flags %x\n", b->primary ? "primary" : "secondary",
        b->size, b->rate, b->channels, b->bits, b->flags);
    return com_new(&IDirectSoundBuffer_i, 0, b);
}

W(ds_CreateSoundBuffer) { W32(ARG(2), new_buffer(ARG(1))); RET(4, S_OK); }
W(ds_GetCaps) {
    uint32_t p = ARG(1), sz = R32(p);
    memset(HP(p + 4), 0, sz > 4 ? sz - 4 : 0);
    W32(p + 4, 0x0F5F);         /* DSCAPS: primary/secondary mono/stereo 8/16 bit, continuous rate, certified */
    W32(p + 8, 100); W32(p + 12, 100000);
    W32(p + 20, 64); W32(p + 24, 64); W32(p + 28, 64);   /* max hw mixing buffers */
    RET(2, S_OK);
}
W(ds_DuplicateSoundBuffer) {
    SBuf *src = com_data(ARG(1));
    if (!src) RET(3, DSERR_INVALIDPARAM);
    SBuf *b = calloc(1, sizeof *b);
    *b = *src;
    b->playing = 0; b->pos = 0; b->notify_n = 0;
    pthread_mutex_lock(&g_snd_lock);
    b->next = g_bufs; g_bufs = b;
    pthread_mutex_unlock(&g_snd_lock);
    W32(ARG(2), com_new(&IDirectSoundBuffer_i, 0, b));
    RET(3, S_OK);
}
W(ds_SetCooperativeLevel) { RET(3, S_OK); }
W(ds_Compact) { RET(1, S_OK); }
W(ds_GetSpeakerConfig) { W32(ARG(1), 4 /* stereo */); RET(2, S_OK); }
W(ds_SetSpeakerConfig) { RET(2, S_OK); }
W(ds_Initialize) { RET(2, S_OK); }

static const ComMethod ds_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "CreateSoundBuffer", 3, ds_CreateSoundBuffer }, { "GetCaps", 1, ds_GetCaps },
    { "DuplicateSoundBuffer", 2, ds_DuplicateSoundBuffer }, { "SetCooperativeLevel", 2, ds_SetCooperativeLevel },
    { "Compact", 0, ds_Compact }, { "GetSpeakerConfig", 1, ds_GetSpeakerConfig },
    { "SetSpeakerConfig", 1, ds_SetSpeakerConfig }, { "Initialize", 1, ds_Initialize },
};
static ComInterface IDirectSound_i = { "IDirectSound", ds_methods, 11, 0 };

void w_DirectSoundCreate(CPU *c) {
    LOG("[dsound] DirectSoundCreate\n");
    W32(ARG(1), com_new(&IDirectSound_i, 0, NULL));
    RET(3, S_OK);
}

/* DSOUND ordinal 6 is DirectSoundCaptureCreate: no capture device */
void w_DirectSoundCaptureCreate(CPU *c) { if (ARG(1)) W32(ARG(1), 0); RET(3, DSERR_NODRIVER); }
