/* game.c - GTA V installation detection. */
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAX_LIBS 24

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void joinpath(char *out, size_t cap, const char *dir, const char *name)
{
    int room = (int)cap - (int)strlen(name) - 2;
    if (room < 0) room = 0;
    snprintf(out, cap, "%.*s/%s", room, dir, name);
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               lower((unsigned char)p[i]) == lower((unsigned char)needle[i]))
            i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* Quoted value of "key" in a VDF / ACF file. */
static void text_value(const char *text, const char *key, char *out, size_t outlen)
{
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p) return;
    p = strchr(p + strlen(pat), '"');
    if (!p) return;
    const char *q = strchr(p + 1, '"');
    if (!q) return;
    size_t l = (size_t)(q - p - 1);
    if (l >= outlen) l = outlen - 1;
    memcpy(out, p + 1, l);
    out[l] = 0;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0 || n > (16 << 20)) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static const char *EXE_CANDIDATES[] = {
    "GTA5_Enhanced.exe", "GTA5.exe", "PlayGTAV.exe", "GTAVLauncher.exe", NULL
};

static void fill_install(GameInstall *g, const char *path, const char *name,
                         const char *store, int installed)
{
    memset(g, 0, sizeof *g);
    snprintf(g->path, sizeof g->path, "%s", path);
    snprintf(g->label, sizeof g->label, "%s (%s)", name, store);
    snprintf(g->store, sizeof g->store, "%s", store);
    g->enhanced = contains_ci(name, "enhanced") || contains_ci(path, "enhanced");
    g->installed = installed;

    for (int i = 0; EXE_CANDIDATES[i]; i++) {
        char p[RPF_PATH_MAX];
        joinpath(p, sizeof p, path, EXE_CANDIDATES[i]);
        if (!is_file(p)) continue;
        snprintf(g->exe, sizeof g->exe, "%s", EXE_CANDIDATES[i]);
        if (contains_ci(EXE_CANDIDATES[i], "enhanced")) g->enhanced = 1;
        break;
    }

    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l > 4 && !strcasecmp(de->d_name + l - 4, ".rpf")) g->archives++;
    }
    closedir(d);
}

static int already_have(const GameInstall *out, int n, const char *path)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(out[i].path, path)) return 1;
    return 0;
}

static int already_have_lib(char libs[][RPF_PATH_MAX], int n, const char *path)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(libs[i], path)) return 1;
    return 0;
}

int game_detect(GameInstall *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;

    char libs[MAX_LIBS][RPF_PATH_MAX];
    int nl = 0;

    const char *home = getenv("HOME");
    char roots[3][RPF_PATH_MAX];
    int nr = 0;
    if (home) {
        snprintf(roots[nr++], RPF_PATH_MAX, "%s/.local/share/Steam", home);
        snprintf(roots[nr++], RPF_PATH_MAX, "%s/.steam/steam", home);
        snprintf(roots[nr++], RPF_PATH_MAX, "%s/.var/app/com.valvesoftware.Steam/data/Steam", home);
    }

    /* Steam libraries: the usual roots plus everything libraryfolders.vdf lists.
     * This is what makes games on other mounts (USB drives, second SSDs) show up. */
    for (int i = 0; i < nr; i++) {
        if (is_dir(roots[i]) && nl < MAX_LIBS) {
            memcpy(libs[nl], roots[i], RPF_PATH_MAX);
            libs[nl][RPF_PATH_MAX - 1] = 0;
            nl++;
        }

        char vdf[RPF_PATH_MAX];
        joinpath(vdf, sizeof vdf, roots[i], "steamapps/libraryfolders.vdf");
        char *text = slurp(vdf);
        if (!text) continue;
        const char *p = text;
        while ((p = strstr(p, "\"path\"")) != NULL) {
            p += 6;
            const char *a = strchr(p, '"');
            if (!a) break;
            const char *b = strchr(a + 1, '"');
            if (!b) break;
            size_t l = (size_t)(b - a - 1);
            if (l > 0 && l < RPF_PATH_MAX) {
                char lib[RPF_PATH_MAX];
                memcpy(lib, a + 1, l);
                lib[l] = 0;
                if (is_dir(lib) && nl < MAX_LIBS && !already_have_lib(libs, nl, lib))
                    snprintf(libs[nl++], RPF_PATH_MAX, "%s", lib);
            }
            p = b + 1;
        }
        free(text);
    }

    /* app manifests carry the real game name and install directory */
    for (int i = 0; i < nl && n < max; i++) {
        char sa[RPF_PATH_MAX];
        joinpath(sa, sizeof sa, libs[i], "steamapps");
        DIR *d = opendir(sa);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) && n < max) {
            if (strncmp(de->d_name, "appmanifest_", 12) != 0) continue;
            char f[RPF_PATH_MAX];
            joinpath(f, sizeof f, sa, de->d_name);
            char *text = slurp(f);
            if (!text) continue;
            char name[160], idir[160], state[32];
            text_value(text, "name", name, sizeof name);
            text_value(text, "installdir", idir, sizeof idir);
            text_value(text, "StateFlags", state, sizeof state);
            free(text);
            if (!contains_ci(name, "Grand Theft Auto V")) continue;

            char base[RPF_PATH_MAX], path[RPF_PATH_MAX];
            joinpath(base, sizeof base, sa, "common");
            joinpath(path, sizeof path, base, idir[0] ? idir : name);
            if (already_have(out, n, path)) continue;
            fill_install(&out[n++], path, name, "Steam", atoi(state) == 4);
        }
        closedir(d);
    }

    /* anything not managed by Steam: Epic / Heroic, Rockstar, a manual copy */
    const char *guess[][2] = {
        { "Games/Heroic", "Epic" },
        { "Games",        "folder" },
        { ".local/share/Steam/steamapps/common", "Steam" },
        { NULL, NULL }
    };
    for (int i = 0; home && guess[i][0] && n < max; i++) {
        char root[RPF_PATH_MAX];
        joinpath(root, sizeof root, home, guess[i][0]);
        DIR *d = opendir(root);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) && n < max) {
            if (de->d_name[0] == '.') continue;
            char path[RPF_PATH_MAX];
            joinpath(path, sizeof path, root, de->d_name);
            if (!is_dir(path)) continue;

            char exe[RPF_PATH_MAX], exe2[RPF_PATH_MAX], cmon[RPF_PATH_MAX];
            joinpath(exe, sizeof exe, path, "GTA5.exe");
            joinpath(exe2, sizeof exe2, path, "GTA5_Enhanced.exe");
            joinpath(cmon, sizeof cmon, path, "common.rpf");
            if (!is_file(exe) && !is_file(exe2) && !is_file(cmon)) continue;
            if (already_have(out, n, path)) continue;
            fill_install(&out[n++], path, de->d_name, guess[i][1], 1);
        }
        closedir(d);
    }

    return n;
}

void game_describe(const char *dir, GameInstall *out)
{
    char name[160];
    snprintf(name, sizeof name, "%s", dir);
    size_t l = strlen(name);
    while (l > 1 && name[l - 1] == '/') name[--l] = 0;
    char *s = strrchr(name, '/');
    fill_install(out, dir, s ? s + 1 : name, "folder", 1);
}

void game_summary(const GameInstall *g, char *buf, size_t len)
{
    if (!g) { if (len) buf[0] = 0; return; }
    snprintf(buf, len, "%s - PC %s%s - %d archives%s%s",
             g->label,
             g->enhanced ? "Enhanced" : "Legacy",
             g->installed ? "" : " (still installing)",
             g->archives,
             g->exe[0] ? " - " : "", g->exe);
}
