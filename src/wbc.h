/* wbc.h - PC GTA V white-box AES (Arxan TransformIT iAES2).
 *
 * GTA V on PC (both Legacy and Enhanced) encrypts RPF7 archives with Arxan
 * TransformIT: a table driven, encoded form of AES-256 whose tables and keys
 * are regenerated for every game build, so no key material can be published
 * for it.  This module implements the block decrypt network itself (see
 * wbc_net.h) and reads the two things that vary - the 84 static tables and
 * the 101 cooked keys - from the user's own data at run time:
 *
 *   iaes2.bin   84 x 1024 bytes of tables followed by 101 x 320 byte keys
 *               (118336 bytes).  Searched next to the archive, next to the
 *               executable, in the current directory, in $XDG_CONFIG_HOME/openvi
 *               and in ~/.local/share/openvi.  --wbc / $OPENVI_WBC override.
 *
 * Nothing here is key material; the file is the user's own extract of their
 * own game data, the same way OpenIV reads GTA5.exe.
 */
#ifndef WBC_H
#define WBC_H

#include <stdint.h>
#include <stddef.h>

/* Type of block decrypt, shared with the AES paths in rpf.c. */
#define WBC_BLOCK 16

/* TransformIT ships exactly 101 cooked keys, same as the console key tables. */
#define WBC_NG_KEYS_MAX 101

/* Load the white-box data for `archive` (may be NULL).  `explicit_file` is a
 * --wbc path or $OPENVI_WBC, either may be NULL.  Returns 0 when usable. */
int  wbc_load(const char *archive, const char *explicit_file);

int  wbc_available(void);
int  wbc_key_count(void);
const char *wbc_path(void);
const char *wbc_hint(void);

/* Decrypt `len` bytes (a multiple of 16) in place with cooked key `index`. */
int  wbc_decrypt(int index, uint8_t *buf, size_t len);

/* Self test: decrypt the built in TransformIT reference vector.  Returns 0
 * when the loaded data produces the expected plaintext, -1 otherwise. */
int  wbc_selftest(void);

/* Pick the cooked key that decrypts this table of contents: the root
 * directory entry is offset 0 / cmpSize 0 / 0x7FFFFF blocks and therefore
 * decrypts to 00 00 00 00 00 FF FF 7F.  `prefer` (name+size selector) is tried
 * first.  Returns the index, or -1. */
int  wbc_pick_key(const uint8_t *toc, size_t len, int prefer);

#endif /* WBC_H */
