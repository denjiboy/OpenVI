/* actions.c - the file and edit actions wired to the menus and tool bar. */

#include "app.h"

/* this file */
static void current_dir_rel(App *app, char *buf, size_t len);
static int selected_entry_or_msg(App *app);
static void close_archive(App *app, int idx);

/* UI construction                                                     */

/* The old text-button helpers were replaced by the menu bar, the tool bar and
 * the live preview pane built below. */
void refresh_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    refresh_list((App *)data);
}

void on_back(GtkWidget *w, gpointer data)
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

void on_forward(GtkWidget *w, gpointer data)
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

void on_up(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (app->cur.archive < 0) return;
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    int p = find_parent(a, app->cur.entry);
    if (p >= 0) nav_to(app, app->cur.archive, p, TRUE);
}

void on_filter_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    refresh_list((App *)data);
}

/* DFM: TAboutWindow3. */
void on_about(GtkWidget *w, gpointer data)
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

/* actions                                                             */
void on_quit(GtkWidget *w, gpointer data)
{
    (void)w; (void)data;
    gtk_main_quit();
}

void on_new_archive(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    msg_dialog(app, GTK_MESSAGE_INFO,
        "Create archive\n\n"
        "Building new .rpf archives is not implemented in this build; it reads "
        "and extracts existing archives.");
}

void on_editor_info(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    msg_dialog(app, GTK_MESSAGE_INFO,
        "External editors are not implemented.\n\n"
        "Use Edit -> View as text... or View in Hex... to look at the file, or "
        "Edit -> Extract... to write it to disk.");
}

void on_view_hex(GtkWidget *w, gpointer data)
{
    (void)w;
    open_viewer((App *)data, TRUE);
}

void on_view_text(GtkWidget *w, gpointer data)
{
    (void)w;
    open_viewer((App *)data, FALSE);
}

void on_defrag(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    msg_dialog(app, GTK_MESSAGE_INFO,
        "Defragmentation\n\n"
        "Rebuilding an archive so that its payloads are stored in order is not "
        "implemented in this build.");
}

void on_edit_mode(GtkWidget *w, gpointer data)
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

void on_select_all(GtkWidget *w, gpointer data)
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

void on_copy_name(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    int entry = selected_entry_or_msg(app);
    if (entry < 0) return;
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    copy_to_clipboard(app, a->entries[entry].name);
}

void on_copy_path(GtkWidget *w, gpointer data)
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
void on_copy_address(GtkWidget *w, gpointer data)
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

void on_filter_focus(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    gtk_widget_grab_focus(app->filter);
}

void on_filter_clear(GtkWidget *w, gpointer data)
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

void on_close_archive(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (has_archive(app)) close_archive(app, app->cur.archive);
}

void on_close_all(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    while (app->archives->len > 0) close_archive(app, (int)app->archives->len - 1);
    gtk_tree_store_clear(app->tree_store);
    app->cur.archive = -1;
    app->cur.entry = -1;
    refresh_list(app);
}
