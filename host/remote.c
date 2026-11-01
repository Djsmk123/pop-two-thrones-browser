/* On-demand game files for the browser. The page leaves big files (pop3.bf, the sound archives, movies) as
 * full-size sparse files in OPFS plus a sidecar "<file>.rcremote" and fetches only the parts the game reads,
 * as HTTP range requests against the server that serves the player's own game folder (web/serve.py --game).
 * A chunk bitmap ("<file>.rcchunks", one byte per chunk) records what is already in the file so a
 * later visit does not fetch it again. Natively every function here does nothing. */
#include "win32.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#define CHUNK (1u << 20)      /* 1 MiB */
#define MAX_RUN 4u            /* fetch up to 4 missing chunks (4 MiB) in one request */

typedef struct Remote {
    char path[1024];          /* host path of the sparse file */
    char rel[512];            /* path below the game folder, '/'-separated */
    uint64_t size;
    uint32_t nchunks;
    uint8_t *have;            /* 1 = chunk is in the file */
    int wfd;                  /* writable descriptor on the sparse file */
    int cfd;                  /* the .rcchunks file */
    struct Remote *next;
} Remote;
static Remote *g_remotes;
static pthread_mutex_t g_remote_lock = PTHREAD_MUTEX_INITIALIZER;
extern char g_game_root[1024];

/* synchronous range request (allowed in workers); copies the body to dest, returns its length or -1 */
EM_JS(int, rc_fetch_range, (const char *url, double start, int len, uint8_t *dest), {
    try {
        const x = new XMLHttpRequest();
        x.open('GET', UTF8ToString(url), false);
        x.responseType = 'arraybuffer';
        x.setRequestHeader('Range', 'bytes=' + start + '-' + (start + len - 1));
        x.send();
        if (x.status !== 206 && x.status !== 200) return -1;
        const b = new Uint8Array(x.response);
        if (x.status === 200 && b.length > len) { HEAPU8.set(b.subarray(start, start + len), dest); return len; }   /* server ignored Range */
        HEAPU8.set(b.subarray(0, Math.min(b.length, len)), dest);
        return b.length;
    } catch (e) { return -1; }
});
static const char *base_url(void) {   /* set by the page (RC_GAME_URL); the C environment is shared by every thread */
    const char *u = getenv("RC_GAME_URL");
    return u ? u : "";
}

void *remote_attach(const char *hostpath) {
    char side[1100];
    snprintf(side, sizeof side, "%s.rcremote", hostpath);
    int sfd = open(side, O_RDONLY);
    if (sfd < 0) return NULL;
    char buf[64] = { 0 };
    ssize_t n = read(sfd, buf, sizeof buf - 1);
    close(sfd);
    if (n <= 0 || !base_url()[0]) return NULL;
    pthread_mutex_lock(&g_remote_lock);
    Remote *r;
    for (r = g_remotes; r; r = r->next) if (!strcmp(r->path, hostpath)) break;
    if (!r) {
        r = calloc(1, sizeof *r);
        snprintf(r->path, sizeof r->path, "%s", hostpath);
        size_t gl = strlen(g_game_root);
        snprintf(r->rel, sizeof r->rel, "%s", strncmp(hostpath, g_game_root, gl) == 0 ? hostpath + gl + (hostpath[gl] == '/') : hostpath);
        r->size = strtoull(buf, NULL, 10);
        r->nchunks = (uint32_t)((r->size + CHUNK - 1) / CHUNK);
        r->have = calloc(r->nchunks ? r->nchunks : 1, 1);
        r->wfd = open(hostpath, O_RDWR);
        char cp[1100];
        snprintf(cp, sizeof cp, "%s.rcchunks", hostpath);
        r->cfd = open(cp, O_RDWR | O_CREAT, 0644);
        if (r->cfd >= 0) {
            ssize_t k = pread(r->cfd, r->have, r->nchunks, 0);
            (void)k;
        }
        r->next = g_remotes;
        g_remotes = r;
    }
    pthread_mutex_unlock(&g_remote_lock);
    return r->wfd >= 0 ? r : NULL;
}

static void fetch_run(Remote *r, uint32_t first, uint32_t count) {
    uint64_t start = (uint64_t)first * CHUNK, end = start + (uint64_t)count * CHUNK;
    if (end > r->size) end = r->size;
    int len = (int)(end - start);
    uint8_t *buf = malloc((size_t)len);
    if (!buf) return;
    char url[1600];
    const char *b = base_url();
    size_t o = (size_t)snprintf(url, sizeof url, "%s", b);
    for (const char *p = r->rel; *p && o + 4 < sizeof url; p++) {   /* percent-encode */
        unsigned char ch = (unsigned char)*p;
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '/' || ch == '.' || ch == '-' || ch == '_')
            url[o++] = (char)ch;
        else o += (size_t)snprintf(url + o, 4, "%%%02X", ch);
    }
    url[o] = 0;
    int got = -1;
    for (int attempt = 0; attempt < 4 && got != len; attempt++) got = rc_fetch_range(url, (double)start, len, buf);
    if (got == len && pwrite(r->wfd, buf, (size_t)len, (off_t)start) == len) {
        for (uint32_t i = 0; i < count; i++) {
            r->have[first + i] = 1;
            uint8_t one = 1;
            if (r->cfd >= 0) pwrite(r->cfd, &one, 1, first + i);
        }
    }
    free(buf);
}

void remote_ensure(void *rp, uint64_t off, uint64_t len) {
    Remote *r = rp;
    if (!r || !len || off >= r->size) return;
    if (off + len > r->size) len = r->size - off;
    uint32_t a = (uint32_t)(off / CHUNK), z = (uint32_t)((off + len - 1) / CHUNK);
    for (uint32_t i = a; i <= z; ) {
        if (__atomic_load_n(&r->have[i], __ATOMIC_ACQUIRE)) { i++; continue; }
        uint32_t n = 1;   /* fetch this chunk and read ahead: the run goes past z when the read was sequential */
        uint32_t max = (i == 0 || r->have[i - 1]) ? MAX_RUN : 2;   /* read ahead further when the access looks sequential */
        while (n < max && i + n < r->nchunks && !__atomic_load_n(&r->have[i + n], __ATOMIC_ACQUIRE)) n++;
        pthread_mutex_lock(&g_remote_lock);   /* one request at a time: the tunnel is the bottleneck anyway */
        if (!r->have[i]) fetch_run(r, i, n);
        pthread_mutex_unlock(&g_remote_lock);
        if (!r->have[i]) return;   /* failed: let the read return what the file has (zeros) */
        i += n;
    }
}
#else
void *remote_attach(const char *hostpath) { (void)hostpath; return NULL; }
void remote_ensure(void *rp, uint64_t off, uint64_t len) { (void)rp; (void)off; (void)len; }
#endif
