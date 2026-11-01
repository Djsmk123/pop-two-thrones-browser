/* English text for a Spanish edition. The game keeps its dialog and menu strings in POPData.bf: an archive whose text entry
 * is an ini-style file ("[Section]" / "Key=value", lines end CRLF, "||" breaks a line in a message). This copy of the game
 * ships only the Spanish text, so the host writes a translated copy of that entry (values replaced from lang_en.h by
 * "Section.Key", everything else kept) and the game opens that instead. The archive header holds the entry's size at
 * offset 0x2d, which is updated. Anything unexpected leaves the original file alone. */
#include "win32.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lang_en.h"
static const struct { const char *key, *val; } k_en[] = { LANG_EN_TABLE };

static const char *lookup(const char *section, const char *key) {
    char full[160];
    snprintf(full, sizeof full, "%s.%s", section, key);
    for (int i = 0; k_en[i].key; i++) if (!strcmp(k_en[i].key, full)) return k_en[i].val;
    return NULL;
}

const char *localize_popdata(const char *orig, const char *out_path) {
    FILE *f = fopen(orig, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    buf[n] = 0;
    const char *marker = "; General information";
    uint8_t *text = (uint8_t *)memmem(buf, (size_t)n, marker, strlen(marker));
    uint32_t start = text ? (uint32_t)(text - buf) : 0, declared = 0;
    if (n > 0x31) memcpy(&declared, buf + 0x2d, 4);
    if (!text || start < 0x30 || declared != (uint32_t)n - start) { free(buf); return NULL; }   /* not the layout we know */
    char *res = malloc((size_t)n * 2 + 4096);
    size_t rl = start;
    memcpy(res, buf, start);
    char section[96] = "";
    for (uint8_t *p = text; p < buf + n; ) {
        uint8_t *e = memchr(p, '\n', (size_t)(buf + n - p));
        size_t len = e ? (size_t)(e - p) + 1 : (size_t)(buf + n - p);
        char line[2048];
        size_t ll = len < sizeof line - 1 ? len : sizeof line - 1;
        memcpy(line, p, ll); line[ll] = 0;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        int done = 0;
        if (*s == '[') { char *r = strchr(s, ']'); if (r) { snprintf(section, sizeof section, "%.*s", (int)(r - s - 1), s + 1); } }
        else if (*s && *s != ';' && section[0]) {
            char *eq = strchr(s, '=');
            if (eq) {
                char key[96]; size_t kl = (size_t)(eq - s);
                while (kl && (s[kl - 1] == ' ' || s[kl - 1] == '\t')) kl--;
                snprintf(key, sizeof key, "%.*s", (int)kl, s);
                const char *v = lookup(section, key);
                if (v) {
                    const char *nl = (ll >= 2 && line[ll - 2] == '\r') ? "\r\n" : "\n";
                    rl += (size_t)snprintf(res + rl, 2048, "%.*s=%s%s", (int)kl, s, v, nl);
                    done = 1;
                }
            }
        }
        if (!done) { memcpy(res + rl, p, len); rl += len; }
        p += len;
    }
    uint32_t newsize = (uint32_t)(rl - start);
    memcpy(res + 0x2d, &newsize, 4);
    FILE *o = fopen(out_path, "wb");
    if (!o) { free(buf); free(res); return NULL; }
    fwrite(res, 1, rl, o);
    fclose(o);
    free(buf); free(res);
    return out_path;
}
