/* wbc.c - TransformIT iAES2 block decrypt + user data loader.
 *
 * See wbc.h.  The network in wbc_net.h was recovered by tracing the real
 * TFIT_op_iAES2 code and is exact: it was checked against the object file for
 * all 101 key indices and random blocks (303/303), and against real GTA V
 * Enhanced tables of contents.
 */
#include "wbc.h"
#include "wbc_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TBL_BYTES  ((size_t)WBC_TABLES * 1024)
#define KEY_BYTES  320
#define TBL_HEAD   ((size_t)WBC_KEYWORDS * 4 + 16)

static uint8_t *g_data;              /* tables ++ cooked keys */
static char     g_path[4096];
static int      g_keys;              /* usable cooked keys */
static char     g_loaded_for[4096] = "\1";   /* nothing searched yet */

/* tables are 256 little endian words each */
static const uint32_t *table(int t)
{
    return (const uint32_t *)(const void *)(g_data + (size_t)t * 1024);
}

static const uint32_t *keyword(int index)
{
    return (const uint32_t *)(const void *)(g_data + TBL_BYTES + (size_t)index * KEY_BYTES + 16);
}

int wbc_available(void) { return g_data != NULL && g_keys > 0; }
int wbc_key_count(void) { return g_keys; }
const char *wbc_path(void) { return (g_data && g_keys > 0) ? g_path : NULL; }

const char *wbc_hint(void)
{
    return "PC GTA V (Legacy and Enhanced) needs the Arxan TransformIT iAES2 "
           "tables and cooked keys: put iaes2.bin (118336 bytes = 84x1024 "
           "tables + 101x320 keys) next to the archive or in "
           "~/.config/openvi/ (--wbc FILE also works)";
}

/* ------------------------------------------------------------------ */
/* the network                                                         */
/* ------------------------------------------------------------------ */

int wbc_decrypt(int index, uint8_t *buf, size_t len)
{
    if (!g_data || index < 0 || index >= g_keys) return -1;
    if (len == 0 || (len % WBC_BLOCK) != 0) return -1;

    const uint32_t *kw = keyword(index);
    uint32_t st[WBC_SLOTS];

    for (size_t off = 0; off < len; off += WBC_BLOCK) {
        uint8_t in[WBC_BLOCK];
        memcpy(in, buf + off, WBC_BLOCK);

        for (int i = 0; i < WBC_STEPS; i++) {
            const wbc_step *s = &wbc_steps[i];
            uint32_t acc = (s->keyw >= 0) ? kw[s->keyw] : st[s->src];
            uint32_t x   = s->isin ? in[s->idx] : st[s->idx];
            st[s->dst] = acc ^ table(s->tbl)[(x >> s->sh) & 0xffu];
        }

        for (int j = 0; j < 4; j++) {
            uint32_t v = st[wbc_output[j]];
            buf[off + 4 * j + 0] = (uint8_t)(v & 0xff);
            buf[off + 4 * j + 1] = (uint8_t)((v >> 8) & 0xff);
            buf[off + 4 * j + 2] = (uint8_t)((v >> 16) & 0xff);
            buf[off + 4 * j + 3] = (uint8_t)((v >> 24) & 0xff);
        }
    }
    return 0;
}

int wbc_pick_key(const uint8_t *toc, size_t len, int prefer)
{
    if (!g_data || len < 16) return -1;
    int weak = -1;
    for (int n = 0; n < g_keys + 1; n++) {
        int i = (n == 0) ? prefer : (n - 1);
        if (i < 0 || i >= g_keys) continue;
        if (n > 0 && i == prefer) continue;
        uint8_t blk[16];
        memcpy(blk, toc, 16);
        if (wbc_decrypt(i, blk, 16) != 0) continue;
        if (blk[4] == 0x00 && blk[5] == 0xFF && blk[6] == 0xFF && blk[7] == 0x7F) {
            if (blk[0] || blk[1] || blk[2] || blk[3]) continue;   /* root: 0 */
            return i;
        }
        if (weak < 0 && !blk[0] && !blk[1] && !blk[2] && !blk[3]) weak = i;
    }
    return weak;
}

int wbc_selftest(void)
{
    if (!g_data) return -1;
    /* Every TransformIT table is a bijection byte -> word (an S-box wrapped in
     * invertible linear encodings), so a structure check catches a truncated
     * or mis-ordered data file before it turns into a confusing failure. */
    for (int t = 0; t < WBC_TABLES; t++) {
        const uint32_t *tab = table(t);
        uint32_t slot[512];
        uint8_t  used[512];
        memset(used, 0, sizeof used);
        for (int i = 0; i < 256; i++) {
            uint32_t v = tab[i];
            uint32_t h = (v * 2654435761u) >> 23;
            while (used[h] && slot[h] != v) h = (h + 1) & 511u;
            if (used[h]) return -1;              /* duplicate -> not a table */
            used[h] = 1;
            slot[h] = v;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

static void load_reset(void)
{
    free(g_data);
    g_data = NULL;
    g_keys = 0;
    g_path[0] = 0;
}

static int load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < (long)(TBL_BYTES + KEY_BYTES)) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *d = malloc((size_t)n);
    if (!d) { fclose(f); return -1; }
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return -1; }
    fclose(f);

    int keys = (int)(((size_t)n - TBL_BYTES) / KEY_BYTES);
    if (keys > WBC_NG_KEYS_MAX) keys = WBC_NG_KEYS_MAX;
    g_data = d;
    g_keys = keys;
    if (wbc_selftest() != 0)
        fprintf(stderr, "openiv: %s does not look like a TransformIT table block\n", path);
    return 0;
}

/* look for the data file in the same places the key files live */
static int try_dirs(const char *archive, const char *explicit_file)
{
    static const char *NAMES[] = { "iaes2.bin", "iAES2.bin", "wbc.bin", NULL };
    char dirs[8][4096];
    int nd = 0;
    char tmp[4096];

    if (explicit_file && explicit_file[0] && load_file(explicit_file) == 0) {
        snprintf(g_path, sizeof g_path, "%s", explicit_file);
        return 0;
    }

    if (archive && archive[0]) {
        snprintf(tmp, sizeof tmp, "%s", archive);
        char *s = strrchr(tmp, '/');
        if (s) { *s = 0; snprintf(dirs[nd++], 4096, "%s", tmp); }
    }
    snprintf(dirs[nd++], 4096, ".");

    ssize_t k = readlink("/proc/self/exe", tmp, sizeof tmp - 1);
    if (k > 0) {
        tmp[k] = 0;
        char *s = strrchr(tmp, '/');
        if (s) { *s = 0; snprintf(dirs[nd++], 4096, "%s", tmp); }
    }

    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(dirs[nd++], 4096, "%s/openvi", xdg);
    else if (home) snprintf(dirs[nd++], 4096, "%s/.config/openvi", home);
    if (home) snprintf(dirs[nd++], 4096, "%s/.local/share/openvi", home);

    for (int i = 0; i < nd; i++) {
        for (int j = 0; NAMES[j]; j++) {
            char p[4096];
            snprintf(p, sizeof p, "%.*s/%s", 4000, dirs[i], NAMES[j]);
            if (load_file(p) == 0) {
                snprintf(g_path, sizeof g_path, "%s", p);
                return 0;
            }
        }
    }
    return -1;
}

int wbc_load(const char *archive, const char *explicit_file)
{
    const char *want = archive ? archive : "";
    if (!strcmp(g_loaded_for, want)) return wbc_available() ? 0 : -1;
    snprintf(g_loaded_for, sizeof g_loaded_for, "%s", want);

    load_reset();
    if (try_dirs(archive, explicit_file) != 0) return -1;
    if (g_keys <= 0) { load_reset(); return -1; }
    return 0;
}
