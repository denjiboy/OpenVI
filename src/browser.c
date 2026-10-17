/* browser.c - the archive model shown in the window: the folder tree, the file
 * list, the address breadcrumb, the status bar, sorting, view modes
 * and navigation between folders. */

#include "app.h"

typedef struct { App *app; int archive, entry; } TreeFind;

/* this file */
static void add_folder_nodes(App *app, RpfArchive *a, GtkTreeIter *parent,
                             int archiveIdx, int dirIdx, int depth);
static gboolean filter_match(App *app, const char *name);
static int cur_dir(App *app);
static gboolean tree_find_cb(GtkTreeModel *m, GtkTreePath *path, GtkTreeIter *it,
                             gpointer data);
static void sync_tree_selection(App *app);
static void crumb_clicked(GtkWidget *w, gpointer data);
static GtkWidget *crumb_button(App *app, const char *text, const char *tip, int archive, int entry);
static void update_breadcrumb(App *app);
static void update_status(App *app);
static void on_column_clicked(GtkTreeViewColumn *c, gpointer data);

/* tree / list population                                              */
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

void add_archive(App *app, RpfArchive *a, gboolean select)
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

    /* OpenIV lists every loaded archive folded down to its root row; the user
     * expands the ones they want to browse.  So a new archive stays collapsed
     * and nothing here expands it. */

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

/* Current directory of the navigation.  cur.entry is a file when the user
 * asked for one, so normalise to the directory that holds it. */
static int cur_dir(App *app)
{
    if (!has_archive(app)) return -1;
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    int e = app->cur.entry;
    if (e < 0 || e >= (int)a->entryCount) return 0;
    if (!a->entries[e].isDir) e = find_parent(a, e);
    return e < 0 ? 0 : e;
}

/* A row can only be selected while its parents are open, and moving around in
 * the list must never unfold the user's archives, so a folded branch keeps the
 * selection on its nearest open ancestor instead. */
static void tree_select_visible(GtkTreeView *tv, GtkTreeModel *m, GtkTreeIter *it)
{
    GtkTreeIter cur = *it, parent;
    while (gtk_tree_model_iter_parent(m, &parent, &cur)) {
        GtkTreePath *pp = gtk_tree_model_get_path(m, &parent);
        gboolean open = gtk_tree_view_row_expanded(tv, pp);
        gtk_tree_path_free(pp);
        if (open) break;
        cur = parent;
    }
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(tv), &cur);
}

static gboolean tree_find_cb(GtkTreeModel *m, GtkTreePath *path, GtkTreeIter *it,
                             gpointer data)
{
    TreeFind *f = data;
    int arch = -1, entry = -1;
    (void)path;
    gtk_tree_model_get(m, it, COL_ISDIR, &arch, COL_ENTRY, &entry, -1);
    if (arch == f->archive && entry == f->entry) {
        tree_select_visible(GTK_TREE_VIEW(f->app->tree), m, it);
        return TRUE;
    }
    return FALSE;
}

/* Set while the browser moves the selection itself, so on_tree_selection does
 * not mistake it for the user picking a folder. */
static gboolean s_tree_syncing;

/* Keep the directory tree pointing at the folder shown in the list.  Without
 * this the view can be moved back to the archive root when the tree receives
 * focus, because GTK then selects its first row. */
static void sync_tree_selection(App *app)
{
    int dir = cur_dir(app);
    if (dir < 0) return;
    TreeFind f = { app, app->cur.archive, dir };
    s_tree_syncing = TRUE;
    gtk_tree_model_foreach(GTK_TREE_MODEL(app->tree_store), tree_find_cb, &f);
    s_tree_syncing = FALSE;
}

void refresh_list(App *app)
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
    sync_tree_selection(app);
}

/* breadcrumb, status bar and sorting                                  */
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

gint list_sort_cmp(GtkTreeModel *m, GtkTreeIter *x, GtkTreeIter *y, gpointer data)
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

void set_sort(App *app, int col)
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

/* navigation                                                          */
void nav_to(App *app, int archive, int entry, gboolean push)
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

void on_list_selection(GtkTreeSelection *sel, gpointer data)
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

void on_list_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
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

void on_tree_selection(GtkTreeSelection *sel, gpointer data)
{
    App *app = data;
    GtkTreeModel *m;
    GtkTreeIter it;
    if (s_tree_syncing) return;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return;
    int entry = 0, arch = 0;
    gtk_tree_model_get(m, &it, COL_ENTRY, &entry, COL_ISDIR, &arch, -1);
    /* A click on the folder already shown in the list refreshes nothing. */
    if (arch == app->cur.archive && entry == cur_dir(app)) return;
    nav_to(app, arch, entry, TRUE);
}

/* list columns, view modes, status bar                                */
static void on_column_clicked(GtkTreeViewColumn *c, gpointer data)
{
    App *app = data;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "col"));
    set_sort(app, col);
}

GtkTreeViewColumn *add_list_column(App *app, const char *title, int col, int width,
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

void apply_view_mode(App *app)
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

GtkWidget *status_label(const char *text, gboolean expand)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_single_line_mode(GTK_LABEL(l), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    if (expand) gtk_widget_set_hexpand(l, TRUE);
    return l;
}

GtkWidget *status_sep(void)
{
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_top(s, 2);
    gtk_widget_set_margin_bottom(s, 2);
    return s;
}
