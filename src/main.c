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
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include <zlib.h>

#include "rpf.h"
#include "game.h"

/* ArchiveList columns, in OpenIV's order (LegacyUI.xml, AeroWindow
 * StringList entries 0-4: Name, Type, Size, Attributes, Compressed). */
#define COL_ICON    0
#define COL_NAME    1
#define COL_TYPE    2
#define COL_SIZE    3
#define COL_ATTR    4
#define COL_PACKED  5
#define COL_SIZENUM 6
#define COL_PACKNUM 7
#define COL_ENTRY   8
#define COL_ISDIR   9
#define COL_NCOLS   10

#define APP_VERSION "1.0"

typedef struct { int archive; int entry; } NavLoc;

typedef struct {
    GtkWidget *window;
    GtkWidget *menubar;
    GtkWidget *toolbar;
    GtkWidget *navband;        /* address / navigation band (NavPane) */
    GtkWidget *crumbs;         /* breadcrumb buttons inside navband */
    GtkWidget *crumb_menu;     /* "copy address" dropdown of the breadcrumb */
    GtkWidget *edit_toggle;    /* "Edit mode" button of tbCmd */
    GtkWidget *filter;         /* SearchBox */
    GtkWidget *tree;
    GtkWidget *list;
    GtkWidget *tree_sw;
    GtkWidget *preview;        /* live preview pane (panePreView) */
    GtkWidget *preview_stack;
    GtkWidget *preview_label;  /* centred "Select file to preview." */
    GtkWidget *preview_view;   /* text / hex preview */
    GtkWidget *preview_image;
    GtkWidget *infobar;        /* notification pane at the top of the content */
    GtkWidget *notify_label;
    GtkWidget *statusbar;
    GtkWidget *status_items;
    GtkWidget *status_selected;
    GtkWidget *status_archive;
    GtkWidget *status_game;
    GtkWidget *status_platform;
    GtkWidget *status_version;
    GtkWidget *paned;
    GtkWidget *paned2;
    GtkTreeViewColumn *col[5];  /* the list's columns, indexed by COL_* - 1 */
    GtkCellRenderer *name_icon;
    GtkTreeStore *tree_store;
    GtkListStore *list_store;
    GPtrArray *archives;       /* RpfArchive* */
    GPtrArray *back;
    GPtrArray *fwd;
    NavLoc cur;
    int nested_seq;
    int view_mode;             /* 0 icon, 1 small icon, 2 list, 3 details, 4 tile */
    int sort_col;              /* -1 = archive order */
    int sort_desc;
    GtkAccelGroup *accel;
} App;

/* forward declarations */
static void refresh_list(App *app);
static void on_toggle_pane(GtkCheckMenuItem *mi_item, gpointer data);
static void nav_to(App *app, int archive, int entry, gboolean push);
static void update_preview(App *app, int archiveIdx, int entryIdx);
static void update_status(App *app);
static void update_breadcrumb(App *app);
static void apply_view_mode(App *app);
static void notify(App *app, GtkMessageType type, const char *fmt, ...);
static void msg_dialog(App *app, GtkMessageType type, const char *fmt, ...);
static void open_viewer(App *app, gboolean hex);
static void on_archive_properties(GtkWidget *w, gpointer data);
static void on_properties(GtkWidget *w, gpointer data);
static void on_extract_clicked(GtkWidget *w, gpointer data);
static void on_extract_all_clicked(GtkWidget *w, gpointer data);
static void on_open_clicked(GtkWidget *w, gpointer data);
static void on_open_folder_clicked(GtkWidget *w, gpointer data);
static void on_games_clicked(GtkWidget *w, gpointer data);
static void on_back(GtkWidget *w, gpointer data);
static void on_forward(GtkWidget *w, gpointer data);
static void on_up(GtkWidget *w, gpointer data);
static gboolean has_archive(App *app);
static int selected_entry(App *app, int *isdir);
static void list_add(const char *name, const char *value, int max);
static void rebuild_dynamic_menus(App *app);

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void fmt_size(uint64_t n, char *buf, size_t len)
{
    if (n < 1024) snprintf(buf, len, "%llu B", (unsigned long long)n);
    else if (n < 1024ull * 1024) snprintf(buf, len, "%.1f KB", n / 1024.0);
    else if (n < 1024ull * 1024 * 1024) snprintf(buf, len, "%.2f MB", n / (1024.0 * 1024));
    else snprintf(buf, len, "%.2f GB", n / (1024.0 * 1024 * 1024));
}

static const char *entry_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

static void entry_type(const RpfEntry *e, char *buf, size_t len)
{
    if (e->isDir) { snprintf(buf, len, "Folder"); return; }
    if (e->isResource) { snprintf(buf, len, "Resource"); return; }
    const char *ext = entry_ext(e->name);
    if (*ext) {
        char up[32];
        size_t k = strlen(ext);
        if (k > sizeof up - 1) k = sizeof up - 1;
        for (size_t i = 0; i < k; i++) up[i] = (char)toupper((unsigned char)ext[i]);
        up[k] = 0;
        snprintf(buf, len, "%s File", up);
    } else {
        snprintf(buf, len, "File");
    }
}

static uint64_t entry_display_size(const RpfEntry *e)
{
    if (e->isDir) return 0;
    if (e->isResource) return e->resourceSize;
    return e->size;
}

static int find_parent(RpfArchive *a, int idx)
{
    for (uint32_t i = 0; i < a->entryCount; i++) {
        const RpfEntry *p = &a->entries[i];
        if (!p->isDir) continue;
        if ((uint32_t)idx >= p->size && (uint32_t)idx < p->size + p->flags) return (int)i;
    }
    return -1;
}

static void archive_label(RpfArchive *a, char *buf, size_t len)
{
    const char *base = strrchr(a->path, '/');
    snprintf(buf, len, "%s", base ? base + 1 : a->path);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Icon used for an entry in the tree and in the list. */
static const char *entry_icon_name(const RpfEntry *e)
{
    if (e->isDir) return "folder";
    const char *ext = entry_ext(e->name);
    if (!g_ascii_strcasecmp(ext, "ytd") || !g_ascii_strcasecmp(ext, "dds") ||
        !g_ascii_strcasecmp(ext, "png") || !g_ascii_strcasecmp(ext, "jpg") ||
        !g_ascii_strcasecmp(ext, "jpeg") || !g_ascii_strcasecmp(ext, "bmp") ||
        !g_ascii_strcasecmp(ext, "gif") || !g_ascii_strcasecmp(ext, "tif"))
        return "image-x-generic";
    if (!g_ascii_strcasecmp(ext, "rpf") || !g_ascii_strcasecmp(ext, "awc") ||
        !g_ascii_strcasecmp(ext, "ybn") || !g_ascii_strcasecmp(ext, "zip"))
        return "package-x-generic";
    if (e->isResource) return "package-x-generic";
    if (!g_ascii_strcasecmp(ext, "exe") || !g_ascii_strcasecmp(ext, "dll") ||
        !g_ascii_strcasecmp(ext, "asi"))
        return "application-x-executable";
    return "text-x-generic";
}

/* Attributes column: what OpenIV reports about an entry.  Compressed sizes and
 * resources have columns of their own. */
static void entry_attrs(const RpfEntry *e, char *buf, size_t len)
{
    buf[0] = 0;
    if (e->isDir) { snprintf(buf, len, "Directory"); return; }
    if (e->encrypted) g_strlcat(buf, "Encrypted", len);
    else g_strlcat(buf, "Read only", len);
}

/* Jenkins one-at-a-time hash, the hash RAGE uses for names. */
static uint32_t rage_joaat(const char *s)
{
    uint32_t h = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        h += (c >= 'A' && c <= 'Z') ? (uint32_t)(c + 32) : (uint32_t)c;
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

static void copy_to_clipboard(App *app, const char *text)
{
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, text ? text : "", -1);
    notify(app, GTK_MESSAGE_INFO, "Copied to clipboard: %s", text ? text : "");
}

/* Modal message box, GLib style formatting. */
static void msg_dialog(App *app, GtkMessageType type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                          type, GTK_BUTTONS_OK, "%s", s);
    gtk_window_set_title(GTK_WINDOW(d), "OpenIV");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(s);
}

/* Message in the notification pane (DFM: paneNotifications). */
static void notify(App *app, GtkMessageType type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (app->infobar && app->notify_label) {
        gtk_info_bar_set_message_type(GTK_INFO_BAR(app->infobar), type);
        gtk_label_set_text(GTK_LABEL(app->notify_label), s);
        gtk_widget_show(app->infobar);
    }
    g_free(s);
}

static gboolean has_archive(App *app)
{
    return app->cur.archive >= 0 && app->cur.archive < (int)app->archives->len;
}

/* First selected entry of the list, or -1.  The list allows a multiple
 * selection (DFM: MultiSelect), so this goes through the selected rows. */
static int selected_entry(App *app, int *isdir)
{
    if (isdir) *isdir = 0;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->list));
    GList *rows = gtk_tree_selection_get_selected_rows(sel, NULL);
    int entry = -1;
    if (rows) {
        GtkTreeModel *m = GTK_TREE_MODEL(app->list_store);
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(m, &it, (GtkTreePath *)rows->data))
            gtk_tree_model_get(m, &it, COL_ENTRY, &entry, COL_ISDIR, isdir, -1);
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    }
    return entry;
}

/* ------------------------------------------------------------------ */
/* text / hex preview helpers                                          */
/* ------------------------------------------------------------------ */

static int looks_like_text(const uint8_t *d, size_t n)
{
    if (n == 0) return 1;
    size_t sample = n < 8192 ? n : 8192;
    if (sample >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) return 1;
    size_t ok = 0;
    for (size_t i = 0; i < sample; i++) {
        uint8_t c = d[i];
        if (c == 0) return 0;
        if (c == '\t' || c == '\n' || c == '\r' || (c >= 0x20 && c != 0x7F)) ok++;
    }
    return ok * 100 / sample >= 95;
}

static char *hexdump_lim(const uint8_t *d, size_t n, size_t lim)
{
    if (lim > n) lim = n;
    size_t cap = lim / 16 * 80 + 160;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t p = 0;
    for (size_t i = 0; i < lim; i += 16) {
        p += (size_t)snprintf(out + p, cap - p, "%08zx  ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < lim) p += (size_t)snprintf(out + p, cap - p, "%02X ", d[i + j]);
            else p += (size_t)snprintf(out + p, cap - p, "   ");
        }
        p += (size_t)snprintf(out + p, cap - p, " ");
        for (size_t j = 0; j < 16 && i + j < lim; j++) {
            uint8_t c = d[i + j];
            out[p++] = (char)((c >= 0x20 && c < 0x7F) ? c : '.');
        }
        out[p++] = '\n';
    }
    if (n > lim) p += (size_t)snprintf(out + p, cap - p, "... (%zu more bytes)\n", n - lim);
    out[p] = 0;
    return out;
}

static char *hexdump(const uint8_t *d, size_t n)
{
    return hexdump_lim(d, n, 4096);
}

static void set_text_view(GtkWidget *tv, const char *s)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(b, s ? s : "", -1);
}

/* ------------------------------------------------------------------ */
/* tree / list population                                              */
/* ------------------------------------------------------------------ */

static void add_folder_nodes(App *app, RpfArchive *a, GtkTreeIter *parent,
                             int archiveIdx, int dirIdx, int depth)
{
    if (depth > 24) return;
    const RpfEntry *dir = &a->entries[dirIdx];
    for (uint32_t i = dir->size; i < dir->size + dir->flags && i < a->entryCount; i++) {
        const RpfEntry *c = &a->entries[i];
        if (!c->isDir) continue;
        GtkTreeIter it;
        gtk_tree_store_append(app->tree_store, &it, parent);
        gtk_tree_store_set(app->tree_store, &it,
                           COL_ICON, "folder",
                           COL_NAME, c->name,
                           COL_ENTRY, (int)i,
                           COL_ISDIR, archiveIdx, /* reuse as archive index for the tree */
                           -1);
        add_folder_nodes(app, a, &it, archiveIdx, (int)i, depth + 1);
    }
}

static void add_archive(App *app, RpfArchive *a, gboolean select)
{
    int idx = (int)app->archives->len;
    g_ptr_array_add(app->archives, a);

    char label[RPF_PATH_MAX];
    archive_label(a, label, sizeof label);

    GtkTreeIter root;
    gtk_tree_store_append(app->tree_store, &root, NULL);
    gtk_tree_store_set(app->tree_store, &root,
                       COL_ICON, "package-x-generic",
                       COL_NAME, label,
                       COL_ENTRY, 0,
                       COL_ISDIR, idx,
                       -1);
    add_folder_nodes(app, a, &root, idx, 0, 0);

    /* expand the first level */
    GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(app->tree_store), &root);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(app->tree), p, FALSE);
    gtk_tree_path_free(p);

    if (select) nav_to(app, idx, 0, TRUE);
}

static gboolean filter_match(App *app, const char *name)
{
    const char *f = gtk_entry_get_text(GTK_ENTRY(app->filter));
    if (!f || !*f) return TRUE;
    char *hay = g_ascii_strdown(name, -1);
    char *needle = g_ascii_strdown(f, -1);
    gboolean r = strstr(hay, needle) != NULL;
    g_free(hay);
    g_free(needle);
    return r;
}

static void refresh_list(App *app)
{
    gtk_list_store_clear(app->list_store);
    if (!has_archive(app)) {
        update_breadcrumb(app);
        update_status(app);
        update_preview(app, -1, -1);
        return;
    }
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    int dirIdx = app->cur.entry;
    if (dirIdx < 0 || dirIdx >= (int)a->entryCount) dirIdx = 0;
    const RpfEntry *dir = &a->entries[dirIdx];
    if (!dir->isDir) dirIdx = find_parent(a, dirIdx);
    if (dirIdx < 0) dirIdx = 0;
    dir = &a->entries[dirIdx];

    for (uint32_t i = dir->size; i < dir->size + dir->flags && i < a->entryCount; i++) {
        const RpfEntry *c = &a->entries[i];
        if (!filter_match(app, c->name)) continue;
        char type[64], sz[32], pk[32], attr[64];
        entry_type(c, type, sizeof type);
        entry_attrs(c, attr, sizeof attr);
        if (c->isDir) sz[0] = 0; else fmt_size(entry_display_size(c), sz, sizeof sz);
        if (c->cmpSize) fmt_size(c->cmpSize, pk, sizeof pk); else snprintf(pk, sizeof pk, "-");

        GtkTreeIter it;
        gtk_list_store_append(app->list_store, &it);
        gtk_list_store_set(app->list_store, &it,
                           COL_ICON, entry_icon_name(c),
                           COL_NAME, c->name,
                           COL_TYPE, type,
                           COL_SIZE, sz,
                           COL_ATTR, attr,
                           COL_PACKED, pk,
                           COL_SIZENUM, (gint64)entry_display_size(c),
                           COL_PACKNUM, (gint64)c->cmpSize,
                           COL_ENTRY, (int)i,
                           COL_ISDIR, c->isDir,
                           -1);
    }

    /* keep the order chosen in "View -> Sort by" */
    if (app->sort_col >= 0)
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->list_store), app->sort_col,
                                             app->sort_desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);

    update_breadcrumb(app);
    update_status(app);
    update_preview(app, -1, -1);
}

/* ------------------------------------------------------------------ */
/* breadcrumb, status bar and sorting                                  */
/* ------------------------------------------------------------------ */

static void crumb_clicked(GtkWidget *w, gpointer data)
{
    App *app = data;
    NavLoc *loc = g_object_get_data(G_OBJECT(w), "navloc");
    if (loc) nav_to(app, loc->archive, loc->entry, TRUE);
}

static GtkWidget *crumb_button(App *app, const char *text, const char *tip, int archive, int entry)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    NavLoc *loc = g_new0(NavLoc, 1);
    loc->archive = archive;
    loc->entry = entry;
    g_object_set_data_full(G_OBJECT(b), "navloc", loc, g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(crumb_clicked), app);
    return b;
}

static void update_breadcrumb(App *app)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(app->crumbs));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    if (!has_archive(app)) {
        GtkWidget *l = gtk_label_new("(no archive)");
        gtk_widget_set_margin_start(l, 8);
        gtk_box_pack_start(GTK_BOX(app->crumbs), l, FALSE, FALSE, 0);
        gtk_widget_show(l);
        return;
    }

    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    int dirIdx = app->cur.entry;
    if (dirIdx < 0 || dirIdx >= (int)a->entryCount) dirIdx = 0;
    if (!a->entries[dirIdx].isDir) dirIdx = find_parent(a, dirIdx);
    if (dirIdx < 0) dirIdx = 0;

    /* collect the directories between the root and the current one */
    int chain[64], n = 0;
    for (int idx = dirIdx; idx > 0 && n < 64; ) {
        chain[n++] = idx;
        int p = find_parent(a, idx);
        if (p < 0) break;
        idx = p;
    }

    char label[RPF_PATH_MAX];
    archive_label(a, label, sizeof label);

    GtkWidget *b = crumb_button(app, label, a->path, app->cur.archive, 0);
    gtk_box_pack_start(GTK_BOX(app->crumbs), b, FALSE, FALSE, 0);
    gtk_widget_show(b);

    for (int i = n - 1; i >= 0; i--) {
        GtkWidget *sep = gtk_label_new("\u203A");
        gtk_widget_set_margin_start(sep, 2);
        gtk_widget_set_margin_end(sep, 2);
        gtk_box_pack_start(GTK_BOX(app->crumbs), sep, FALSE, FALSE, 0);
        gtk_widget_show(sep);

        GtkWidget *cb = crumb_button(app, a->entries[chain[i]].name, NULL,
                                     app->cur.archive, chain[i]);
        gtk_box_pack_start(GTK_BOX(app->crumbs), cb, FALSE, FALSE, 0);
        gtk_widget_show(cb);
    }
}

static void update_status(App *app)
{
    char buf[256], label[RPF_PATH_MAX];
    gtk_label_set_text(GTK_LABEL(app->status_version), "Version: " APP_VERSION);

    if (!has_archive(app)) {
        gtk_label_set_text(GTK_LABEL(app->status_items), "No open archive");
        gtk_label_set_text(GTK_LABEL(app->status_selected), "");
        gtk_button_set_label(GTK_BUTTON(app->status_archive), "");
        gtk_label_set_text(GTK_LABEL(app->status_game), "");
        gtk_label_set_text(GTK_LABEL(app->status_platform), "");
        return;
    }

    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    guint shown = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(app->list_store), NULL);
    guint total = 0;

    int dirIdx = app->cur.entry;
    if (dirIdx < 0 || dirIdx >= (int)a->entryCount) dirIdx = 0;
    if (!a->entries[dirIdx].isDir) dirIdx = find_parent(a, dirIdx);
    if (dirIdx < 0) dirIdx = 0;
    const RpfEntry *dir = &a->entries[dirIdx];
    for (uint32_t i = dir->size; i < dir->size + dir->flags && i < a->entryCount; i++) total++;

    if (shown == total) snprintf(buf, sizeof buf, "%u items shown", shown);
    else snprintf(buf, sizeof buf, "%u out of %u items shown", shown, total);
    gtk_label_set_text(GTK_LABEL(app->status_items), buf);

    guint sel = gtk_tree_selection_count_selected_rows(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(app->list)));
    if (sel == 0) snprintf(buf, sizeof buf, "No selected items");
    else snprintf(buf, sizeof buf, "Selected: %u", sel);
    gtk_label_set_text(GTK_LABEL(app->status_selected), buf);

    archive_label(a, label, sizeof label);
    gtk_button_set_label(GTK_BUTTON(app->status_archive), label);
    gtk_widget_set_tooltip_text(app->status_archive, a->path);
    gtk_label_set_text(GTK_LABEL(app->status_game), rpf_game_name(a));
    gtk_label_set_text(GTK_LABEL(app->status_platform), a->platform ? a->platform : "");
}

static gint list_sort_cmp(GtkTreeModel *m, GtkTreeIter *x, GtkTreeIter *y, gpointer data)
{
    int col = GPOINTER_TO_INT(data);
    if (col == COL_SIZE || col == COL_PACKED) {
        gint64 vx = 0, vy = 0;
        gtk_tree_model_get(m, x, col == COL_SIZE ? COL_SIZENUM : COL_PACKNUM, &vx, -1);
        gtk_tree_model_get(m, y, col == COL_SIZE ? COL_SIZENUM : COL_PACKNUM, &vy, -1);
        if (vx < vy) return -1;
        if (vx > vy) return 1;
        return 0;
    }
    char *sx = NULL, *sy = NULL;
    gtk_tree_model_get(m, x, col, &sx, -1);
    gtk_tree_model_get(m, y, col, &sy, -1);
    gint r = g_ascii_strcasecmp(sx ? sx : "", sy ? sy : "");
    g_free(sx);
    g_free(sy);
    return r;
}

static void set_sort(App *app, int col)
{
    if (app->sort_col == col) app->sort_desc = !app->sort_desc;
    else { app->sort_col = col; app->sort_desc = 0; }
    if (col < 0) {
        app->sort_col = -1;
        app->sort_desc = 0;
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->list_store),
                                             GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
                                             GTK_SORT_ASCENDING);
        refresh_list(app);
        return;
    }
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->list_store), col,
                                         app->sort_desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
}

/* ------------------------------------------------------------------ */
/* navigation                                                          */
/* ------------------------------------------------------------------ */

static void nav_to(App *app, int archive, int entry, gboolean push)
{
    NavLoc *prev = NULL;
    if (push && app->cur.archive >= 0) {
        prev = g_new0(NavLoc, 1);
        *prev = app->cur;
    }
    app->cur.archive = archive;
    app->cur.entry = entry;
    if (push) {
        g_ptr_array_set_size(app->fwd, 0);
        if (prev) g_ptr_array_add(app->back, prev);
    } else if (prev) {
        g_free(prev);
    }
    refresh_list(app);
}

/* ------------------------------------------------------------------ */
/* live preview pane (DFM: panePreView)                                */
/* ------------------------------------------------------------------ */

static void preview_message(App *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->preview_label), text);
    gtk_stack_set_visible_child_name(GTK_STACK(app->preview_stack), "message");
}

static void preview_text(App *app, const char *text)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->preview_view));
    gtk_text_buffer_set_text(b, text ? text : "", -1);
    gtk_stack_set_visible_child_name(GTK_STACK(app->preview_stack), "text");
}

static void update_preview(App *app, int archiveIdx, int entryIdx)
{
    if (archiveIdx < 0 || archiveIdx >= (int)app->archives->len || entryIdx < 0) {
        preview_message(app, "Select file to preview.");
        return;
    }
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)archiveIdx);
    if (entryIdx >= (int)a->entryCount) {
        preview_message(app, "Select file to preview.");
        return;
    }
    const RpfEntry *e = &a->entries[entryIdx];
    if (e->isDir) {
        preview_message(app, "Folder.\nDouble click to open it,\nor use View as text / hex for files.");
        return;
    }
    if (entry_display_size(e) > 64ull * 1024 * 1024) {
        preview_message(app, "No preview data.\nThe file is too large\nfor the live preview.");
        return;
    }

    uint8_t *data = NULL;
    size_t len = 0;
    char err[256];
    if (rpf_extract(a, e, &data, &len, err, sizeof err) != 0) {
        preview_message(app, "No preview data.");
        return;
    }

    /* an image the platform can decode? */
    const char *ext = entry_ext(e->name);
    if (!g_ascii_strcasecmp(ext, "png") || !g_ascii_strcasecmp(ext, "jpg") ||
        !g_ascii_strcasecmp(ext, "jpeg") || !g_ascii_strcasecmp(ext, "bmp") ||
        !g_ascii_strcasecmp(ext, "gif")) {
        GdkPixbufLoader *l = gdk_pixbuf_loader_new();
        gboolean okimg = FALSE;
        if (gdk_pixbuf_loader_write(l, data, len, NULL) &&
            gdk_pixbuf_loader_close(l, NULL)) {
            GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(l);
            if (pb) {
                gtk_image_set_from_pixbuf(GTK_IMAGE(app->preview_image), pb);
                okimg = TRUE;
            }
        }
        g_object_unref(l);
        if (okimg) {
            gtk_stack_set_visible_child_name(GTK_STACK(app->preview_stack), "image");
            free(data);
            return;
        }
    }

    if (looks_like_text(data, len)) {
        size_t cap = len < 256 * 1024 ? len : 256 * 1024;
        char *txt = g_malloc(cap + 1);
        memcpy(txt, data, cap);
        txt[cap] = 0;
        if (cap >= 3 && (uint8_t)txt[0] == 0xEF && (uint8_t)txt[1] == 0xBB &&
            (uint8_t)txt[2] == 0xBF)
            memmove(txt, txt + 3, cap - 2);
        preview_text(app, txt);
        g_free(txt);
    } else {
        char *hx = hexdump(data, len);
        preview_text(app, hx);
        free(hx);
    }
    free(data);
}

/* Full size viewer window, the way OpenIV opens files in the hex / text
 * editors instead of showing them inside the main window. */
static void open_viewer(App *app, gboolean hex)
{
    if (!has_archive(app)) {
        msg_dialog(app, GTK_MESSAGE_INFO, "No open archive.");
        return;
    }
    int isdir = 0;
    int entry = selected_entry(app, &isdir);
    if (entry < 0 || isdir) {
        msg_dialog(app, GTK_MESSAGE_INFO, "Select a file in the list first.");
        return;
    }
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    const RpfEntry *e = &a->entries[entry];
    if (entry_display_size(e) > 512ull * 1024 * 1024) {
        msg_dialog(app, GTK_MESSAGE_INFO, "%s is too large to open here (%s).",
                   e->name, hex ? "hex view" : "text view");
        return;
    }
    uint8_t *data = NULL;
    size_t len = 0;
    char err[256];
    if (rpf_extract(a, e, &data, &len, err, sizeof err) != 0) {
        msg_dialog(app, GTK_MESSAGE_ERROR, "Cannot read %s:\n\n%s", e->name, err);
        return;
    }

    char *text;
    if (hex) {
        text = hexdump_lim(data, len, 32u << 20);
    } else {
        text = malloc(len + 1);
        if (text) {
            memcpy(text, data, len);
            text[len] = 0;
            if (len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB &&
                (uint8_t)text[2] == 0xBF)
                memmove(text, text + 3, len - 2);
        }
    }
    free(data);

    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[RPF_MAX_NAME + 64];
    snprintf(title, sizeof title, "%s%s - OpenIV", e->name, hex ? " (hex)" : "");
    gtk_window_set_title(GTK_WINDOW(w), title);
    gtk_window_set_default_size(GTK_WINDOW(w), 900, 620);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), hex ? GTK_WRAP_NONE : GTK_WRAP_WORD_CHAR);
    set_text_view(tv, text ? text : "");
    free(text);
    gtk_container_add(GTK_CONTAINER(sw), tv);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    GtkWidget *info = gtk_label_new("");
    char line[64];
    char rel[RPF_PATH_MAX];
    rpf_entry_path(a, (uint32_t)entry, rel, sizeof rel);
    fmt_size(len, line, sizeof line);
    char buf[RPF_PATH_MAX * 2 + 256];
    snprintf(buf, sizeof buf, "%s  -  %s  -  %s", rel, line, a->path);
    gtk_label_set_text(GTK_LABEL(info), buf);
    gtk_widget_set_margin_start(info, 6);
    gtk_widget_set_margin_end(info, 6);
    gtk_widget_set_margin_top(info, 3);
    gtk_widget_set_margin_bottom(info, 3);
    gtk_label_set_ellipsize(GTK_LABEL(info), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(info, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), info, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(w), box);
    gtk_widget_show_all(w);
}

/* Everything the properties window reports about one entry. */
static void entry_info_text(RpfArchive *a, int entryIdx, char *buf, size_t len)
{
    const RpfEntry *e = &a->entries[entryIdx];
    char path[RPF_PATH_MAX], sz[32], pk[32], type[64], attr[64];
    rpf_entry_path(a, (uint32_t)entryIdx, path, sizeof path);
    entry_type(e, type, sizeof type);
    entry_attrs(e, attr, sizeof attr);
    fmt_size(entry_display_size(e), sz, sizeof sz);
    if (e->cmpSize) fmt_size(e->cmpSize, pk, sizeof pk); else snprintf(pk, sizeof pk, "stored");

    char resline[256];
    resline[0] = 0;
    if (e->isResource) {
        char vs[32], ps[32];
        fmt_size(e->resVirtSize, vs, sizeof vs);
        fmt_size(e->resPhysSize, ps, sizeof ps);
        snprintf(resline, sizeof resline,
                 "RSC segments: virtual %s (flags 0x%08X), physical %s (flags 0x%08X)\n",
                 vs, e->resVirtual, ps, e->resPhysical);
    }

    snprintf(buf, len,
             "Name:        %s\n"
             "Path:        %s\n"
             "Type:        %s\n"
             "Attributes:  %s\n"
             "Size:        %s\n"
             "Compressed:  %s\n"
             "Offset:      0x%llX\n"
             "Encrypted:   %s\n"
             "Resource:    %s\n"
             "%s"
             "Archive:     %s\n"
             "Game:        %s\n"
             "Platform:    %s   (%s)\n",
             e->name, path, type, attr, sz, pk,
             (unsigned long long)e->offset,
             e->encrypted ? "yes (AES-256-ECB)" : "no",
             e->isResource ? "yes" : "no",
             resline,
             a->path, rpf_game_name(a), a->platform, a->cipherName);
}

static GtkWidget *info_grid_field(GtkWidget *grid, int row, const char *label,
                                  const char *value)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_widget_set_valign(l, GTK_ALIGN_START);
    GtkWidget *v = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(v), value ? value : "");
    gtk_editable_set_editable(GTK_EDITABLE(v), FALSE);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
    return v;
}

/* DFM: TPropertiesWindow - the tabs with the name / type / size fields. */
static void on_properties(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (!has_archive(app)) { msg_dialog(app, GTK_MESSAGE_INFO, "No open archive."); return; }
    int isdir = 0;
    int entry = selected_entry(app, &isdir);
    if (entry < 0) { msg_dialog(app, GTK_MESSAGE_INFO, "No selected items."); return; }

    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    const RpfEntry *e = &a->entries[entry];
    char path[RPF_PATH_MAX], sz[32], pk[32], type[64], attr[64];
    rpf_entry_path(a, (uint32_t)entry, path, sizeof path);
    entry_type(e, type, sizeof type);
    entry_attrs(e, attr, sizeof attr);
    fmt_size(entry_display_size(e), sz, sizeof sz);
    if (e->cmpSize) fmt_size(e->cmpSize, pk, sizeof pk); else snprintf(pk, sizeof pk, "stored");

    GtkWidget *d = gtk_dialog_new_with_buttons("Properties", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, "Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    /* DFM: TPropertiesWindow.pcTabs */
    GtkWidget *tabs = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(box), tabs, TRUE, TRUE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);

    info_grid_field(grid, 0, "Name:", e->name);
    info_grid_field(grid, 1, "Type:", type);
    info_grid_field(grid, 2, "Path:", path);
    info_grid_field(grid, 3, "Size:", sz);
    info_grid_field(grid, 4, "Compressed:", pk);
    info_grid_field(grid, 5, "Attributes:", attr);

    char off[32];
    snprintf(off, sizeof off, "0x%llX", (unsigned long long)e->offset);
    info_grid_field(grid, 6, "Offset:", off);
    info_grid_field(grid, 7, "Archive:", a->path);

    GtkWidget *cb = gtk_check_button_new_with_label("Resource (RSC7)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), e->isResource);
    gtk_widget_set_sensitive(cb, FALSE);
    gtk_grid_attach(GTK_GRID(grid), cb, 1, 8, 1, 1);
    GtkWidget *cb2 = gtk_check_button_new_with_label("AES encrypted payload");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb2), e->encrypted);
    gtk_widget_set_sensitive(cb2, FALSE);
    gtk_grid_attach(GTK_GRID(grid), cb2, 1, 9, 1, 1);

    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), grid, gtk_label_new("Main"));

    GtkWidget *dsw = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *dtv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(dtv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(dtv), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(dtv), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(dsw), dtv);
    char *details = g_malloc(RPF_PATH_MAX * 4);
    entry_info_text(a, entry, details, RPF_PATH_MAX * 4);
    set_text_view(dtv, details);
    g_free(details);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), dsw, gtk_label_new("Details"));
    gtk_widget_set_size_request(tabs, 620, 360);

    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* DFM: TArchivePropertiesWindow - what the archive itself is. */
static void on_archive_properties(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (!has_archive(app)) { msg_dialog(app, GTK_MESSAGE_INFO, "No open archive."); return; }
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);

    const char *keysrc = a->wbc ? rpf_wbc_path()
                       : a->ng ? rpf_ng_key_path()
                       : rpf_key_path();

    GtkWidget *d = gtk_dialog_new_with_buttons("Archive properties", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, "Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start(GTK_BOX(box), grid, TRUE, TRUE, 0);

    char buf[256];
    snprintf(buf, sizeof buf, "RPF7 (version %d)", a->version);
    info_grid_field(grid, 0, "Format:", buf);
    info_grid_field(grid, 1, "Path:", a->path);
    info_grid_field(grid, 2, "Game:", rpf_game_name(a));
    info_grid_field(grid, 3, "Platform:", a->platform ? a->platform : "");
    snprintf(buf, sizeof buf, "%u", a->entryCount);
    info_grid_field(grid, 4, "Entries:", buf);
    snprintf(buf, sizeof buf, "%u names, %u bytes", a->entryCount, a->namesLen);
    info_grid_field(grid, 5, "Name table:", buf);
    snprintf(buf, sizeof buf, "%s (tag 0x%08X, %s endian)", a->cipherName, a->cipherTag,
             a->bigEndian ? "big" : "little");
    info_grid_field(grid, 6, "Encryption:", buf);
    if (a->wbc) snprintf(buf, sizeof buf, "white-box iAES, key index %d", a->ngIndex);
    else if (a->ng) snprintf(buf, sizeof buf, "NG key table index %d of 101", a->ngIndex);
    else snprintf(buf, sizeof buf, "%s", a->tocEncrypted ? "TOC and payloads" : "payloads only");
    info_grid_field(grid, 7, "Key selection:", buf);
    info_grid_field(grid, 8, "Key source:", keysrc ? keysrc : "not found");

    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void on_list_selection(GtkTreeSelection *sel, gpointer data)
{
    App *app = data;
    (void)sel;
    if (!has_archive(app)) return;
    int isdir = 0;
    int entry = selected_entry(app, &isdir);
    if (entry < 0) update_preview(app, -1, -1);
    else update_preview(app, app->cur.archive, entry);
    update_status(app);
}

static void on_list_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
    App *app = data;
    (void)tv; (void)col;
    GtkTreeModel *m = gtk_tree_view_get_model(GTK_TREE_VIEW(app->list));
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(m, &it, path)) return;
    int entry = 0, isdir = 0;
    gtk_tree_model_get(m, &it, COL_ENTRY, &entry, COL_ISDIR, &isdir, -1);

    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    const RpfEntry *e = &a->entries[entry];
    if (e->isDir) { nav_to(app, app->cur.archive, entry, TRUE); return; }

    /* nested archive? open it on demand */
    const char *ext = entry_ext(e->name);
    if (!g_ascii_strcasecmp(ext, "rpf") && entry_display_size(e) < (512ull << 20)) {
        uint8_t *data = NULL;
        size_t len = 0;
        char err[256];
        if (rpf_extract(a, e, &data, &len, err, sizeof err) != 0) {
            GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Cannot open nested archive: %s", err);
            gtk_dialog_run(GTK_DIALOG(d));
            gtk_widget_destroy(d);
            return;
        }
        char tmp[RPF_PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s/openiv-native", g_get_tmp_dir());
        g_mkdir_with_parents(tmp, 0700);
        char safe[RPF_MAX_NAME];
        snprintf(safe, sizeof safe, "%s", e->name);
        for (char *p = safe; *p; p++) if (*p == '/' || *p == '\\') *p = '_';
        snprintf(tmp, sizeof tmp, "%s/openiv-native/%d_%s", g_get_tmp_dir(), app->nested_seq++, safe);
        FILE *f = fopen(tmp, "wb");
        if (f) { fwrite(data, 1, len, f); fclose(f); }
        free(data);
        if (!f) return;
        char oerr[256];
        RpfArchive *na = rpf_open(tmp, oerr, sizeof oerr);
        if (!na) {
            GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "Nested archive %s could not be parsed:\n%s", e->name, oerr);
            gtk_dialog_run(GTK_DIALOG(d));
            gtk_widget_destroy(d);
            return;
        }
        add_archive(app, na, TRUE);
        return;
    }

    /* OpenIV hands files to its own editors; the read only equivalent here is
     * the text viewer window (Edit -> View as text... / View in Hex...). */
    open_viewer(app, FALSE);
}

static void on_tree_selection(GtkTreeSelection *sel, gpointer data)
{
    App *app = data;
    GtkTreeModel *m;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return;
    int entry = 0, arch = 0;
    gtk_tree_model_get(m, &it, COL_ENTRY, &entry, COL_ISDIR, &arch, -1);
    nav_to(app, arch, entry, TRUE);
}

/* ------------------------------------------------------------------ */
/* extraction                                                          */
/* ------------------------------------------------------------------ */

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

static void on_extract_clicked(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (app->cur.archive < 0) return;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->list));
    GtkTreeModel *m;
    GtkTreeIter it;
    int entry = -1, isdir = 0;
    if (gtk_tree_selection_get_selected(sel, &m, &it)) {
        gtk_tree_model_get(m, &it, COL_ENTRY, &entry, COL_ISDIR, &isdir, -1);
    }
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

static void on_extract_all_clicked(GtkWidget *w, gpointer data)
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

/* ------------------------------------------------------------------ */
/* opening                                                             */
/* ------------------------------------------------------------------ */

static void open_archive_path(App *app, const char *path, gboolean select)
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

static void on_open_clicked(GtkWidget *w, gpointer data)
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

static void scan_game_folder(App *app, const char *dir)
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

static void on_open_folder_clicked(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    const char *dest = choose_folder(app, "Select a GTA V game folder (containing *.rpf)");
    if (dest) scan_game_folder(app, dest);
}

/* ------------------------------------------------------------------ */
/* game folders                                                        */
/* ------------------------------------------------------------------ */

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

static void on_games_clicked(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    const char *dir = choose_game(app);
    if (dir) scan_game_folder(app, dir);
}

/* ------------------------------------------------------------------ */
/* drag and drop                                                       */
/* ------------------------------------------------------------------ */

static void on_drag_data(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
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

/* ------------------------------------------------------------------ */
/* UI construction                                                     */
/* ------------------------------------------------------------------ */

/* The old text-button helpers were replaced by the menu bar, the tool bar and
 * the live preview pane built below. */

static void refresh_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    refresh_list((App *)data);
}

static void on_back(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (app->back->len == 0) return;
    NavLoc *cur = g_new0(NavLoc, 1);
    *cur = app->cur;
    g_ptr_array_add(app->fwd, cur);
    NavLoc *l = g_ptr_array_steal_index(app->back, app->back->len - 1);
    nav_to(app, l->archive, l->entry, FALSE);
    app->cur = *l;
    g_free(l);
    refresh_list(app);
}

static void on_forward(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (app->fwd->len == 0) return;
    NavLoc *cur = g_new0(NavLoc, 1);
    *cur = app->cur;
    g_ptr_array_add(app->back, cur);
    NavLoc *l = g_ptr_array_steal_index(app->fwd, app->fwd->len - 1);
    nav_to(app, l->archive, l->entry, FALSE);
    app->cur = *l;
    g_free(l);
    refresh_list(app);
}

static void on_up(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (app->cur.archive < 0) return;
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    int p = find_parent(a, app->cur.entry);
    if (p >= 0) nav_to(app, app->cur.archive, p, TRUE);
}

static void on_filter_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    refresh_list((App *)data);
}

/* DFM: TAboutWindow3. */
static void on_about(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    GtkWidget *d = gtk_dialog_new_with_buttons("About", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, "Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v), 16);
    gtk_box_pack_start(GTK_BOX(box), v, TRUE, TRUE, 0);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b><big>OpenIV for Linux</big></b>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(v), title, FALSE, FALSE, 0);

    static const char *lines[] = {
        "Version " APP_VERSION " - native GTK3 build",
        "",
        "Browser and extractor for RAGE RPF7 archives, aimed at Grand Theft Auto V",
        "(PC Legacy and Enhanced, plus the PS3 and Xbox 360 console archives).",
        "",
        "The window layout, the labels and the RPF7/RSC7 handling were reverse",
        "engineered from OpenIV.exe; AES-256-ECB tables, raw deflate payloads and",
        "the Arxan white box iAES of the Enhanced archives are all implemented.",
        "",
        "No key material is bundled.  Point the app at your own",
        "encryption_key.bin, ng_keys.bin and iaes2.bin - see docs/KEYS.md.",
        "",
        "OpenIV is a project of its own; this is an independent reimplementation.",
        NULL
    };
    for (int i = 0; lines[i]; i++) {
        GtkWidget *l = gtk_label_new(lines[i]);
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_label_set_selectable(GTK_LABEL(l), TRUE);
        gtk_box_pack_start(GTK_BOX(v), l, FALSE, FALSE, 0);
    }

    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* ------------------------------------------------------------------ */
/* list columns, view modes, status bar                                */
/* ------------------------------------------------------------------ */

static void on_column_clicked(GtkTreeViewColumn *c, gpointer data)
{
    App *app = data;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "col"));
    set_sort(app, col);
}

static GtkTreeViewColumn *add_list_column(App *app, const char *title, int col, int width,
                                          gboolean icon, gfloat xalign)
{
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    if (icon) {
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        g_object_set(pix, "stock-size", GTK_ICON_SIZE_MENU, NULL);
        app->name_icon = pix;
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(c), pix, FALSE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(c), pix, "icon-name", COL_ICON, NULL);
    }
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    if (xalign > 0.0) g_object_set(r, "xalign", xalign, NULL);
    else g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(c), r, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(c), r, "text", col, NULL);

    gtk_tree_view_column_set_title(c, title);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_clickable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_alignment(c, xalign > 0.0f ? 1.0f : 0.0f);
    if (width > 0) gtk_tree_view_column_set_fixed_width(c, width);
    g_object_set_data(G_OBJECT(c), "col", GINT_TO_POINTER(col));
    g_signal_connect(c, "clicked", G_CALLBACK(on_column_clicked), app);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->list), c);
    return c;
}

static void apply_view_mode(App *app)
{
    /* "Details" shows every column.  The icon-like modes keep the name column
     * and grow the icon, which is as close as a GtkTreeView comes to OpenIV's
     * icon views. */
    static const GtkIconSize sizes[5] = {
        GTK_ICON_SIZE_DIALOG, GTK_ICON_SIZE_LARGE_TOOLBAR, GTK_ICON_SIZE_MENU,
        GTK_ICON_SIZE_MENU, GTK_ICON_SIZE_DIALOG
    };
    gboolean details = (app->view_mode == 3);
    for (int i = 0; i < 5; i++) {
        if (!app->col[i]) continue;
        gtk_tree_view_column_set_visible(app->col[i], details || i == 0);
    }
    if (app->name_icon) {
        int m = (app->view_mode >= 0 && app->view_mode <= 4) ? app->view_mode : 3;
        g_object_set(app->name_icon, "stock-size", sizes[m], NULL);
    }
}

static GtkWidget *status_label(const char *text, gboolean expand)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_single_line_mode(GTK_LABEL(l), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    if (expand) gtk_widget_set_hexpand(l, TRUE);
    return l;
}

static GtkWidget *status_sep(void)
{
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_top(s, 2);
    gtk_widget_set_margin_bottom(s, 2);
    return s;
}

/* ------------------------------------------------------------------ */
/* small configuration files: favorites and recent archives            */
/* ------------------------------------------------------------------ */

static char *config_file(const char *name)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "openvi", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, name, NULL);
    g_free(dir);
    return path;
}

static GPtrArray *list_load(const char *name)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    char *path = config_file(name);
    char *data = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *s = g_strstrip(lines[i]);
            if (*s) g_ptr_array_add(arr, g_strdup(s));
        }
        g_strfreev(lines);
        g_free(data);
    }
    g_free(path);
    return arr;
}

static void list_save(const char *name, GPtrArray *arr)
{
    GString *out = g_string_new("");
    for (guint i = 0; i < arr->len; i++)
        g_string_append_printf(out, "%s\n", (char *)g_ptr_array_index(arr, i));
    char *path = config_file(name);
    g_file_set_contents(path, out->str, -1, NULL);
    g_free(path);
    g_string_free(out, TRUE);
}

static void list_add(const char *name, const char *value, int max)
{
    GPtrArray *arr = list_load(name);
    for (int i = (int)arr->len - 1; i >= 0; i--)
        if (!strcmp((char *)g_ptr_array_index(arr, i), value)) g_ptr_array_remove_index(arr, i);
    g_ptr_array_insert(arr, 0, g_strdup(value));
    while ((int)arr->len > max) g_ptr_array_remove_index(arr, arr->len - 1);
    list_save(name, arr);
    g_ptr_array_free(arr, TRUE);
}

static void list_remove_value(const char *name, const char *value)
{
    GPtrArray *arr = list_load(name);
    for (int i = (int)arr->len - 1; i >= 0; i--)
        if (!strcmp((char *)g_ptr_array_index(arr, i), value)) g_ptr_array_remove_index(arr, i);
    list_save(name, arr);
    g_ptr_array_free(arr, TRUE);
}

/* ------------------------------------------------------------------ */
/* menu helpers                                                        */
/* ------------------------------------------------------------------ */

static GtkWidget *g_mi_preview;     /* View -> Layout -> Preview Pane */
static int g_win_w, g_win_h;        /* window size negotiated by build_ui */static GtkWidget *g_mi_editmode;    /* File -> Edit mode */
static GtkWidget *g_mi_menubar;     /* View -> Layout -> Menu bar */
static GtkWidget *g_fav_menu;
static GtkWidget *g_fav_marker;
static GtkWidget *g_recent_menu;

static void mi_add(GtkWidget *menu, GtkWidget *item)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static GtkWidget *mi_sep(void)
{
    return gtk_separator_menu_item_new();
}

static GtkWidget *mi_sub(const char *label, GtkWidget **out_menu)
{
    GtkWidget *it = gtk_menu_item_new_with_label(label);
    GtkWidget *m = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), m);
    if (out_menu) *out_menu = m;
    return it;
}

static void add_accel(App *app, GtkWidget *item, const char *accel)
{
    if (!accel || !*accel || !app->accel) return;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", accel);
    GdkModifierType mods = 0;
    guint key = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        if (!g_ascii_strcasecmp(tok, "Ctrl")) mods |= GDK_CONTROL_MASK;
        else if (!g_ascii_strcasecmp(tok, "Shift")) mods |= GDK_SHIFT_MASK;
        else if (!g_ascii_strcasecmp(tok, "Alt")) mods |= GDK_MOD1_MASK;
        else if (!g_ascii_strcasecmp(tok, "Left")) key = GDK_KEY_Left;
        else if (!g_ascii_strcasecmp(tok, "Right")) key = GDK_KEY_Right;
        else if (!g_ascii_strcasecmp(tok, "Up")) key = GDK_KEY_Up;
        else if (!g_ascii_strcasecmp(tok, "Down")) key = GDK_KEY_Down;
        else if (!g_ascii_strcasecmp(tok, "Insert")) key = GDK_KEY_Insert;
        else if (!g_ascii_strcasecmp(tok, "Delete")) key = GDK_KEY_Delete;
        else if (!g_ascii_strcasecmp(tok, "Home")) key = GDK_KEY_Home;
        else if (!g_ascii_strcasecmp(tok, "End")) key = GDK_KEY_End;
        else if (!g_ascii_strcasecmp(tok, "PageUp")) key = GDK_KEY_Page_Up;
        else if (!g_ascii_strcasecmp(tok, "PageDown")) key = GDK_KEY_Page_Down;
        else if ((tok[0] == 'F' || tok[0] == 'f') && atoi(tok + 1) >= 1 && atoi(tok + 1) <= 12)
            key = GDK_KEY_F1 + (guint)(atoi(tok + 1) - 1);
        else if (strlen(tok) == 1) key = (guint)g_ascii_tolower(tok[0]);
    }
    if (key) gtk_widget_add_accelerator(item, "activate", app->accel, key, mods, GTK_ACCEL_VISIBLE);
}

static GtkWidget *mi(const char *label, const char *accel, GCallback cb, App *app)
{
    GtkWidget *it = gtk_menu_item_new_with_label(label);
    if (cb) g_signal_connect(it, "activate", cb, app);
    add_accel(app, it, accel);
    return it;
}

static GtkWidget *mi_check(const char *label, gboolean active, GCallback cb, gpointer data)
{
    GtkWidget *it = gtk_check_menu_item_new_with_label(label);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), active);
    if (cb) g_signal_connect(it, "toggled", cb, data);
    return it;
}

static GtkWidget *mi_radio(GtkWidget *group, const char *label, gboolean active,
                           GCallback cb, gpointer data)
{
    GtkWidget *it = gtk_radio_menu_item_new_with_label(
        group ? gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(group)) : NULL, label);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), active);
    if (cb) g_signal_connect(it, "toggled", cb, data);
    return it;
}

static GtkWidget *mi_pane(const char *label, GtkWidget *pane, App *app)
{
    GtkWidget *it = mi_check(label, TRUE, G_CALLBACK(on_toggle_pane), app);
    g_object_set_data(G_OBJECT(it), "pane", pane);
    return it;
}

static void menu_clear_after(GtkWidget *menu, GtkWidget *marker)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
    gboolean after = (marker == NULL);
    for (GList *l = kids; l; l = l->next) {
        if (after) gtk_widget_destroy(GTK_WIDGET(l->data));
        else if (l->data == marker) after = TRUE;
    }
    g_list_free(kids);
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

static void on_quit(GtkWidget *w, gpointer data)
{
    (void)w; (void)data;
    gtk_main_quit();
}

static void on_new_archive(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    msg_dialog(app, GTK_MESSAGE_INFO,
        "Create archive\n\n"
        "Building new .rpf archives is not implemented in this build; it reads "
        "and extracts existing archives.");
}

static void on_editor_info(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    msg_dialog(app, GTK_MESSAGE_INFO,
        "External editors are not implemented.\n\n"
        "Use Edit -> View as text... or View in Hex... to look at the file, or "
        "Edit -> Extract... to write it to disk.");
}

static void on_view_hex(GtkWidget *w, gpointer data)
{
    (void)w;
    open_viewer((App *)data, TRUE);
}

static void on_view_text(GtkWidget *w, gpointer data)
{
    (void)w;
    open_viewer((App *)data, FALSE);
}

static void on_defrag(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    msg_dialog(app, GTK_MESSAGE_INFO,
        "Defragmentation\n\n"
        "Rebuilding an archive so that its payloads are stored in order is not "
        "implemented in this build.");
}

static void on_edit_mode(GtkWidget *w, gpointer data)
{
    App *app = data;
    if (w && GTK_IS_TOGGLE_BUTTON(w) && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)))
        return;
    if (app->edit_toggle) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->edit_toggle), FALSE);
    if (g_mi_editmode && GTK_IS_CHECK_MENU_ITEM(g_mi_editmode))
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_mi_editmode), FALSE);
    msg_dialog(app, GTK_MESSAGE_INFO,
        "Edit mode\n\n"
        "Adding, replacing, renaming and deleting files needs edit mode, which "
        "this build does not implement: archives are opened read only.\n\n"
        "Use Edit -> Extract... to copy the selected file out, or the tool bar's "
        "Extract all to copy the whole folder.");
}

static void on_select_all(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    gtk_tree_selection_select_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(app->list)));
}

static void current_dir_rel(App *app, char *buf, size_t len)
{
    buf[0] = 0;
    if (!has_archive(app)) return;
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    int dirIdx = app->cur.entry;
    if (dirIdx < 0 || dirIdx >= (int)a->entryCount) dirIdx = 0;
    if (!a->entries[dirIdx].isDir) dirIdx = find_parent(a, dirIdx);
    if (dirIdx < 0) dirIdx = 0;
    rpf_entry_path(a, (uint32_t)dirIdx, buf, len);
}

static int selected_entry_or_msg(App *app)
{
    if (!has_archive(app)) { msg_dialog(app, GTK_MESSAGE_INFO, "No open archive."); return -1; }
    int isdir = 0;
    int entry = selected_entry(app, &isdir);
    if (entry < 0) { msg_dialog(app, GTK_MESSAGE_INFO, "No selected items."); return -1; }
    return entry;
}

static void on_copy_name(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    int entry = selected_entry_or_msg(app);
    if (entry < 0) return;
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    copy_to_clipboard(app, a->entries[entry].name);
}

static void on_copy_path(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    int entry = selected_entry_or_msg(app);
    if (entry < 0) return;
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    char rel[RPF_PATH_MAX];
    rpf_entry_path(a, (uint32_t)entry, rel, sizeof rel);
    copy_to_clipboard(app, rel);
}

/* DFM: PathBar's PathDownMenu, "Copy Address (Path)". */
static void on_copy_address(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (!has_archive(app)) return;
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    char rel[RPF_PATH_MAX], full[RPF_PATH_MAX * 2];
    current_dir_rel(app, rel, sizeof rel);
    if (rel[0]) snprintf(full, sizeof full, "%.*s/%s", RPF_PATH_MAX - 1, a->path, rel);
    else snprintf(full, sizeof full, "%.*s", RPF_PATH_MAX * 2 - 1, a->path);
    copy_to_clipboard(app, full);
}

static void on_filter_focus(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    gtk_widget_grab_focus(app->filter);
}

static void on_filter_clear(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    gtk_entry_set_text(GTK_ENTRY(app->filter), "");
}

static void close_archive(App *app, int idx)
{
    if (idx < 0 || idx >= (int)app->archives->len) return;
    RpfArchive *a = g_ptr_array_index(app->archives, idx);

    /* drop the tree row of this archive, then renumber the rows behind it */
    GtkTreeModel *tm = GTK_TREE_MODEL(app->tree_store);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(tm, &it)) {
        do {
            int ai = -1;
            gtk_tree_model_get(tm, &it, COL_ISDIR, &ai, -1);
            if (ai == idx) { gtk_tree_store_remove(app->tree_store, &it); break; }
        } while (gtk_tree_model_iter_next(tm, &it));
    }
    if (gtk_tree_model_get_iter_first(tm, &it)) {
        do {
            int ai = -1;
            gtk_tree_model_get(tm, &it, COL_ISDIR, &ai, -1);
            if (ai > idx) gtk_tree_store_set(app->tree_store, &it, COL_ISDIR, ai - 1, -1);
        } while (gtk_tree_model_iter_next(tm, &it));
    }

    g_ptr_array_remove_index(app->archives, idx);
    rpf_close(a);
    g_ptr_array_set_size(app->back, 0);
    g_ptr_array_set_size(app->fwd, 0);
    app->cur.archive = -1;
    app->cur.entry = -1;
    if (app->archives->len > 0) nav_to(app, 0, 0, FALSE);
    else refresh_list(app);
}

static void on_close_archive(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (has_archive(app)) close_archive(app, app->cur.archive);
}

static void on_close_all(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    while (app->archives->len > 0) close_archive(app, (int)app->archives->len - 1);
    gtk_tree_store_clear(app->tree_store);
    app->cur.archive = -1;
    app->cur.entry = -1;
    refresh_list(app);
}

static void on_open_recent(GtkWidget *w, gpointer data)
{
    App *app = data;
    const char *path = g_object_get_data(G_OBJECT(w), "path");
    if (path) open_archive_path(app, path, TRUE);
}

/* Favorites and recent archives are read from ~/.config/openvi/ every time the
 * menu is (re)built, like OpenIV refreshes its own lists. */
static void fill_list_menu(App *app, GtkWidget *menu, GtkWidget *marker,
                           const char *file, const char *empty_text)
{
    menu_clear_after(menu, marker);
    GPtrArray *paths = list_load(file);
    if (paths->len == 0) {
        GtkWidget *it = gtk_menu_item_new_with_label(empty_text);
        gtk_widget_set_sensitive(it, FALSE);
        mi_add(menu, it);
    } else {
        for (guint i = 0; i < paths->len; i++) {
            const char *path = (const char *)g_ptr_array_index(paths, i);
            char *base = g_path_get_basename(path);
            GtkWidget *it = gtk_menu_item_new_with_label(base);
            gtk_widget_set_tooltip_text(it, path);
            g_object_set_data_full(G_OBJECT(it), "path", g_strdup(path), g_free);
            g_signal_connect(it, "activate", G_CALLBACK(on_open_recent), app);
            mi_add(menu, it);
            g_free(base);
        }
    }
    g_ptr_array_free(paths, TRUE);
    gtk_widget_show_all(menu);
}

static void rebuild_dynamic_menus(App *app)
{
    if (g_fav_menu)
        fill_list_menu(app, g_fav_menu, g_fav_marker, "favorites", "(no favorites)");
    if (g_recent_menu)
        fill_list_menu(app, g_recent_menu, NULL, "recent", "(no recent archives)");
}

static void on_favorites_add(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (!has_archive(app)) { msg_dialog(app, GTK_MESSAGE_INFO, "No open archive."); return; }
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    list_add("favorites", a->path, 20);
    rebuild_dynamic_menus(app);
    notify(app, GTK_MESSAGE_INFO, "Pinned to favorites: %s", a->path);
}

typedef struct { App *app; GtkWidget *listbox; } FavOrg;

static void fav_fill(FavOrg *fo)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(fo->listbox));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    GPtrArray *fav = list_load("favorites");
    for (guint i = 0; i < fav->len; i++) {
        const char *path = (const char *)g_ptr_array_index(fav, i);
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lbl = gtk_label_new(path);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_margin_start(lbl, 6);
        gtk_container_add(GTK_CONTAINER(row), lbl);
        g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path), g_free);
        gtk_list_box_insert(GTK_LIST_BOX(fo->listbox), row, -1);
    }
    g_ptr_array_free(fav, TRUE);
    gtk_widget_show_all(fo->listbox);
}

static void on_fav_remove(GtkWidget *w, gpointer data)
{
    FavOrg *fo = data;
    (void)w;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(fo->listbox));
    if (!row) return;
    const char *path = g_object_get_data(G_OBJECT(row), "path");
    if (path) list_remove_value("favorites", path);
    fav_fill(fo);
    rebuild_dynamic_menus(fo->app);
}

static void on_favorites_organize(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    GtkWidget *d = gtk_dialog_new_with_buttons("Organize favorites", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, "Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v), 10);
    gtk_box_pack_start(GTK_BOX(box), v, TRUE, TRUE, 0);

    FavOrg fo;
    fo.app = app;
    fo.listbox = gtk_list_box_new();
    gtk_widget_set_size_request(fo.listbox, 460, 240);
    gtk_box_pack_start(GTK_BOX(v), fo.listbox, TRUE, TRUE, 0);
    GtkWidget *remove = gtk_button_new_with_label("Remove");
    gtk_widget_set_halign(remove, GTK_ALIGN_END);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_fav_remove), &fo);
    gtk_box_pack_start(GTK_BOX(v), remove, FALSE, FALSE, 0);
    fav_fill(&fo);

    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

typedef struct { GtkWidget *entry, *l1, *l2, *l3; } HashDlg;

static void hash_update(GtkEditable *e, gpointer data)
{
    HashDlg *h = data;
    (void)e;
    const char *s = gtk_entry_get_text(GTK_ENTRY(h->entry));
    char buf[160];
    uint32_t j = rage_joaat(s);
    snprintf(buf, sizeof buf, "joaat          0x%08X", j);
    gtk_label_set_text(GTK_LABEL(h->l1), buf);
    snprintf(buf, sizeof buf, "joaat (decimal) %u", j);
    gtk_label_set_text(GTK_LABEL(h->l2), buf);
    snprintf(buf, sizeof buf, "CRC32          0x%08X",
             (unsigned)crc32(0L, (const Bytef *)s, (uInt)strlen(s)));
    gtk_label_set_text(GTK_LABEL(h->l3), buf);
}

static void on_hash_generator(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    GtkWidget *d = gtk_dialog_new_with_buttons("Hash generator", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, "Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v), 12);
    gtk_box_pack_start(GTK_BOX(box), v, TRUE, TRUE, 0);

    HashDlg h;
    h.entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(h.entry), "file or archive name");
    gtk_box_pack_start(GTK_BOX(v), h.entry, FALSE, FALSE, 0);
    h.l1 = gtk_label_new("");
    h.l2 = gtk_label_new("");
    h.l3 = gtk_label_new("");
    GtkWidget *labels[3] = { h.l1, h.l2, h.l3 };
    for (int i = 0; i < 3; i++) {
        gtk_widget_set_halign(labels[i], GTK_ALIGN_START);
        gtk_label_set_selectable(GTK_LABEL(labels[i]), TRUE);
        gtk_box_pack_start(GTK_BOX(v), labels[i], FALSE, FALSE, 0);
    }
    g_signal_connect(h.entry, "changed", G_CALLBACK(hash_update), &h);
    hash_update(NULL, &h);

    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void on_options(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    GtkWidget *d = gtk_dialog_new_with_buttons("Options", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, "Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start(GTK_BOX(box), grid, TRUE, TRUE, 0);

    info_grid_field(grid, 0, "Version:", APP_VERSION);
    info_grid_field(grid, 1, "encryption_key.bin:",
                    rpf_key_available() ? rpf_key_path() : "not found");
    info_grid_field(grid, 2, "ng_keys.bin:",
                    rpf_ng_key_count() ? rpf_ng_key_path() : "not found");
    info_grid_field(grid, 3, "iaes2.bin:",
                    rpf_wbc_available() ? rpf_wbc_path() : "not found");

    GtkWidget *note = gtk_label_new(
        "Key material is never bundled with this build.  The files are looked up next to the\n"
        "archive, in the working directory, next to the executable and in ~/.config/openvi/.\n"
        "docs/KEYS.md explains where the files come from.");
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_widget_set_margin_top(note, 10);
    gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);

    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void on_help_verify(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    msg_dialog(app, GTK_MESSAGE_INFO,
        "How to verify and restore game files\n\n"
        "Ask your store client to check the game files:\n\n"
        "  Steam      Library -> Grand Theft Auto V -> Properties ->\n"
        "             Installed Files -> Verify integrity of game files\n\n"
        "  Rockstar   Launcher -> Settings -> Verify game file integrity\n\n"
        "Damaged archives are downloaded again.");
}

static void on_web(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    GError *err = NULL;
    if (!gtk_show_uri_on_window(GTK_WINDOW(app->window), "https://openiv.com",
                                GDK_CURRENT_TIME, &err)) {
        msg_dialog(app, GTK_MESSAGE_INFO, "Cannot open a browser: %s",
                   err ? err->message : "unknown error");
        if (err) g_error_free(err);
    }
}

static void on_toggle_pane(GtkCheckMenuItem *mi_item, gpointer data)
{
    (void)data;
    GtkWidget *pane = g_object_get_data(G_OBJECT(mi_item), "pane");
    if (pane) gtk_widget_set_visible(pane, gtk_check_menu_item_get_active(mi_item));
}

static GtkWidget *g_view_radios[5];
static GtkWidget *g_tb_radios[5];
static gboolean g_syncing;

static void on_view_mode_menu(GtkCheckMenuItem *mi_item, gpointer data)
{
    App *app = data;
    if (g_syncing) return;
    if (!gtk_check_menu_item_get_active(mi_item)) return;
    app->view_mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi_item), "mode"));
    apply_view_mode(app);
    g_syncing = TRUE;
    for (int i = 0; i < 5; i++) {
        if (g_view_radios[i])
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_view_radios[i]),
                                           i == app->view_mode);
        if (g_tb_radios[i])
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tb_radios[i]),
                                           i == app->view_mode);
    }
    g_syncing = FALSE;
}

static void view_mode_items(App *app, GtkWidget *menu, GtkWidget **slots)
{
    static const char *names[5] = { "Icon", "Small icon", "List", "Details", "Tile" };
    GtkWidget *group = NULL;
    for (int i = 0; i < 5; i++) {
        GtkWidget *it = mi_radio(group, names[i], app->view_mode == i,
                                 G_CALLBACK(on_view_mode_menu), app);
        g_object_set_data(G_OBJECT(it), "mode", GINT_TO_POINTER(i));
        if (!group) group = it;
        if (slots) slots[i] = it;
        mi_add(menu, it);
    }
}

static void on_sort_menu(GtkCheckMenuItem *mi_item, gpointer data)
{
    App *app = data;
    if (!gtk_check_menu_item_get_active(mi_item)) return;
    set_sort(app, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi_item), "col")));
}

static void on_toolbar_preview(GtkToolButton *b, gpointer data)
{
    (void)b; (void)data;
    if (g_mi_preview)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_mi_preview),
            !gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(g_mi_preview)));
}

static void on_infobar_response(GtkInfoBar *bar, gint resp, gpointer data)
{
    (void)resp; (void)data;
    gtk_widget_hide(GTK_WIDGET(bar));
}

/* Right click menu of the file list (OpenIV's FilesMenu). */
static gboolean on_list_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    App *app = data;
    (void)w;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->list));
    GtkTreePath *path = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(app->list), (gint)ev->x, (gint)ev->y,
                                      &path, NULL, NULL, NULL)) {
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
        gtk_tree_path_free(path);
    }

    int isdir = 0;
    int entry = has_archive(app) ? selected_entry(app, &isdir) : -1;
    gboolean have = (entry >= 0);

    GtkWidget *m = gtk_menu_new();
    GtkWidget *it;
    it = mi("Open", NULL, G_CALLBACK(on_view_text), app);
    gtk_widget_set_sensitive(it, have && !isdir);
    mi_add(m, it);
    it = mi("View in Hex...", NULL, G_CALLBACK(on_view_hex), app);
    gtk_widget_set_sensitive(it, have && !isdir);
    mi_add(m, it);
    it = mi("View as text...", NULL, G_CALLBACK(on_view_text), app);
    gtk_widget_set_sensitive(it, have && !isdir);
    mi_add(m, it);
    mi_add(m, mi_sep());
    it = mi("Extract...", NULL, G_CALLBACK(on_extract_clicked), app);
    mi_add(m, it);
    it = mi("Extract all...", NULL, G_CALLBACK(on_extract_all_clicked), app);
    mi_add(m, it);
    mi_add(m, mi_sep());
    it = mi("Copy name", NULL, G_CALLBACK(on_copy_name), app);
    gtk_widget_set_sensitive(it, have);
    mi_add(m, it);
    it = mi("Copy path", NULL, G_CALLBACK(on_copy_path), app);
    gtk_widget_set_sensitive(it, have);
    mi_add(m, it);
    mi_add(m, mi_sep());
    it = mi("Properties", NULL, G_CALLBACK(on_properties), app);
    gtk_widget_set_sensitive(it, have);
    mi_add(m, it);

    gtk_widget_show_all(m);
    gtk_menu_popup_at_pointer(GTK_MENU(m), (GdkEvent *)ev);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* menu bar and tool bar (DFM: MainMenuBar, ToolBar, ViewToolBar)      */
/* ------------------------------------------------------------------ */

static GtkWidget *build_menubar(App *app)
{
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *m = NULL, *m2 = NULL, *item = NULL;

    /* ---------- File ---------- */
    item = mi_sub("File", &m);
    mi_add(m, mi("Create archive...", "Ctrl+Shift+N", G_CALLBACK(on_new_archive), app));
    mi_add(m, mi_sep());
    mi_add(m, mi("Open game folder", "Ctrl+Shift+O", G_CALLBACK(on_games_clicked), app));
    mi_add(m, mi("Open archive...", "Ctrl+O", G_CALLBACK(on_open_clicked), app));
    mi_add(m, mi("Open folder...", "Shift+Alt+O", G_CALLBACK(on_open_folder_clicked), app));
    mi_add(m, mi_sub("Recent Archives", &g_recent_menu));
    mi_add(m, mi_sep());
    mi_add(m, mi("Archive properties", NULL, G_CALLBACK(on_archive_properties), app));
    mi_add(m, mi("Defragmentation", "Ctrl+Shift+R", G_CALLBACK(on_defrag), app));
    g_mi_editmode = mi("Edit mode", "F6", G_CALLBACK(on_edit_mode), app);
    mi_add(m, g_mi_editmode);
    GtkWidget *ro = mi_check("Read only mode", TRUE, NULL, app);
    gtk_widget_set_sensitive(ro, FALSE);
    mi_add(m, ro);
    mi_add(m, mi_sep());
    mi_add(m, mi("Close archive", "Ctrl+F4", G_CALLBACK(on_close_archive), app));
    mi_add(m, mi("Close all archives", NULL, G_CALLBACK(on_close_all), app));
    mi_add(m, mi_sep());
    mi_add(m, mi("Exit", "Alt+F4", G_CALLBACK(on_quit), app));
    mi_add(bar, item);

    /* ---------- Edit ---------- */
    item = mi_sub("Edit", &m);
    mi_add(m, mi("Open", NULL, G_CALLBACK(on_editor_info), app));
    mi_add(m, mi("Edit", NULL, G_CALLBACK(on_editor_info), app));
    mi_add(m, mi("View in Hex...", NULL, G_CALLBACK(on_view_hex), app));
    mi_add(m, mi("View as text...", NULL, G_CALLBACK(on_view_text), app));
    mi_add(m, mi_sep());
    mi_add(m, mi("Copy name", "Ctrl+C", G_CALLBACK(on_copy_name), app));
    mi_add(m, mi("Copy path", NULL, G_CALLBACK(on_copy_path), app));
    mi_add(m, mi_sep());
    mi_add(m, mi("Extract...", "Ctrl+E", G_CALLBACK(on_extract_clicked), app));
    mi_add(m, mi("Extract all...", NULL, G_CALLBACK(on_extract_all_clicked), app));
    mi_add(m, mi_sep());
    /* writing needs edit mode, which this build does not implement */
    static const char *edit_items[] = { "Add", "Rename", "Replace", "Delete", NULL };
    for (int i = 0; edit_items[i]; i++) {
        GtkWidget *ei = mi(edit_items[i], NULL, NULL, app);
        gtk_widget_set_sensitive(ei, FALSE);
        mi_add(m, ei);
    }
    mi_add(m, mi_sep());
    mi_add(m, mi("Select all", "Ctrl+A", G_CALLBACK(on_select_all), app));
    mi_add(m, mi_sep());
    GtkWidget *filter = mi_sub("Filter", &m2);
    mi_add(m2, mi("Filter by name", "Ctrl+F", G_CALLBACK(on_filter_focus), app));
    mi_add(m2, mi("Clear filter", NULL, G_CALLBACK(on_filter_clear), app));
    mi_add(m, filter);
    mi_add(bar, item);

    /* ---------- New ---------- */
    item = mi_sub("New", &m);
    static const char *new_items[] = { "Add", "New folder", "Import openFormats...", NULL };
    for (int i = 0; new_items[i]; i++) {
        GtkWidget *ni = mi(new_items[i], NULL, NULL, app);
        gtk_widget_set_sensitive(ni, FALSE);
        mi_add(m, ni);
    }
    mi_add(bar, item);

    /* ---------- View ---------- */
    item = mi_sub("View", &m);
    GtkWidget *layout = mi_sub("Layout", &m2);
    g_mi_menubar = mi_pane("Menu bar", NULL, app);
    add_accel(app, g_mi_menubar, "F10");
    mi_add(m2, g_mi_menubar);
    mi_add(m2, mi_sep());
    mi_add(m2, mi_pane("Directory Tree", app->tree_sw, app));
    mi_add(m2, mi_pane("Tool Bar", app->toolbar, app));
    mi_add(m2, mi_pane("Search and Address bar", app->navband, app));
    mi_add(m2, mi_pane("Status Bar", app->statusbar, app));
    g_mi_preview = mi_pane("Preview Pane", app->preview, app);
    mi_add(m2, g_mi_preview);
    mi_add(m, layout);

    GtkWidget *sort = mi_sub("Sort by", &m2);
    struct { const char *label; int col; } sorts[] = {
        { "None", -1 }, { "Name", COL_NAME }, { "Type", COL_TYPE }, { "Size", COL_SIZE },
        { "Compressed", COL_PACKED }, { "Attributes", COL_ATTR }
    };
    GtkWidget *group = NULL;
    for (size_t i = 0; i < sizeof sorts / sizeof sorts[0]; i++) {
        GtkWidget *si = mi_radio(group, sorts[i].label, FALSE, G_CALLBACK(on_sort_menu), app);
        g_object_set_data(G_OBJECT(si), "col", GINT_TO_POINTER(sorts[i].col));
        if (!group) group = si;
        mi_add(m2, si);
    }
    mi_add(m, sort);
    mi_add(m, mi_sep());

    view_mode_items(app, m, g_view_radios);
    mi_add(m, mi_sep());

    GtkWidget *go = mi_sub("Go To", &m2);
    mi_add(m2, mi("Back", "Alt+Left", G_CALLBACK(on_back), app));
    mi_add(m2, mi("Forward", "Alt+Right", G_CALLBACK(on_forward), app));
    mi_add(m2, mi("Up One level", "Alt+Up", G_CALLBACK(on_up), app));
    mi_add(m, go);
    mi_add(bar, item);

    /* ---------- Favorites ---------- */
    item = mi_sub("Favorites", &m);
    g_fav_menu = m;
    mi_add(m, mi("Add to favorites...", NULL, G_CALLBACK(on_favorites_add), app));
    mi_add(m, mi("Organize favorites...", NULL, G_CALLBACK(on_favorites_organize), app));
    g_fav_marker = mi_sep();
    mi_add(m, g_fav_marker);
    mi_add(bar, item);

    /* ---------- Tools ---------- */
    item = mi_sub("Tools", &m);
    mi_add(m, mi("Options...", NULL, G_CALLBACK(on_options), app));
    mi_add(m, mi_sep());
    mi_add(m, mi("Archive properties", NULL, G_CALLBACK(on_archive_properties), app));
    mi_add(m, mi("Hash generator...", NULL, G_CALLBACK(on_hash_generator), app));
    mi_add(m, mi("Defragmentation", NULL, G_CALLBACK(on_defrag), app));
    mi_add(bar, item);

    /* ---------- Help ---------- */
    item = mi_sub("Help", &m);
    mi_add(m, mi("How to verify and restore game files...", NULL, G_CALLBACK(on_help_verify), app));
    mi_add(m, mi_sep());
    mi_add(m, mi("Go to website...", NULL, G_CALLBACK(on_web), app));
    mi_add(m, mi_sep());
    mi_add(m, mi("About...", "F1", G_CALLBACK(on_about), app));
    mi_add(bar, item);

    return bar;
}

static GtkToolItem *tb_item(App *app, const char *icon, const char *tip, GCallback cb)
{
    GtkToolItem *it = gtk_tool_button_new(NULL, NULL);
    gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(it),
                                    gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_widget_set_tooltip_text(GTK_WIDGET(it), tip);
    if (cb) g_signal_connect(it, "clicked", cb, app);
    return it;
}

static GtkWidget *build_toolbar(App *app)
{
    GtkWidget *bar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(bar), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(bar), GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_margin_start(bar, 2);
    gtk_widget_set_margin_top(bar, 2);
    gtk_widget_set_margin_bottom(bar, 2);

    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "document-open", "Open archive... (Ctrl+O)", G_CALLBACK(on_open_clicked)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "folder-open", "Open folder...", G_CALLBACK(on_open_folder_clicked)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "applications-games", "Open game folder", G_CALLBACK(on_games_clicked)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), gtk_separator_tool_item_new(), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "go-previous", "Back (Alt+Left)", G_CALLBACK(on_back)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "go-next", "Forward (Alt+Right)", G_CALLBACK(on_forward)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "go-up", "Up One level (Alt+Up)", G_CALLBACK(on_up)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), gtk_separator_tool_item_new(), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "document-save-as", "Extract...", G_CALLBACK(on_extract_clicked)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "document-save", "Extract all...", G_CALLBACK(on_extract_all_clicked)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), gtk_separator_tool_item_new(), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "document-properties", "Properties", G_CALLBACK(on_properties)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "window-close", "Close archive (Ctrl+F4)", G_CALLBACK(on_close_archive)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "view-refresh", "Refresh", G_CALLBACK(refresh_cb)), -1);

    /* the right hand side group is OpenIV's ViewToolBar */
    GtkToolItem *sp = gtk_separator_tool_item_new();
    gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(sp), FALSE);
    gtk_tool_item_set_expand(sp, TRUE);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), sp, -1);

    GtkToolItem *view = gtk_menu_tool_button_new(NULL, NULL);
    gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(view),
                                    gtk_image_new_from_icon_name("view-list", GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_widget_set_tooltip_text(GTK_WIDGET(view), "View mode");
    GtkWidget *vm = gtk_menu_new();
    view_mode_items(app, vm, g_tb_radios);
    gtk_menu_tool_button_set_menu(GTK_MENU_TOOL_BUTTON(view), vm);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), view, -1);

    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "document-print-preview", "Preview pane",
                G_CALLBACK(on_toolbar_preview)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "preferences-system", "Options...", G_CALLBACK(on_options)), -1);
    gtk_toolbar_insert(GTK_TOOLBAR(bar),
        tb_item(app, "help-about", "About... (F1)", G_CALLBACK(on_about)), -1);

    return bar;
}

static gboolean on_window_map(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    App *app = data;
    (void)w; (void)ev;
    int W = gtk_widget_get_allocated_width(app->window);
    if (W <= 1) return FALSE;
    gtk_paned_set_position(GTK_PANED(app->paned), 220);              /* DFM: treeHost 220 */
    gtk_paned_set_position(GTK_PANED(app->paned2), W - 220 - 200);   /* DFM: panePreView 200 */
    return FALSE;
}

static void build_ui(App *app)
{
    /* The address band, the live preview pane and the status bar get the flat
     * look of the DFM forms; everything else follows the user's GTK theme. */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        ".addressbar { border: 1px solid alpha(@theme_fg_color, 0.28);"
        " border-radius: 3px; background-color: alpha(@theme_base_color, 0.6); }\n"
        ".addressbar button { border: none; background: none; box-shadow: none;"
        " padding: 1px 5px; }\n"
        ".addressbar button:hover { background-color: alpha(@theme_fg_color, 0.1);"
        " border-radius: 3px; }\n"
        ".livepreview { background-color: @theme_base_color;"
        " border-left: 1px solid alpha(@theme_fg_color, 0.2); }\n"
        ".statusbar label { padding: 1px 6px; }\n", -1, NULL);
    GdkScreen *cscreen = gdk_screen_get_default();
    if (cscreen)
        gtk_style_context_add_provider_for_screen(cscreen,
            GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    app->accel = gtk_accel_group_new();
    app->view_mode = 3;    /* Details, OpenIV's default */
    app->sort_col = -1;

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "OpenIV");
    gtk_window_add_accel_group(GTK_WINDOW(app->window), app->accel);

    /* fit the desktop, OpenIV's design size is 1120x700 */
    int win_w = 1180, win_h = 700;
    GdkScreen *scr = gdk_screen_get_default();
    GdkMonitor *mon = NULL;
    if (scr) {
        GdkDisplay *dpy = gdk_screen_get_display(scr);
        if (dpy) {
            mon = gdk_display_get_primary_monitor(dpy);
            if (!mon) mon = gdk_display_get_monitor(dpy, 0);
        }
    }
    if (mon) {
        GdkRectangle geo;
        gdk_monitor_get_geometry(mon, &geo);
        if (geo.width > 40 && win_w > geo.width - 60) win_w = geo.width - 60;
        if (geo.height > 40 && win_h > geo.height - 80) win_h = geo.height - 80;
    }
    if (win_w < 760) win_w = 760;
    if (win_h < 520) win_h = 520;
    gtk_window_set_default_size(GTK_WINDOW(app->window), win_w, win_h);
    /* also resize: without a window manager (bare X, some remote sessions) GTK
     * would otherwise map the window at its natural size instead */
    gtk_window_resize(GTK_WINDOW(app->window), win_w, win_h);
    g_win_w = win_w;
    g_win_h = win_h;
    gtk_widget_set_size_request(app->window, 780, 500);
    gtk_window_set_position(GTK_WINDOW(app->window), GTK_WIN_POS_CENTER);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), vbox);

    /* --- navigation band (DFM: NavPane, back/forward + address bar) */
    app->navband = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_start(app->navband, 4);
    gtk_widget_set_margin_end(app->navband, 4);
    gtk_widget_set_margin_top(app->navband, 4);
    gtk_widget_set_margin_bottom(app->navband, 4);
    gtk_box_pack_start(GTK_BOX(vbox), app->navband, FALSE, FALSE, 0);

    struct { const char *icon, *tip; GCallback cb; } nav[] = {
        { "go-previous", "Back (Alt+Left)", G_CALLBACK(on_back) },
        { "go-next", "Forward (Alt+Right)", G_CALLBACK(on_forward) },
        { "go-up", "Up One level (Alt+Up)", G_CALLBACK(on_up) }
    };
    for (size_t i = 0; i < sizeof nav / sizeof nav[0]; i++) {
        GtkWidget *b = gtk_button_new_from_icon_name(nav[i].icon, GTK_ICON_SIZE_SMALL_TOOLBAR);
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(b, nav[i].tip);
        g_signal_connect(b, "clicked", nav[i].cb, app);
        gtk_box_pack_start(GTK_BOX(app->navband), b, FALSE, FALSE, 0);
    }

    /* DFM: AddressBand + PathBar, the breadcrumb of the current folder */
    GtkWidget *addr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(addr), "addressbar");
    gtk_box_pack_start(GTK_BOX(app->navband), addr, TRUE, TRUE, 2);
    app->crumbs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(addr), app->crumbs, TRUE, TRUE, 2);
    {
        /* DFM: PathDownMenu */
        GtkWidget *mb = gtk_menu_button_new();
        gtk_button_set_relief(GTK_BUTTON(mb), GTK_RELIEF_NONE);
        gtk_button_set_image(GTK_BUTTON(mb),
            gtk_image_new_from_icon_name("pan-down-symbolic", GTK_ICON_SIZE_MENU));
        gtk_widget_set_tooltip_text(mb, "Copy Address (Path)");
        GtkWidget *pm = gtk_menu_new();
        mi_add(pm, mi("Copy Address (Path)", NULL, G_CALLBACK(on_copy_address), app));
        gtk_menu_button_set_popup(GTK_MENU_BUTTON(mb), pm);
        gtk_box_pack_end(GTK_BOX(addr), mb, FALSE, FALSE, 0);
    }

    /* DFM: tbCmd, the edit mode switch - this build is read only */
    app->edit_toggle = gtk_toggle_button_new_with_label("Edit mode");
    gtk_widget_set_tooltip_text(app->edit_toggle, "Toggle edit mode (F6)");
    gtk_button_set_image(GTK_BUTTON(app->edit_toggle),
        gtk_image_new_from_icon_name("document-edit", GTK_ICON_SIZE_MENU));
    g_signal_connect(app->edit_toggle, "toggled", G_CALLBACK(on_edit_mode), app);
    gtk_box_pack_start(GTK_BOX(app->navband), app->edit_toggle, FALSE, FALSE, 2);

    /* DFM: SearchBox */
    gtk_box_pack_start(GTK_BOX(app->navband),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
    app->filter = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->filter), "Filter");
    gtk_widget_set_size_request(app->filter, 170, -1);
    g_signal_connect(app->filter, "changed", G_CALLBACK(on_filter_changed), app);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(app->filter), GTK_ENTRY_ICON_PRIMARY, "edit-find");
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(app->filter), GTK_ENTRY_ICON_SECONDARY, "edit-clear");
    g_signal_connect_swapped(app->filter, "icon-release", G_CALLBACK(on_filter_clear), app);
    gtk_box_pack_start(GTK_BOX(app->navband), app->filter, FALSE, FALSE, 0);

    /* --- tool bar (the menu bar is packed above this one below) --- */
    app->toolbar = build_toolbar(app);
    gtk_box_pack_start(GTK_BOX(vbox), app->toolbar, FALSE, FALSE, 0);

    /* --- content: archive tree | (file list | live preview) ------- */
    app->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), app->paned, TRUE, TRUE, 0);

    app->tree_store = gtk_tree_store_new(COL_NCOLS,
                                         G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                         G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                         G_TYPE_INT64, G_TYPE_INT64, G_TYPE_INT, G_TYPE_INT);
    app->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->tree_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app->tree), FALSE);
    gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(app->tree), FALSE);
    {
        GtkTreeViewColumn *tc = gtk_tree_view_column_new();
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        g_object_set(pix, "stock-size", GTK_ICON_SIZE_MENU, NULL);
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(tc), pix, FALSE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(tc), pix, "icon-name", COL_ICON, NULL);
        GtkCellRenderer *txt = gtk_cell_renderer_text_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(tc), txt, TRUE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(tc), txt, "text", COL_NAME, NULL);
        gtk_tree_view_column_set_expand(tc, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(app->tree), tc);
    }
    app->tree_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(app->tree_sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(app->tree_sw), app->tree);
    gtk_paned_pack1(GTK_PANED(app->paned), app->tree_sw, FALSE, FALSE);
    gtk_widget_set_size_request(app->tree_sw, 200, -1);

    app->paned2 = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack2(GTK_PANED(app->paned), app->paned2, TRUE, FALSE);

    /* DFM: contentPane = notifications on top, ArchiveList below */
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    app->infobar = gtk_info_bar_new();
    gtk_info_bar_set_show_close_button(GTK_INFO_BAR(app->infobar), TRUE);
    app->notify_label = gtk_label_new("");
    gtk_widget_set_halign(app->notify_label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(app->notify_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(app->notify_label, 6);
    gtk_box_pack_start(GTK_BOX(gtk_info_bar_get_content_area(GTK_INFO_BAR(app->infobar))),
                       app->notify_label, TRUE, TRUE, 0);
    g_signal_connect(app->infobar, "response", G_CALLBACK(on_infobar_response), app);
    gtk_widget_set_no_show_all(app->infobar, TRUE);
    gtk_widget_hide(app->infobar);
    gtk_box_pack_start(GTK_BOX(content), app->infobar, FALSE, FALSE, 0);

    app->list_store = gtk_list_store_new(COL_NCOLS,
                                         G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                         G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                         G_TYPE_INT64, G_TYPE_INT64, G_TYPE_INT, G_TYPE_INT);
    app->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->list_store));
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(app->list)),
                                GTK_SELECTION_MULTIPLE);
    /* OpenIV's ArchiveList draws the horizontal row lines of vsReport */
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(app->list), GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    app->col[0] = add_list_column(app, "Name", COL_NAME, 165, TRUE, 0.0f);
    app->col[1] = add_list_column(app, "Type", COL_TYPE, 80, FALSE, 0.0f);
    app->col[2] = add_list_column(app, "Size", COL_SIZE, 70, FALSE, 1.0f);
    app->col[3] = add_list_column(app, "Attributes", COL_ATTR, 90, FALSE, 0.0f);
    app->col[4] = add_list_column(app, "Compressed", COL_PACKED, 95, FALSE, 1.0f);
    gtk_tree_view_column_set_expand(app->col[0], TRUE);
    {
        int sortable[] = { COL_NAME, COL_TYPE, COL_SIZE, COL_ATTR, COL_PACKED };
        for (size_t i = 0; i < sizeof sortable / sizeof sortable[0]; i++)
            gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(app->list_store), sortable[i],
                                            list_sort_cmp, GINT_TO_POINTER(sortable[i]), NULL);
    }
    GtkWidget *lsw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(lsw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(lsw), app->list);
    gtk_box_pack_start(GTK_BOX(content), lsw, TRUE, TRUE, 0);
    gtk_paned_pack1(GTK_PANED(app->paned2), content, TRUE, FALSE);

    /* DFM: panePreView, the live preview on the right */
    GtkWidget *preview = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(preview), "livepreview");
    gtk_widget_set_size_request(preview, 200, -1);
    app->preview_stack = gtk_stack_new();
    app->preview_label = gtk_label_new("Select file to preview.");
    gtk_label_set_justify(GTK_LABEL(app->preview_label), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(app->preview_label), TRUE);
    gtk_widget_set_valign(app->preview_label, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(app->preview_label, GTK_ALIGN_FILL);
    gtk_widget_set_margin_start(app->preview_label, 10);
    gtk_widget_set_margin_end(app->preview_label, 10);
    gtk_stack_add_named(GTK_STACK(app->preview_stack), app->preview_label, "message");
    {
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_NONE);
        app->preview_view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(app->preview_view), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->preview_view), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->preview_view), TRUE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->preview_view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->preview_view), 4);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->preview_view), 4);
        gtk_container_add(GTK_CONTAINER(sw), app->preview_view);
        gtk_stack_add_named(GTK_STACK(app->preview_stack), sw, "text");
    }
    {
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_NONE);
        app->preview_image = gtk_image_new();
        gtk_container_add(GTK_CONTAINER(sw), app->preview_image);
        gtk_stack_add_named(GTK_STACK(app->preview_stack), sw, "image");
    }
    gtk_box_pack_start(GTK_BOX(preview), app->preview_stack, TRUE, TRUE, 0);
    app->preview = preview;
    gtk_paned_pack2(GTK_PANED(app->paned2), preview, FALSE, FALSE);

    /* --- status bar (DFM: AeroStatusBar) -------------------------- */
    app->statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->statusbar), "statusbar");
    app->status_items = status_label("No open archive", TRUE);
    app->status_selected = status_label("", FALSE);
    app->status_archive = gtk_button_new_with_label("");
    gtk_button_set_relief(GTK_BUTTON(app->status_archive), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(app->status_archive, "Archive properties");
    g_signal_connect(app->status_archive, "clicked", G_CALLBACK(on_archive_properties), app);
    app->status_version = status_label("Version: " APP_VERSION, FALSE);
    app->status_game = status_label("", FALSE);
    app->status_platform = status_label("", FALSE);

    gtk_box_pack_start(GTK_BOX(app->statusbar), app->status_items, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(app->statusbar), status_sep(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app->statusbar), app->status_selected, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(app->statusbar), app->status_platform, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(app->statusbar), status_sep(), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(app->statusbar), app->status_game, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(app->statusbar), status_sep(), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(app->statusbar), app->status_archive, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(app->statusbar), status_sep(), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(app->statusbar), app->status_version, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(vbox), app->statusbar, FALSE, FALSE, 0);

    /* --- menu bar: built last because View -> Layout guards the panes */
    app->menubar = build_menubar(app);
    gtk_box_pack_start(GTK_BOX(vbox), app->menubar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(vbox), app->menubar, 1);
    g_object_set_data(G_OBJECT(g_mi_menubar), "pane", app->menubar);

    /* --- signals ------------------------------------------------- */
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app->window, "map-event", G_CALLBACK(on_window_map), app);
    GtkTreeSelection *tsel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree));
    g_signal_connect(tsel, "changed", G_CALLBACK(on_tree_selection), app);
    GtkTreeSelection *lsel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->list));
    g_signal_connect(lsel, "changed", G_CALLBACK(on_list_selection), app);
    g_signal_connect(app->list, "row-activated", G_CALLBACK(on_list_activated), app);
    g_signal_connect(app->list, "button-press-event", G_CALLBACK(on_list_button_press), app);

    /* drag and drop */
    static const GtkTargetEntry targets[] = { { "text/uri-list", 0, 0 } };
    gtk_drag_dest_set(app->window, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
    g_signal_connect(app->window, "drag-data-received", G_CALLBACK(on_drag_data), app);

    rebuild_dynamic_menus(app);
    apply_view_mode(app);
    refresh_list(app);
}

/* ------------------------------------------------------------------ */
/* CLI test mode                                                       */
/* ------------------------------------------------------------------ */

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
