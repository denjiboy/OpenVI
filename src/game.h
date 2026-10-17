/* game.h - locate GTA V installations (PC Legacy and PC Enhanced).
 *
 * Detection mirrors what OpenIV does when you ask it to find a game folder:
 * Steam libraries are read from `libraryfolders.vdf` and their app manifests,
 * and a few well known folders are scanned directly for the executable.  Only
 * GTA V is handled here, and only the PC editions:
 *
 *   Legacy   (2015)    GTA5.exe            common.rpf, x64*.rpf, update/update.rpf
 *   Enhanced (2025)    GTA5_Enhanced.exe   same layout, plus *_g9ec dlc packs
 *
 * Both editions use the same RPF7 layout with the "NG" cipher (tag 0x0FEFFFFF),
 * so both need the same key material (see rpf.h).
 */
#ifndef GAME_H
#define GAME_H

#include "rpf.h"

typedef struct GameInstall {
    char path[RPF_PATH_MAX];   /* folder that holds the .rpf archives */
    char label[192];           /* "Grand Theft Auto V Enhanced (Steam)" */
    char store[24];            /* "Steam", "Epic", "Rockstar", "local" */
    char exe[64];              /* executable found in the folder, "" if none */
    int  enhanced;             /* 1 = Enhanced (Gen9), 0 = Legacy */
    int  installed;            /* the store says the install is complete */
    int  archives;             /* number of *.rpf files in the folder */
} GameInstall;

/* Every GTA V installation that can be found.  Returns the count. */
int game_detect(GameInstall *out, int max);

/* Describe one folder the user picked (or an unknown install). */
void game_describe(const char *dir, GameInstall *out);

/* One line summary, for the status bar and dialogs. */
void game_summary(const GameInstall *g, char *buf, size_t len);

#endif /* GAME_H */
