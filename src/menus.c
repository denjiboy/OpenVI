/* menus.c - the menu bar, the tool bar, the context menu and the shared widget
 * helpers used to build them. */

#include "app.h"

/* this file */
static GtkWidget *mi_sep(void);
static GtkWidget *mi_sub(const char *label, GtkWidget **out_menu);
static void add_accel(App *app, GtkWidget *item, const char *accel);
static GtkWidget *mi_check(const char *label, gboolean active, GCallback cb, gpointer data);
static GtkWidget *mi_radio(GtkWidget *group, const char *label, gboolean active,
                           GCallback cb, gpointer data);
static GtkWidget *mi_pane(const char *label, GtkWidget *pane, App *app);
static void on_toggle_pane(GtkCheckMenuItem *mi_item, gpointer data);
static void on_view_mode_menu(GtkCheckMenuItem *mi_item, gpointer data);
static void view_mode_items(App *app, GtkWidget *menu, GtkWidget **slots);
static void on_sort_menu(GtkCheckMenuItem *mi_item, gpointer data);
static void on_toolbar_preview(GtkToolButton *b, gpointer data);
static void on_tree_expand_all(GtkWidget *w, gpointer data);
static void on_tree_collapse_all(GtkWidget *w, gpointer data);
static GtkToolItem *tb_item(App *app, const char *icon, const char *tip, GCallback cb);

/* menu helpers                                                        */
static GtkWidget *g_mi_preview;     /* View -> Layout -> Preview Pane */
GtkWidget *g_mi_editmode;    /* File -> Edit mode */
GtkWidget *g_mi_menubar;     /* View -> Layout -> Menu bar */
GtkWidget *g_fav_menu;
GtkWidget *g_fav_marker;
GtkWidget *g_recent_menu;

void mi_add(GtkWidget *menu, GtkWidget *item)
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

GtkWidget *mi(const char *label, const char *accel, GCallback cb, App *app)
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

void menu_clear_after(GtkWidget *menu, GtkWidget *marker)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
    gboolean after = (marker == NULL);
    for (GList *l = kids; l; l = l->next) {
        if (after) gtk_widget_destroy(GTK_WIDGET(l->data));
        else if (l->data == marker) after = TRUE;
    }
    g_list_free(kids);
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

void on_infobar_response(GtkInfoBar *bar, gint resp, gpointer data)
{
    (void)resp; (void)data;
    gtk_widget_hide(GTK_WIDGET(bar));
}

/* Right click menu of the folder tree: unfold or fold the loaded archives. */
static void on_tree_expand_all(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    gtk_tree_view_expand_all(GTK_TREE_VIEW(app->tree));
}

static void on_tree_collapse_all(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    gtk_tree_view_collapse_all(GTK_TREE_VIEW(app->tree));
}

/* Right click menu of the folder tree (OpenIV's tree context menu), plus the
 * one thing GTK will not do for us: a single click on the row that is already
 * selected emits no "changed", yet it should still move the browser there. */
gboolean on_tree_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    App *app = data;
    (void)w;
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;

    GtkTreeView *tv = GTK_TREE_VIEW(app->tree);
    GtkTreeModel *m = GTK_TREE_MODEL(app->tree_store);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(tv, (gint)ev->x, (gint)ev->y,
                                       &path, NULL, NULL, NULL))
        return FALSE;

    GtkTreeIter it;
    gboolean have = gtk_tree_model_get_iter(m, &it, path);

    if (ev->button == 1) {
        GtkTreeIter cur;
        if (have && gtk_tree_selection_get_selected(sel, NULL, &cur)) {
            GtkTreePath *cp = gtk_tree_model_get_path(m, &cur);
            gboolean same = cp && gtk_tree_path_compare(cp, path) == 0;
            gtk_tree_path_free(cp);
            if (same) {
                int entry = 0, arch = 0;
                gtk_tree_model_get(m, &it, COL_ENTRY, &entry, COL_ISDIR, &arch, -1);
                nav_to(app, arch, entry, TRUE);
            }
        }
        gtk_tree_path_free(path);
        return FALSE;   /* let GTK run its own selection and expander handling */
    }

    if (ev->button != 3) {
        gtk_tree_path_free(path);
        return FALSE;
    }
    if (!has_archive(app)) {
        gtk_tree_path_free(path);
        return FALSE;
    }

    /* Select and unfold the archive under the pointer, like the list does. */
    gtk_tree_selection_unselect_all(sel);
    gtk_tree_selection_select_path(sel, path);
    gtk_tree_view_expand_row(tv, path, FALSE);
    gtk_tree_path_free(path);

    GtkWidget *menu = gtk_menu_new();
    mi_add(menu, mi("Expand all", NULL, G_CALLBACK(on_tree_expand_all), app));
    mi_add(menu, mi("Collapse all", NULL, G_CALLBACK(on_tree_collapse_all), app));
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/* Right click menu of the file list (OpenIV's FilesMenu). */
gboolean on_list_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
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

/* menu bar and tool bar (DFM: MainMenuBar, ToolBar, ViewToolBar)      */
GtkWidget *build_menubar(App *app)
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

GtkWidget *build_toolbar(App *app)
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
