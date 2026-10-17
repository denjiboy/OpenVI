/* rpf.c - RPF7 reader implementation. */
#include "rpf.h"
#include "wbc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include <openssl/evp.h>
#include <zlib.h>

/* ------------------------------------------------------------------ */
/* byte helpers                                                        */
/* ------------------------------------------------------------------ */

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ------------------------------------------------------------------ */
/* encryption keys                                                     */
/*                                                                     */
/* No key material is embedded in this program - it is read from the    */
/* user's own files, the way OpenIV does:                              */
/*                                                                     */
/*   encryption_key.bin  32 bytes (raw, or 64 hex digits).  Required    */
/*                       for the console ciphers: PS3 (cell), Xbox 360  */
/*                       (xenon) and the generic rage key.             */
/*   ng_keys.bin         101 x 32 bytes (raw, or hex).  Required for    */
/*                       "NG" archives (PC Legacy and PC Enhanced, tag  */
/*                       0x0FEFFFFF).  Only 101 keys exist and each     */
/*                       archive uses exactly one of them for its table  */
/*                       of contents, so a partial table is still useful.*/
/*                                                                     */
/* Each file is looked for in this order:                              */
/*   1. the path given with --key / --ng-keys                          */
/*   2. $OPENVI_KEY / $OPENVI_NG_KEYS                                  */
/*   3. next to the archive being opened                               */
/*   4. the current directory                                          */
/*   5. next to the executable                                         */
/*   6. $XDG_CONFIG_HOME/openvi/   (default ~/.config/openvi/)         */
/*   7. ~/.local/share/openvi/                                         */
/* ------------------------------------------------------------------ */

#define RPF_KEY_LEN 32
#define RPF_NG_KEYS 101

static uint8_t g_key[RPF_KEY_LEN];
static int     g_key_found;
static char    g_key_path[RPF_PATH_MAX];

static uint8_t g_ng[RPF_NG_KEYS][RPF_KEY_LEN];
static int     g_ng_count;
static char    g_ng_path[RPF_PATH_MAX];

static char    g_key_wanted[RPF_PATH_MAX];   /* --key     */
static char    g_ng_wanted[RPF_PATH_MAX];    /* --ng-keys */
static char    g_wbc_wanted[RPF_PATH_MAX];   /* --wbc     */

static int aes256_ecb_decrypt(const uint8_t key[32], uint8_t *buf, size_t len, int rounds);

void rpf_set_key_path(const char *path)
{
    snprintf(g_key_wanted, sizeof g_key_wanted, "%s", path ? path : "");
    g_key_found = 0;
    g_key_path[0] = 0;
}

void rpf_set_ng_key_path(const char *path)
{
    snprintf(g_ng_wanted, sizeof g_ng_wanted, "%s", path ? path : "");
    g_ng_count = 0;
    g_ng_path[0] = 0;
}

void rpf_set_wbc_path(const char *path)
{
    snprintf(g_wbc_wanted, sizeof g_wbc_wanted, "%s", path ? path : "");
}

static void ensure_keys(const char *archive);

void rpf_preload_keys(void) { ensure_keys(NULL); }

/* Read up to `max` 32 byte keys from a file - raw binary or hex text. */
static int load_key_table(const char *path, uint8_t *out, int max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t raw[RPF_KEY_LEN * RPF_NG_KEYS * 2];
    size_t n = fread(raw, 1, sizeof raw, f);
    fclose(f);

    if (n >= (size_t)RPF_KEY_LEN) {           /* raw binary */
        int cnt = (int)(n / RPF_KEY_LEN);
        if (cnt > max) cnt = max;
        memcpy(out, raw, (size_t)cnt * RPF_KEY_LEN);
        return cnt;
    }

    int nibble = -1, bytes = 0;               /* hex text */
    for (size_t i = 0; i < n && bytes < max * RPF_KEY_LEN; i++) {
        uint8_t c = raw[i];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;                        /* whitespace, commas, 0x, ... */
        if (nibble < 0) nibble = v;
        else { out[bytes++] = (uint8_t)((nibble << 4) | v); nibble = -1; }
    }
    if (bytes < RPF_KEY_LEN) return -1;
    int cnt = bytes / RPF_KEY_LEN;
    return cnt > max ? max : cnt;
}

/* directories that may hold key files, most specific first */
static int collect_key_dirs(const char *archive, char dirs[8][RPF_PATH_MAX])
{
    int n = 0;
    char tmp[RPF_PATH_MAX];

    if (archive && archive[0]) {
        snprintf(tmp, sizeof tmp, "%s", archive);
        char *s = strrchr(tmp, '/');
        if (s) { *s = 0; snprintf(dirs[n++], RPF_PATH_MAX, "%s", tmp); }
    }
    snprintf(dirs[n++], RPF_PATH_MAX, ".");

    ssize_t k = readlink("/proc/self/exe", tmp, sizeof tmp - 1);
    if (k > 0) {
        tmp[k] = 0;
        char *s = strrchr(tmp, '/');
        if (s) { *s = 0; snprintf(dirs[n++], RPF_PATH_MAX, "%s", tmp); }
    }

    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(dirs[n++], RPF_PATH_MAX, "%s/openvi", xdg);
    else if (home) snprintf(dirs[n++], RPF_PATH_MAX, "%s/.config/openvi", home);
    if (home) snprintf(dirs[n++], RPF_PATH_MAX, "%s/.local/share/openvi", home);
    return n;
}

/* Locate the key material for `archive` once per archive. */
static void ensure_keys(const char *archive)
{
    static char searched[RPF_PATH_MAX] = "\1";   /* nothing searched yet */
    const char *want = archive ? archive : "";
    if (!strcmp(searched, want)) return;
    snprintf(searched, sizeof searched, "%s", want);

    g_key_found = 0;
    g_key_path[0] = 0;
    g_ng_count = 0;
    g_ng_path[0] = 0;

    char dirs[8][RPF_PATH_MAX];
    int nd = collect_key_dirs(archive, dirs);

    const char *env_key = getenv("OPENVI_KEY");
    const char *explicit_key[2] = { g_key_wanted[0] ? g_key_wanted : NULL, env_key };
    for (int i = 0; i < 2 && !g_key_found; i++) {
        if (!explicit_key[i] || !explicit_key[i][0]) continue;
        if (load_key_table(explicit_key[i], g_key, 1) == 1) {
            g_key_found = 1;
            snprintf(g_key_path, sizeof g_key_path, "%s", explicit_key[i]);
        }
    }
    for (int i = 0; i < nd && !g_key_found; i++) {
        char p[RPF_PATH_MAX];
        snprintf(p, sizeof p, "%.*s/encryption_key.bin", RPF_PATH_MAX - 32, dirs[i]);
        if (load_key_table(p, g_key, 1) == 1) {
            g_key_found = 1;
            snprintf(g_key_path, sizeof g_key_path, "%s", p);
        }
    }

    const char *env_ng = getenv("OPENVI_NG_KEYS");
    const char *explicit_ng[2] = { g_ng_wanted[0] ? g_ng_wanted : NULL, env_ng };
    for (int i = 0; i < 2 && !g_ng_count; i++) {
        if (!explicit_ng[i] || !explicit_ng[i][0]) continue;
        int c = load_key_table(explicit_ng[i], (uint8_t *)g_ng, RPF_NG_KEYS);
        if (c > 0) { g_ng_count = c; snprintf(g_ng_path, sizeof g_ng_path, "%s", explicit_ng[i]); }
    }
    for (int i = 0; i < nd && !g_ng_count; i++) {
        char p[RPF_PATH_MAX];
        snprintf(p, sizeof p, "%.*s/ng_keys.bin", RPF_PATH_MAX - 32, dirs[i]);
        int c = load_key_table(p, (uint8_t *)g_ng, RPF_NG_KEYS);
        if (c > 0) { g_ng_count = c; snprintf(g_ng_path, sizeof g_ng_path, "%s", p); }
    }

    /* PC white-box data: 84 tables + 101 cooked keys, from the user's own copy */
    const char *env_wbc = getenv("OPENVI_WBC");
    const char *wbc_file = g_wbc_wanted[0] ? g_wbc_wanted : env_wbc;
    wbc_load(archive, wbc_file);
}

int rpf_key_available(void) { return g_key_found; }
const uint8_t *rpf_key(void) { return g_key_found ? g_key : NULL; }
const char *rpf_key_path(void) { return g_key_found ? g_key_path : NULL; }
int rpf_ng_key_count(void) { return g_ng_count; }
const char *rpf_ng_key_path(void) { return g_ng_count ? g_ng_path : NULL; }
int rpf_wbc_available(void) { return wbc_available(); }
const char *rpf_wbc_path(void) { return wbc_path(); }

const char *rpf_key_hint(void)
{
    return "no encryption key found: put encryption_key.bin (32 bytes) next to the "
           "archive or in ~/.config/openvi/";
}

/* RAGE picks the key for a key-table archive from the archive itself:
 *
 *     selector = (joaat(basename(archive)) + archiveSizeInBytes) % 101
 *
 * which the table of contents, the names block and every payload all share.
 * Knowing it means the right key can be tried first instead of all 101. */
static uint32_t joaat(const char *s)
{
    uint32_t h = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h += *p;
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

static int ng_selector(const char *path, uint64_t size, int count)
{
    if (count <= 0) return -1;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return (int)(((uint64_t)joaat(base) + size) % (uint64_t)count);
}

/* Try one table key.  Returns 2 when the decrypted first entry is certainly a
 * directory, 0 otherwise (a weak match is remembered in *weak). */
static int ng_try_key(const uint8_t *toc, int i, int *weak)
{
    uint8_t blk[16];
    memcpy(blk, toc, 16);
    aes256_ecb_decrypt(g_ng[i], blk, 16, 1);
    if (blk[0] || blk[1] || blk[2] || blk[3]) return 0;
    if (blk[4] == 0x00 && blk[5] == 0xFF && blk[6] == 0xFF && blk[7] == 0x7F) return 2;
    if (*weak < 0) *weak = i;
    return 0;
}

/* Which key in the table encrypts this archive's table of contents?  The root
 * directory entry is nameOffset 0 / cmpSize 0 / offsetBlocks 0x7FFFFF, so it
 * decrypts to 00 00 00 00 00 FF FF 7F - a 64 bit signature that will not come
 * up by accident.  `prefer` is the selector computed from the archive name and
 * size and is tried first.  Returns the key index, or -1. */
static int ng_find_toc_key(const uint8_t *toc, size_t len, int prefer)
{
    if (len < 16) return -1;
    int weak = -1;

    if (prefer >= 0 && prefer < g_ng_count && ng_try_key(toc, prefer, &weak) == 2)
        return prefer;
    for (int i = 0; i < g_ng_count; i++) {
        if (i == prefer) continue;
        if (ng_try_key(toc, i, &weak) == 2) return i;
    }
    return weak;
}

/* Decrypt a buffer with whichever cipher this archive resolved to. */
static int archive_decrypt(RpfArchive *a, uint8_t *buf, size_t len)
{
    if (len == 0) return 0;
    if (a->wbc) {
        size_t whole = len & ~(size_t)(WBC_BLOCK - 1);   /* block cipher */
        return whole ? wbc_decrypt(a->ngIndex, buf, whole) : 0;
    }
    if (a->key) return aes256_ecb_decrypt(a->key, buf, len, a->rounds);
    return 0;
}

/* Decrypt a compressed payload.  PC archives ("NG", Legacy and Enhanced) key
 * every file separately:
 *
 *     index = (joaat(entry name) + uncompressed size) % 101
 *
 * and encrypt only whole 16 byte blocks, so the trailing bytes of a payload
 * are stored as they are and must not be touched. */
static int decrypt_payload(RpfArchive *a, const RpfEntry *e, uint8_t *buf, size_t len)
{
    if (a->wbc) {
        int n = wbc_key_count();
        if (n <= 0) return -1;
        int ki = (int)(((uint64_t)joaat(e->name) + (uint64_t)e->size) % (uint64_t)n);
        size_t whole = len & ~(size_t)(WBC_BLOCK - 1);
        if (whole == 0) return 0;
        return wbc_decrypt(ki, buf, whole);
    }
    if (a->key) return aes256_ecb_decrypt(a->key, buf, len, a->rounds);
    return 0;
}

/* Resolve the cipher for a header tag.  Returns 0 when the archive can be
 * read.  `ng` is set for archives whose table of contents key still has to be
 * picked out of the NG key table. */
static int resolve_cipher(uint32_t tag, const uint8_t **key, const char **name,
                          int *rounds, int *ng, int *wbc)
{
    uint32_t id = tag & 0x0FFFFFFFu;
    *rounds = ((tag >> 28) == 0xF) ? 16 : 1;
    *key = NULL;
    *ng = 0;
    *wbc = 0;
    *name = "none";

    if (tag == 0 || tag == 0x4E45504Fu) return 0;   /* stored / PC OPEN */

    switch (id) {
    case 0x0FFFFFF8u:                                /* PS3 cell */
        *name = g_key_found ? "AES-256-ECB (cell)" : "AES-256-ECB (cell, encryption_key.bin required)";
        break;
    case 0x0FFFFFF7u:                                /* Xbox 360 xenon */
        *name = g_key_found ? "AES-256-ECB (xenon)" : "AES-256-ECB (xenon, encryption_key.bin required)";
        break;
    case 0x0FFFFFF9u:                                /* rage default */
        *name = g_key_found ? "AES-256-ECB (rage)" : "AES-256-ECB (rage, encryption_key.bin required)";
        break;
    case 0x0FEFFFFFu:                                /* PC Legacy / Enhanced */
        if (wbc_available()) {
            *wbc = 1;
            *name = "white-box AES (Arxan TransformIT)";
            return 0;
        }
        /* A plain 101 x 32 AES key table never opens these archives - the PC
         * cipher is a white-box, so say what is really missing. */
        if (g_ng_count > 0)
            *name = "PC white-box AES (Arxan TransformIT) - needs iaes2.bin (tables + cooked keys), not ng_keys.bin";
        else
            *name = "PC white-box AES (Arxan TransformIT) - iaes2.bin required (tables + cooked keys)";
        return -1;
    case 0xFFEFFFFu:                                 /* PS4 orbis, 101 key table */
    case 0xFFFEFFFu:                                 /* Xbox One, 101 key table */
        *ng = 1;
        if (g_ng_count > 0) {
            *name = "AES-256-ECB (multi-key)";
            return 0;
        }
        *name = "console multi-key AES - 101 key table required (ng_keys.bin)";
        return -1;
    default:
        *name = g_key_found ? "AES-256-ECB (custom)" : "unknown cipher";
        break;
    }
    if (!g_key_found) return -1;
    *key = g_key;
    return 0;
}

/* ------------------------------------------------------------------ */
/* AES-256-ECB (no padding)                                            */
/* ------------------------------------------------------------------ */

static int aes256_ecb_decrypt(const uint8_t key[32], uint8_t *buf, size_t len, int rounds)
{
    size_t n = len & ~(size_t)15;
    if (n == 0) return 0;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    for (int r = 0; r < rounds; r++) {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_ecb(), NULL, key, NULL) != 1 ||
            EVP_CIPHER_CTX_set_padding(ctx, 0) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }
        size_t done = 0;
        while (done < n) {
            size_t step = n - done;
            if (step > (1u << 20)) step = 1u << 20;
            int outl = 0;
            if (EVP_DecryptUpdate(ctx, buf + done, &outl, buf + done, (int)step) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }
            done += step;
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* raw deflate                                                         */
/* ------------------------------------------------------------------ */

static int inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    if (outlen == 0) return 0;
    z_stream s;
    memset(&s, 0, sizeof s);
    if (inflateInit2(&s, -15) != Z_OK) return -1;
    s.next_in = (Bytef *)in;
    s.avail_in = (uInt)inlen;
    s.next_out = out;
    s.avail_out = (uInt)outlen;

    int rc = inflate(&s, Z_FINISH);
    int ok = (rc == Z_STREAM_END) || (rc == Z_OK && s.avail_out == 0) || (rc == Z_BUF_ERROR && s.avail_out == 0);
    size_t produced = s.total_out;
    inflateEnd(&s);
    if (!ok) return -1;
    return (int)produced;
}

/* ------------------------------------------------------------------ */
/* RSC7 resource sizing                                                */
/* ------------------------------------------------------------------ */

static uint64_t rsc7_size(uint32_t flags, uint32_t page)
{
    uint64_t chunk = (uint64_t)page << ((flags & 0xF) + 4);
    uint64_t total = 0;
    total += (uint64_t)((flags >> 4) & 0x1) * (chunk >> 0);
    total += (uint64_t)((flags >> 5) & 0x3) * (chunk >> 1);
    total += (uint64_t)((flags >> 7) & 0xF) * (chunk >> 2);
    total += (uint64_t)((flags >> 11) & 0x3F) * (chunk >> 3);
    total += (uint64_t)((flags >> 17) & 0x7F) * (chunk >> 4);
    total += (uint64_t)((flags >> 24) & 0x1) * (chunk >> 5);
    total += (uint64_t)((flags >> 25) & 0x1) * (chunk >> 6);
    total += (uint64_t)((flags >> 26) & 0x1) * (chunk >> 7);
    total += (uint64_t)((flags >> 27) & 0x1) * (chunk >> 8);
    return total;
}

static uint32_t rsc7_large_size(const uint8_t h[16])
{
    return (uint32_t)h[7] | ((uint32_t)h[14] << 8) |
           ((uint32_t)h[5] << 16) | ((uint32_t)h[2] << 24);
}

/* ------------------------------------------------------------------ */
/* open / parse                                                        */
/* ------------------------------------------------------------------ */

static void parse_entry(RpfArchive *a, const uint8_t *b, RpfEntry *e)
{
    uint32_t d0, d1, d2, d3;
    if (a->bigEndian) {
        d0 = rd32be(b); d1 = rd32be(b + 4); d2 = rd32be(b + 8); d3 = rd32be(b + 12);
        e->offsetBlocks = (d0 >> 8) & 0xFFFFFFu;
        e->isResource   = (int)(d0 >> 31);
        e->offsetBlocks &= 0x7FFFFFu;
        e->cmpSize      = ((d0 & 0xFFu) << 16) | ((d1 >> 16) & 0xFFFFu);
        e->nameOffset   = d1 & 0xFFFFu;
    } else {
        d0 = rd32le(b); d1 = rd32le(b + 4); d2 = rd32le(b + 8); d3 = rd32le(b + 12);
        e->nameOffset   = d0 & 0xFFFFu;
        e->cmpSize      = ((d0 >> 16) & 0xFFFFu) | ((d1 & 0xFFu) << 16);
        e->offsetBlocks = (d1 >> 8) & 0x7FFFFFu;
        e->isResource   = (int)(d1 >> 31);
    }
    e->offset = e->offsetBlocks * 512u;
    e->size = d2;
    e->flags = d3;
    e->resVirtual = d2;
    e->resPhysical = d3;
    e->isDir = (e->offsetBlocks == 0x7FFFFFu);
    e->name[0] = 0;
    e->resourceSize = 0;

    /* payload encryption: non-resource files flagged d3 != 0 use the header cipher.
     * Resources are NOT encrypted (only "#sc" script resources are, and those use a
     * size-derived key index we do not need for normal assets). */
    e->encrypted = 0;
    if (!e->isDir && !e->isResource && e->flags != 0 && a->tocEncrypted)
        e->encrypted = 1;
}

RpfArchive *rpf_open(const char *path, char *err, size_t errlen)
{
#define FAIL(...) do { if (err) snprintf(err, errlen, __VA_ARGS__); return NULL; } while (0)

    FILE *fp = fopen(path, "rb");
    if (!fp) FAIL("cannot open %s: %s", path, strerror(errno));

    uint8_t hdr[16];
    if (fread(hdr, 1, 16, fp) != 16) { fclose(fp); FAIL("short header"); }

    uint32_t m_be = rd32be(hdr);
    uint32_t m_le = rd32le(hdr);
    int be;
    uint32_t expected_v = 7;
    if (m_be == 0x52504637u) be = 1;
    else if (m_le == 0x52504637u) be = 0;
    else {
        uint32_t v = (m_be >> 24) == 'R' ? (m_be & 0xFF) - '0' : (m_le & 0xFF) - '0';
        fclose(fp);
        FAIL("not an RPF7 archive (magic 0x%08X/0x%08X, version guess %u)", m_be, m_le, v);
    }
    (void)expected_v;

    RpfArchive *a = calloc(1, sizeof *a);
    if (!a) { fclose(fp); FAIL("out of memory"); }
    snprintf(a->path, sizeof a->path, "%s", path);
    a->fp = fp;
    a->bigEndian = be;
    a->version = 7;

    a->entryCount = be ? rd32be(hdr + 4) : rd32le(hdr + 4);
    uint32_t namesField = be ? rd32be(hdr + 8) : rd32le(hdr + 8);
    a->cipherTag = be ? rd32be(hdr + 12) : rd32le(hdr + 12);
    a->namesLen = namesField & 0x0FFFFFFFu;
    a->nameShift = (namesField >> 28) & 7u;
    /* The top bit of that word says the payloads use XCompress, not zlib. */
    a->xcompress = (namesField & 0x80000000u) != 0;

    if (a->entryCount == 0 || a->entryCount > 0x3FFFFFFFu) {
        rpf_close(a);
        FAIL("bogus entry count %u", a->entryCount);
    }
    if (a->namesLen > 64u * 1024u * 1024u) {
        rpf_close(a);
        FAIL("bogus names length %u", a->namesLen);
    }

    const char *cname = NULL;
    int rounds = 1, ng = 0, wbc = 0;
    ensure_keys(path);
    int rc = resolve_cipher(a->cipherTag, &a->key, &cname, &rounds, &ng, &wbc);
    a->cipherName = cname;
    a->rounds = rounds;
    a->ng = ng;
    a->wbc = wbc;
    a->ngIndex = -1;
    if (rc != 0) {
        a->platform = rpf_platform_of_tag(a->cipherTag);
        char buf[192];
        snprintf(buf, sizeof buf, "%s", cname);
        rpf_close(a);
        FAIL("%s", buf);
    }

    a->platform = rpf_platform_of_tag(a->cipherTag);

    if (a->xcompress) {
        snprintf(a->cipherBuf, sizeof a->cipherBuf, "%s, XCompress", cname);
        a->cipherName = a->cipherBuf;
    }

    size_t tocLen = (size_t)a->entryCount * 16;
    uint8_t *toc = malloc(tocLen);
    if (!toc) { rpf_close(a); FAIL("out of memory"); }
    if (fseek(fp, 0x10, SEEK_SET) != 0 || fread(toc, 1, tocLen, fp) != tocLen) {
        free(toc); rpf_close(a); FAIL("cannot read table of contents");
    }

    /* Key-table archives (PC "NG", PS4, Xbox One): pick which of the 101 keys
     * encrypts this table of contents. */
    if (a->ng && !a->key) {
        struct stat st;
        uint64_t asize = (stat(a->path, &st) == 0) ? (uint64_t)st.st_size : 0;
        int prefer = ng_selector(a->path, asize, g_ng_count);
        int idx = ng_find_toc_key(toc, tocLen, prefer);
        if (idx < 0) {
            free(toc); rpf_close(a);
            FAIL("key table: none of the %d keys in %s decrypts this table of contents",
                 g_ng_count, g_ng_path);
        }
        a->key = g_ng[idx];
        a->ngIndex = idx;
        snprintf(a->cipherBuf, sizeof a->cipherBuf,
                 "AES-256-ECB (key-table %d/%d%s)", idx, g_ng_count,
                 idx == prefer ? ", name+size selector" : "");
        a->cipherName = a->cipherBuf;
    }

    /* PC Legacy / Enhanced: the white-box tables of contents is unlocked with
     * one of the 101 cooked keys, again picked from the archive's own name and
     * size first. */
    if (a->wbc) {
        struct stat st;
        uint64_t asize = (stat(a->path, &st) == 0) ? (uint64_t)st.st_size : 0;
        int prefer = ng_selector(a->path, asize, wbc_key_count());
        int idx = wbc_pick_key(toc, tocLen, prefer);
        if (idx < 0) {
            free(toc); rpf_close(a);
            FAIL("white-box key table: none of the %d cooked keys in %s unlocks this table of contents",
                 wbc_key_count(), wbc_path());
        }
        a->ngIndex = idx;
        snprintf(a->cipherBuf, sizeof a->cipherBuf,
                 "white-box AES (TransformIT key-table %d/%d%s)",
                 idx, wbc_key_count(), idx == prefer ? ", name+size selector" : "");
        a->cipherName = a->cipherBuf;
    }
    a->tocEncrypted = (a->key != NULL || a->wbc);

    if (archive_decrypt(a, toc, tocLen) != 0) {
        free(toc); rpf_close(a); FAIL("decrypting the table of contents failed");
    }

    a->names = malloc(a->namesLen ? a->namesLen : 1);
    if (!a->names) { free(toc); rpf_close(a); FAIL("out of memory"); }
    if (a->namesLen) {
        if (fread(a->names, 1, a->namesLen, fp) != a->namesLen) {
            free(toc); rpf_close(a); FAIL("cannot read names block");
        }
        if (archive_decrypt(a, a->names, a->namesLen) != 0) {
            free(toc); rpf_close(a); FAIL("decrypting the names block failed");
        }
    }

    a->entries = calloc(a->entryCount, sizeof(RpfEntry));
    if (!a->entries) { free(toc); rpf_close(a); FAIL("out of memory"); }

    for (uint32_t i = 0; i < a->entryCount; i++) {
        RpfEntry *e = &a->entries[i];
        parse_entry(a, toc + (size_t)i * 16, e);

        uint32_t off = e->nameOffset << a->nameShift;
        if (off < a->namesLen) {
            size_t max = a->namesLen - off;
            size_t k = 0;
            while (k < max && a->names[off + k] && k < RPF_MAX_NAME - 1) k++;
            memcpy(e->name, a->names + off, k);
            e->name[k] = 0;
        }

        if (e->isResource) {
            uint32_t vp = (a->cipherTag & 0xFFFFFFFu) == 0x0FFFFFF8u ? 0x1000 : 0x2000;
            uint32_t pp = (a->cipherTag & 0xFFFFFFFu) == 0x0FFFFFF8u ? 0x1580 : 0x2000;
            e->resVirtSize = rsc7_size(e->resVirtual, vp);
            e->resPhysSize = rsc7_size(e->resPhysical, pp);
            e->resourceSize = e->resVirtSize + e->resPhysSize;
        }
    }
    free(toc);

    if (!a->entries[0].isDir) {
        rpf_close(a);
        FAIL("root entry is not a directory (wrong key?)");
    }
    if (err) err[0] = 0;
    return a;
#undef FAIL
}

void rpf_close(RpfArchive *a)
{
    if (!a) return;
    if (a->fp) fclose((FILE *)a->fp);
    free(a->names);
    free(a->entries);
    free(a);
}

/* Peek at an archive header without needing a key.  Returns 0 on success. */
int rpf_probe(const char *path, uint32_t *tag, uint32_t *entries, int *bigEndian)
{
    if (!path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[16];
    size_t n = fread(h, 1, 16, f);
    fclose(f);
    if (n != 16) return -1;
    int be;
    if (rd32be(h) == 0x52504637u) be = 1;
    else if (rd32le(h) == 0x52504637u) be = 0;
    else return -1;
    if (bigEndian) *bigEndian = be;
    if (entries) *entries = be ? rd32be(h + 4) : rd32le(h + 4);
    if (tag) *tag = be ? rd32be(h + 12) : rd32le(h + 12);
    return 0;
}

const char *rpf_platform_of_tag(uint32_t tag)
{
    switch (tag & 0x0FFFFFFFu) {
    case 0x4E45504Fu: return "PC";
    case 0x0FFFFFF8u: return "PS3";
    case 0x0FFFFFF7u: return "Xbox 360";
    case 0x0FFFFFF9u: return "RAGE";
    case 0x0FEFFFFFu: return "PC (NG)";
    case 0xFFEFFFFu: return "PS4";
    case 0xFFFEFFFu: return "Xbox 360";
    default: return "unknown";
    }
}

const char *rpf_game_name(const RpfArchive *a)
{
    if (a->version == 7) return "Grand Theft Auto V";
    return "Unknown RAGE title";
}

/* ------------------------------------------------------------------ */
/* extraction                                                          */
/* ------------------------------------------------------------------ */

static int read_at(RpfArchive *a, uint64_t off, void *buf, size_t len)
{
    if (fseek((FILE *)a->fp, (long)off, SEEK_SET) != 0) return -1;
    if (fread(buf, 1, len, (FILE *)a->fp) != len) return -1;
    return 0;
}

int rpf_extract(RpfArchive *a, const RpfEntry *e,
                uint8_t **out, size_t *outlen, char *err, size_t errlen)
{
#define FAIL(...) do { if (err) snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)
    if (e->isDir) FAIL("entry is a directory");

    /* Resources: the entry's size fields are segment flags, not byte counts.
     * Layout on disk is [16 byte resource header][raw deflate payload], which
     * inflates to virtual + physical segment sizes. */
    if (e->isResource) {
        uint32_t onDisk = e->cmpSize;
        if (onDisk == 0xFFFFFFu) {
            uint8_t rh[16];
            if (read_at(a, e->offset, rh, 16) != 0) FAIL("cannot read large resource header");
            onDisk = rsc7_large_size(rh);
        }
        if (onDisk < 0x10u) FAIL("resource raw size too small (%u)", onDisk);

        size_t body = (size_t)onDisk - 16u;
        uint8_t *buf = malloc(body ? body : 1);
        if (!buf) FAIL("out of memory");
        if (read_at(a, e->offset + 16u, buf, body) != 0) { free(buf); FAIL("short read"); }

        /* Masked script resources ("#sc") are AES encrypted with the archive cipher,
         * unlike every other resource. */
        {
            size_t nl = strlen(e->name);
            if (nl >= 3 && !strcmp(e->name + nl - 3, "#sc") &&
                (body % WBC_BLOCK) == 0)
                archive_decrypt(a, buf, body);
        }

        uint64_t want = e->resourceSize;
        if (want > 0 && want <= (256ull << 20)) {
            uint8_t *dec = malloc((size_t)want);
            if (dec) {
                int n = inflate_raw(buf, body, dec, (size_t)want);
                if (n > 0) {
                    free(buf);
                    *out = dec;
                    *outlen = (size_t)n;
                    return 0;
                }
                free(dec);
            }
        }
        /* fall back to the raw body (stored resource) */
        *out = buf;
        *outlen = body;
        return 0;
    }

    size_t want = e->size;
    if (e->cmpSize == 0) {
        /* stored */
        uint8_t *buf = malloc(want ? want : 1);
        if (!buf) FAIL("out of memory");
        if (read_at(a, e->offset, buf, want) != 0) { free(buf); FAIL("short read"); }
        *out = buf; *outlen = want;
        return 0;
    }

    uint8_t *cbuf = malloc(e->cmpSize);
    if (!cbuf) FAIL("out of memory");
    if (read_at(a, e->offset, cbuf, e->cmpSize) != 0) { free(cbuf); FAIL("short read"); }
    if (e->encrypted && decrypt_payload(a, e, cbuf, e->cmpSize) != 0) {
        free(cbuf); FAIL("decryption failed");
    }

    uint8_t *dbuf = malloc(want ? want : 1);
    if (!dbuf) { free(cbuf); FAIL("out of memory"); }
    int n = inflate_raw(cbuf, e->cmpSize, dbuf, want);
    free(cbuf);
    if (n < 0) {
        free(dbuf);
        if (a->xcompress)
            FAIL("XCompress payload: this archive uses RAGE's XCompress codec instead of zlib");
        FAIL("decompression failed");
    }
    *out = dbuf; *outlen = (size_t)n;
    return 0;
#undef FAIL
}

void rpf_entry_path(RpfArchive *a, uint32_t idx, char *buf, size_t buflen)
{
    const char *parts[256];
    int n = 0;
    uint32_t cur = idx;
    int guard = 0;
    while (cur != 0 && n < 256 && guard++ < 512) {
        parts[n++] = a->entries[cur].name;
        uint32_t parent = 0;
        for (uint32_t i = 0; i < a->entryCount; i++) {
            const RpfEntry *p = &a->entries[i];
            if (!p->isDir) continue;
            if (cur >= p->size && cur < p->size + p->flags) { parent = i; break; }
        }
        cur = parent;
    }
    size_t pos = 0;
    buf[0] = 0;
    for (int i = n - 1; i >= 0; i--) {
        size_t l = strlen(parts[i]);
        if (pos + l + 2 >= buflen) break;
        memcpy(buf + pos, parts[i], l);
        pos += l;
        if (i > 0) buf[pos++] = '/';
    }
    buf[pos] = 0;
}
