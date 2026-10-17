/* main.c - OpenIV for Linux: native GTK3 browser for RAGE (GTA V) RPF7 archives.
 *
 * The window mirrors OpenIV's main form (TMainWindow.dfm) and its English
 * strings (Resources/Languages/en_GB/LegacyUI.xml):
 *
 *   navigation band : back / forward / up, the address breadcrumb, the edit
 *                     mode button and the filter box (NavPane, PathBar, tbCmd,
 *                     SearchBox)
 *   menu bar        : File Edit New View Favorites Tools Help (TActionsModule)
 *   tool bar        : icon buttons for the archive and view actions
 *   archive tree    : every opened archive and its folders (treeHost)
 *   file list       : Name / Type / Size / Attributes / Compressed (ArchiveList)
 *   live preview    : right hand pane, "Select file to preview." (panePreView)
 *   status bar      : items, selection, version, archive, game, platform
 */

#include "app.h"

/* this file */
static int cli_games(void);
static int cli_list(const char *path);
static int cli_extract(const char *path, const char *dest);
static int find_by_path(RpfArchive *a, const char *path);
static int cli_cat(const char *path, const char *entrypath, const char *out);
static int app_screenshot(App *app, const char *out);

/* CLI test mode                                                       */
static int cli_games(void)
{
    GameInstall games[16];
    rpf_preload_keys();
    int n = game_detect(games, 16);
    printf("GTA V installations found: %d\n", n);
    for (int i = 0; i < n; i++) {
        char line[320];
        game_summary(&games[i], line, sizeof line);
        printf("\n%s\n  %s\n", line, games[i].path);

        DIR *d = opendir(games[i].path);
        if (!d) continue;
        struct dirent *de;
        int shown = 0;
        while ((de = readdir(d)) && shown < 8) {
            size_t l = strlen(de->d_name);
            if (l <= 4 || strcasecmp(de->d_name + l - 4, ".rpf")) continue;
            char p[RPF_PATH_MAX];
            snprintf(p, sizeof p, "%.*s/%s", RPF_PATH_MAX - 300, games[i].path, de->d_name);
            uint32_t tag = 0, cnt = 0;
            int be = 0;
            if (rpf_probe(p, &tag, &cnt, &be) != 0) continue;
            printf("    %-28s RPF7 %s %6u entries  tag 0x%08X  %s\n", de->d_name,
                   be ? "BE" : "LE", cnt, tag, rpf_platform_of_tag(tag));
            shown++;
        }
        closedir(d);
    }
    printf("\nencryption_key.bin: %s\nng_keys.bin:        %s\niaes2.bin:          %s\n",
           rpf_key_available() ? rpf_key_path() : "not found (console archives need one)",
           rpf_ng_key_count() ? rpf_ng_key_path() : "not found (PS4/Xbox One need one)",
           rpf_wbc_available() ? rpf_wbc_path() : "not found (PC Legacy/Enhanced need one)");
    return 0;
}

static int cli_list(const char *path)
{
    char err[512];
    RpfArchive *a = rpf_open(path, err, sizeof err);
    if (!a) { fprintf(stderr, "error: %s\n", err); return 1; }
    printf("%s  [%s, %s, %u entries]  %s\n", path, rpf_game_name(a), a->platform, a->entryCount,
           a->cipherName ? a->cipherName : "none");
    for (uint32_t i = 0; i < a->entryCount; i++) {
        const RpfEntry *e = &a->entries[i];
        char rel[RPF_PATH_MAX];
        rpf_entry_path(a, i, rel, sizeof rel);
        if (e->isDir) {
            printf("  D %s/\n", rel);
        } else {
            char sz[32];
            fmt_size(entry_display_size(e), sz, sizeof sz);
            printf("  %c %-58s size=%-10s packed=%-9u %s\n",
                   e->isResource ? 'R' : 'F', rel, sz, e->cmpSize,
                   e->encrypted ? "aes" : "");
        }
    }
    rpf_close(a);
    return 0;
}

static int cli_extract(const char *path, const char *dest)
{
    char err[512];
    RpfArchive *a = rpf_open(path, err, sizeof err);
    if (!a) { fprintf(stderr, "error: %s\n", err); return 1; }
    int n = 0;
    for (uint32_t i = 0; i < a->entryCount; i++) {
        const RpfEntry *e = &a->entries[i];
        if (e->isDir) continue;
        char rel[RPF_PATH_MAX];
        rpf_entry_path(a, i, rel, sizeof rel);
        char out[RPF_PATH_MAX * 2];
        snprintf(out, sizeof out, "%s/%s", dest, rel);
        char *slash = strrchr(out, '/');
        if (slash) { *slash = 0; g_mkdir_with_parents(out, 0755); *slash = '/'; }
        uint8_t *data = NULL; size_t len = 0;
        if (rpf_extract(a, e, &data, &len, err, sizeof err) != 0) {
            fprintf(stderr, "  !! %s: %s\n", rel, err);
            continue;
        }
        FILE *f = fopen(out, "wb");
        if (f) { fwrite(data, 1, len, f); fclose(f); printf("  %-60s %zu bytes\n", rel, len); n++; }
        free(data);
    }
    printf("extracted %d files to %s\n", n, dest);
    rpf_close(a);
    return 0;
}

static int find_by_path(RpfArchive *a, const char *path)
{
    for (uint32_t i = 0; i < a->entryCount; i++) {
        char rel[RPF_PATH_MAX];
        rpf_entry_path(a, i, rel, sizeof rel);
        if (!strcmp(rel, path)) return (int)i;
    }
    return -1;
}

static int cli_cat(const char *path, const char *entrypath, const char *out)
{
    char err[512];
    RpfArchive *a = rpf_open(path, err, sizeof err);
    if (!a) { fprintf(stderr, "error: %s\n", err); return 1; }
    int idx = find_by_path(a, entrypath);
    if (idx < 0) { fprintf(stderr, "not found: %s\n", entrypath); rpf_close(a); return 1; }
    uint8_t *data = NULL;
    size_t len = 0;
    if (rpf_extract(a, &a->entries[idx], &data, &len, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        rpf_close(a);
        return 1;
    }
    FILE *f = (out && strcmp(out, "-")) ? fopen(out, "wb") : stdout;
    if (!f) { free(data); rpf_close(a); fprintf(stderr, "cannot write %s\n", out); return 1; }
    fwrite(data, 1, len, f);
    if (f != stdout) fclose(f);
    fprintf(stderr, "%s: %zu bytes\n", entrypath, len);
    free(data);
    rpf_close(a);
    return 0;
}

static int app_screenshot(App *app, const char *out)
{
    /* Give the window the size build_ui() negotiated: on a bare X server
     * without a window manager GTK maps it at its natural size instead. */
    if (g_win_w > 0 && g_win_h > 0)
        gtk_window_resize(GTK_WINDOW(app->window), g_win_w, g_win_h);
    for (int i = 0; i < 200; i++) {
        while (gtk_events_pending()) gtk_main_iteration_do(FALSE);
        g_usleep(5000);
    }
    GtkAllocation al;
    gtk_widget_get_allocation(app->window, &al);
    if (al.width <= 1 || al.height <= 1) {
        fprintf(stderr, "bad allocation %dx%d\n", al.width, al.height);
        return 1;
    }
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, al.width, al.height);
    cairo_t *cr = cairo_create(s);
    gtk_widget_draw(app->window, cr);
    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(s, out);
    cairo_surface_destroy(s);
    if (st != CAIRO_STATUS_SUCCESS) { fprintf(stderr, "png write failed\n"); return 1; }
    printf("wrote %s (%dx%d)\n", out, al.width, al.height);
    return 0;
}

int main(int argc, char **argv)
{
    /* key material applies to every mode */
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--key")) rpf_set_key_path(argv[i + 1]);
        else if (!strcmp(argv[i], "--ng-keys")) rpf_set_ng_key_path(argv[i + 1]);
        else if (!strcmp(argv[i], "--wbc")) rpf_set_wbc_path(argv[i + 1]);
    }

    if (argc > 1 && !strcmp(argv[1], "--games"))
        return cli_games();
    if (argc > 2 && !strcmp(argv[1], "--list"))
        return cli_list(argv[2]);
    if (argc > 3 && !strcmp(argv[1], "--extract")) {
        g_mkdir_with_parents(argv[3], 0755);
        return cli_extract(argv[2], argv[3]);
    }
    if (argc > 3 && !strcmp(argv[1], "--cat"))
        return cli_cat(argv[2], argv[3], argc > 4 ? argv[4] : NULL);

    const char *shot = NULL, *target = NULL, *navpath = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--nav") && i + 1 < argc) navpath = argv[++i];
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) ++i;
        else if (!strcmp(argv[i], "--ng-keys") && i + 1 < argc) ++i;
        else if (!strcmp(argv[i], "--wbc") && i + 1 < argc) ++i;
        else if (argv[i][0] != '-') target = argv[i];
    }

    gtk_init(&argc, &argv);

    App app;
    memset(&app, 0, sizeof app);
    app.cur.archive = -1;
    app.cur.entry = -1;
    app.archives = g_ptr_array_new();
    app.back = g_ptr_array_new();
    app.fwd = g_ptr_array_new();

    build_ui(&app);
    gtk_widget_show_all(app.window);

    if (target) {
        GStatBuf st;
        if (g_stat(target, &st) == 0 && S_ISDIR(st.st_mode))
            scan_game_folder(&app, target);
        else
            open_archive_path(&app, target, TRUE);
    } else {
        /* started without a target: open the first complete installation found */
        GameInstall games[16];
        int n = game_detect(games, 16);
        for (int i = 0; i < n; i++) {
            if (games[i].installed && games[i].archives > 0) {
                scan_game_folder(&app, games[i].path);
                break;
            }
        }
    }

    if (navpath && app.cur.archive >= 0) {
        RpfArchive *a = g_ptr_array_index(app.archives, (guint)app.cur.archive);
        int idx = find_by_path(a, navpath);
        if (idx >= 0) {
            nav_to(&app, app.cur.archive, idx, TRUE);
            /* if a file was requested, highlight its row so it is previewed */
            if (!a->entries[idx].isDir) {
                GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.list));
                GtkTreeModel *m = GTK_TREE_MODEL(app.list_store);
                GtkTreeIter it;
                if (gtk_tree_model_get_iter_first(m, &it)) {
                    do {
                        int v = 0;
                        gtk_tree_model_get(m, &it, COL_ENTRY, &v, -1);
                        if (v == idx) { gtk_tree_selection_select_iter(sel, &it); break; }
                    } while (gtk_tree_model_iter_next(m, &it));
                }
            }
        } else {
            fprintf(stderr, "path not found: %s\n", navpath);
        }
    }

    if (shot) return app_screenshot(&app, shot);
    gtk_main();
    return 0;
}
