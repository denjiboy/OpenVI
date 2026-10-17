/* archive.c - opening archives, the open/extract actions, the game folder scan
 * and drag and drop. */

#include "app.h"

/* this file */
static int write_entry(App *app, RpfArchive *a, int entryIdx, const char *destDir);
static int extract_dir(App *app, RpfArchive *a, int dirIdx, const char *destDir);
static const char *choose_folder(App *app, const char *title);
static const char *choose_game(App *app);

/* extraction                                                          */
static int write_entry(App *app, RpfArchive *a, int entryIdx, const char *destDir)
{
    const RpfEntry *e = &a->entries[entryIdx];
    char path[RPF_PATH_MAX];
    rpf_entry_path(a, (uint32_t)entryIdx, path, sizeof path);

    char out[RPF_PATH_MAX * 2];
    snprintf(out, sizeof out, "%s/%s", destDir, path);
    char *slash = strrchr(out, '/');
    if (slash) { *slash = 0; g_mkdir_with_parents(out, 0755); *slash = '/'; }

    uint8_t *data = NULL;
    size_t len = 0, wrote = 0;
    char err[256];
    if (rpf_extract(a, e, &data, &len, err, sizeof err) != 0) {
        fprintf(stderr, "extract %s: %s\n", path, err);
        return -1;
    }
    FILE *f = fopen(out, "wb");
    if (f) { wrote = fwrite(data, 1, len, f); fclose(f); }
    free(data);
    (void)wrote;
    (void)app;
    return 0;
}

static int extract_dir(App *app, RpfArchive *a, int dirIdx, const char *destDir)
{
    const RpfEntry *dir = &a->entries[dirIdx];
    int n = 0;
    for (uint32_t i = dir->size; i < dir->size + dir->flags && i < a->entryCount; i++) {
        const RpfEntry *c = &a->entries[i];
        if (c->isDir) n += extract_dir(app, a, (int)i, destDir);
        else { write_entry(app, a, (int)i, destDir); n++; }
    }
    return n;
}

static const char *choose_folder(App *app, const char *title)
{
    GtkWidget *d = gtk_file_chooser_dialog_new(title, GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Select", GTK_RESPONSE_ACCEPT, NULL);
    const char *ret = NULL;
    static char buf[RPF_PATH_MAX];
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        snprintf(buf, sizeof buf, "%s", fn);
        g_free(fn);
        ret = buf;
    }
    gtk_widget_destroy(d);
    return ret;
}

void on_extract_clicked(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (app->cur.archive < 0) return;
    int isdir = 0;
    int entry = selected_entry(app, &isdir);
    if (entry < 0) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Select a file or folder in the list first.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    const char *dest = choose_folder(app, "Extract to folder");
    if (!dest) return;
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    int n = 0;
    if (isdir) {
        char base[RPF_PATH_MAX * 2];
        snprintf(base, sizeof base, "%s/%s", dest, a->entries[entry].name);
        g_mkdir_with_parents(base, 0755);
        n = extract_dir(app, a, entry, base);
    } else {
        write_entry(app, a, entry, dest);
        n = 1;
    }
    char msg[512];
    snprintf(msg, sizeof msg, "Extracted %d file(s) to %s", n, dest);
    notify(app, GTK_MESSAGE_INFO, "%s", msg);
}

void on_extract_all_clicked(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (app->cur.archive < 0) return;
    const char *dest = choose_folder(app, "Extract current folder to...");
    if (!dest) return;
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    int n = extract_dir(app, a, app->cur.entry < 0 ? 0 : app->cur.entry, dest);
    char msg[512];
    snprintf(msg, sizeof msg, "Extracted %d file(s) to %s", n, dest);
    notify(app, GTK_MESSAGE_INFO, "%s", msg);
}

/* opening                                                             */
void open_archive_path(App *app, const char *path, gboolean select)
{
    /* Say what is actually wrong instead of reporting a cipher failure for a
     * file that is not even there. */
    GStatBuf st;
    if (g_stat(path, &st) != 0) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "No such file:\n%s\n\nThis game may not ship that archive - open the "
            "game folder instead and pick one of the .rpf files that exist.", path);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    char err[512];
    RpfArchive *a = rpf_open(path, err, sizeof err);
    if (!a) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Cannot open:\n%s\n\n%s", path, err);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    add_archive(app, a, select);
    /* File -> Recent Archives, kept in ~/.config/openvi/recent */
    list_add("recent", path, 10);
    rebuild_dynamic_menus(app);
}

void on_open_clicked(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    GtkWidget *d = gtk_file_chooser_dialog_new("Open RAGE archive", GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Open", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "RAGE archives (*.rpf)");
    gtk_file_filter_add_pattern(f, "*.rpf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        open_archive_path(app, fn, TRUE);
        g_free(fn);
    }
    gtk_widget_destroy(d);
}

void scan_game_folder(App *app, const char *dir)
{
    /* A GTA V install keeps its archives both in the root and in update/. */
    static const char *SUBDIRS[] = { "", "update", NULL };
    const char *name;
    char common[RPF_PATH_MAX] = "";
    int total = 0, opened = 0;
    char firstErr[512] = "";

    /* Unreadable archives are normal here: console archives and PC archives
     * both need key material the user supplies (see docs/KEYS.md), so report
     * rather than shout. */
    for (int s = 0; SUBDIRS[s]; s++) {
        char sub[RPF_PATH_MAX];
        if (SUBDIRS[s][0]) snprintf(sub, sizeof sub, "%.*s/%s", RPF_PATH_MAX - 300, dir, SUBDIRS[s]);
        else snprintf(sub, sizeof sub, "%s", dir);

        GDir *g = g_dir_open(sub, 0, NULL);
        if (!g) continue;
        while ((name = g_dir_read_name(g))) {
            size_t l = strlen(name);
            if (l < 5 || g_ascii_strcasecmp(name + l - 4, ".rpf")) continue;
            char full[RPF_PATH_MAX];
            snprintf(full, sizeof full, "%.*s/%s", RPF_PATH_MAX - 300, sub, name);
            total++;
            if (!g_ascii_strcasecmp(name, "common.rpf") && !common[0]) {
                snprintf(common, sizeof common, "%s", full);   /* opened last */
                continue;
            }
            char err[512];
            RpfArchive *a = rpf_open(full, err, sizeof err);
            if (a) { add_archive(app, a, FALSE); opened++; }
            else if (!firstErr[0]) snprintf(firstErr, sizeof firstErr, "%s", err);
        }
        g_dir_close(g);
    }
    if (common[0]) {
        char err[512];
        RpfArchive *a = rpf_open(common, err, sizeof err);
        if (a) { add_archive(app, a, TRUE); opened++; }
        else if (!firstErr[0]) snprintf(firstErr, sizeof firstErr, "%s", err);
    }

    char msg[512];
    if (total == 0) snprintf(msg, sizeof msg, "No .rpf archives in %s", dir);
    else if (opened == total) snprintf(msg, sizeof msg, "%s: %d archives opened", dir, total);
    else if (opened == 0)
        snprintf(msg, sizeof msg, "%s: %d archives, none could be decrypted - %s", dir, total, firstErr);
    else
        snprintf(msg, sizeof msg, "%s: %d of %d archives opened - %s", dir, opened, total, firstErr);
    notify(app, opened == total && total > 0 ? GTK_MESSAGE_INFO : GTK_MESSAGE_WARNING, "%s", msg);
}

void on_open_folder_clicked(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    const char *dest = choose_folder(app, "Select a GTA V game folder (containing *.rpf)");
    if (dest) scan_game_folder(app, dest);
}

/* game folders                                                        */

/* Ask the user which detected game folder to open.  Returns static storage. */
static const char *choose_game(App *app)
{
    static char chosen[RPF_PATH_MAX];
    GameInstall games[16];
    int n = game_detect(games, 16);

    if (n == 0) {
        GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "No GTA V installation was found.\n\n"
            "Checked the Steam libraries, ~/Games and the usual Steam install folder.  "
            "Use \"Open Game...\" to point at the folder yourself.");
        gtk_dialog_run(GTK_DIALOG(m));
        gtk_widget_destroy(m);
        return NULL;
    }

    if (n == 1) {
        snprintf(chosen, sizeof chosen, "%s", games[0].path);
        return chosen;
    }

    GtkWidget *d = gtk_dialog_new_with_buttons("Select game", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, "Cancel", GTK_RESPONSE_CANCEL, "Open", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget *combo = gtk_combo_box_text_new();
    for (int i = 0; i < n; i++) {
        char line[320];
        game_summary(&games[i], line, sizeof line);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), line);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 8);
    gtk_widget_show_all(box);

    const char *res = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        int i = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
        if (i >= 0 && i < n) {
            snprintf(chosen, sizeof chosen, "%s", games[i].path);
            res = chosen;
        }
    }
    gtk_widget_destroy(d);
    return res;
}

void on_games_clicked(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    const char *dir = choose_game(app);
    if (dir) scan_game_folder(app, dir);
}

/* drag and drop                                                       */
void on_drag_data(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                         GtkSelectionData *sel, guint info, guint time, gpointer data)
{
    App *app = data;
    (void)w; (void)x; (void)y; (void)info;
    gchar **uris = gtk_selection_data_get_uris(sel);
    if (uris) {
        for (int i = 0; uris[i]; i++) {
            gchar *path = g_filename_from_uri(uris[i], NULL, NULL);
            if (path) { open_archive_path(app, path, TRUE); g_free(path); }
        }
        g_strfreev(uris);
    }
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}
