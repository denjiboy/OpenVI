/* rpf.h - RAGE Package File (RPF7 / GTA V) reader.
 *
 * Reverse engineered from OpenIV.exe (Rage2.Archives.RPF7) and validated
 * against real GTA V PS3 archives.
 *
 * Layout (16 byte header, table offset 0x10, block size 512):
 *   0x00 u32 magic      "RPF7" as a big/little endian u32 (0x52504637)
 *   0x04 u32 entryCount
 *   0x08 u32 namesField (len = & 0x0FFFFFFF, shift = (>>28)&7, xcomp = >>31)
 *   0x0C u32 cipherTag
 *
 * Entry (16 bytes).  Little endian files (PC):
 *   nameOffset = d0 & 0xFFFF
 *   cmpSize    = (d0 >> 16) | ((d1 & 0xFF) << 16)
 *   offset     = (d1 >> 8) & 0x7FFFFF     (in 512 byte blocks)
 *   isResource = d1 >> 31
 *   size       = d2    (uncompressed size; directories: first child index)
 *   flags      = d3    (directories: child count)
 * Big endian files (PS3/X360) reverse the first three field slots:
 *   offset     = ((d0 >> 8) & 0xFFFFFF)   (23 bits + resource bit)
 *   cmpSize    = ((d0 & 0xFF) << 16) | ((d1 >> 16) & 0xFFFF)
 *   nameOffset = d1 & 0xFFFF
 *   isResource = d0 >> 31
 *   size = d2, flags = d3
 *
 * Directories are marked by a block offset of 0x7FFFFF.
 */
#ifndef RPF_H
#define RPF_H

#include <stdint.h>
#include <stddef.h>

#define RPF_MAX_NAME 512
#define RPF_PATH_MAX 4096

typedef struct RpfEntry {
    uint32_t nameOffset;
    uint32_t offset;        /* byte offset of the payload */
    uint32_t offsetBlocks;  /* raw block index */
    uint32_t size;          /* uncompressed size, or first child index for dirs */
    uint32_t cmpSize;       /* on-disk payload size (0 = stored) */
    uint32_t flags;         /* d3, for dirs: child count */
    uint32_t resVirtual;    /* resource system flags (d2) */
    uint32_t resPhysical;   /* resource graphics flags (d3) */
    uint64_t resourceSize;  /* computed resource payload size */
    uint64_t resVirtSize;   /* virtual (system) segment size */
    uint64_t resPhysSize;   /* physical (graphics) segment size */
    int isDir;
    int isResource;
    int encrypted;          /* payload is AES encrypted */
    char name[RPF_MAX_NAME];
} RpfEntry;

typedef struct RpfArchive {
    char path[RPF_PATH_MAX];
    void *fp;                            /* FILE* */
    int bigEndian;
    int version;                         /* 7 */
    uint32_t entryCount;
    uint32_t namesLen;
    uint32_t nameShift;
    uint32_t cipherTag;
    const char *platform;                /* "PC", "PS3", ... */
    const char *cipherName;              /* "AES-256-ECB", "none", ... */
    int tocEncrypted;
    int xcompress;                       /* payloads use XCompress, not zlib */
    int ng;                              /* archive uses a 101 key table */
    int ngIndex;                         /* NG key index used, -1 when n/a */
    int wbc;                             /* TransformIT white-box path */
    char cipherBuf[96];                   /* storage for a composed cipher name */
    uint8_t *names;
    const uint8_t *key;                  /* resolved 32 byte key or NULL */
    int rounds;
    RpfEntry *entries;
} RpfArchive;

/* Open an archive. Returns NULL on failure and fills err. */
RpfArchive *rpf_open(const char *path, char *err, size_t errlen);
void rpf_close(RpfArchive *a);

/* Extract one entry. Caller frees *out. */
int rpf_extract(RpfArchive *a, const RpfEntry *e,
                uint8_t **out, size_t *outlen,
                char *err, size_t errlen);

/* Path of entry `idx` inside archive `a` ("levels/gta5/foo.rpf"). */
void rpf_entry_path(RpfArchive *a, uint32_t idx, char *buf, size_t buflen);

/* Heuristic game name for a version 7 archive. */
const char *rpf_game_name(const RpfArchive *a);

/* Human readable platform derived from the cipher tag. */
const char *rpf_platform_of_tag(uint32_t tag);

/* Peek at an archive header without needing a key.  Returns 0 on success. */
int rpf_probe(const char *path, uint32_t *tag, uint32_t *entries, int *bigEndian);

/* Key material.  Nothing is embedded: keys are read from the files below,
 * searched next to the archive, next to the executable, in the current
 * directory and in ~/.config/openvi/ (see rpf.c for the full order).
 *
 *   encryption_key.bin   32 bytes, for the console ciphers.
 *   ng_keys.bin          101 x 32 bytes, for PC "NG" archives.
 *   iaes2.bin            84 x 1024 table bytes + 101 x 320 key bytes, for the
 *                        PC white-box cipher (Legacy and Enhanced).
 */
void rpf_set_key_path(const char *path);      /* --key     */
void rpf_set_ng_key_path(const char *path);   /* --ng-keys */
void rpf_set_wbc_path(const char *path);      /* --wbc     */
int rpf_key_available(void);
const uint8_t *rpf_key(void);
const char *rpf_key_path(void);
int rpf_ng_key_count(void);
const char *rpf_ng_key_path(void);
int rpf_wbc_available(void);
const char *rpf_wbc_path(void);
void rpf_preload_keys(void);                  /* look for key files up front */
const char *rpf_key_hint(void);

#endif /* RPF_H */
