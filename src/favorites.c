/* favorites.c - the saved folders and recent archive lists (small files under
 * the user configuration directory) and their menus. */

#include "app.h"

typedef struct { App *app; GtkWidget *listbox; } FavOrg;

/* this file */
static char *config_file(const char *name);
static GPtrArray *list_load(const char *name);
static void list_save(const char *name, GPtrArray *arr);
static void list_remove_value(const char *name, const char *value);
static void on_open_recent(GtkWidget *w, gpointer data);
static void fill_list_menu(App *app, GtkWidget *menu, GtkWidget *marker,
                           const char *file, const char *empty_text);
static void fav_fill(FavOrg *fo);
static void on_fav_remove(GtkWidget *w, gpointer data);

/* small configuration files: favorites and recent archives            */
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

void list_add(const char *name, const char *value, int max)
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

void rebuild_dynamic_menus(App *app)
{
    if (g_fav_menu)
        fill_list_menu(app, g_fav_menu, g_fav_marker, "favorites", "(no favorites)");
    if (g_recent_menu)
        fill_list_menu(app, g_recent_menu, NULL, "recent", "(no recent archives)");
}

void on_favorites_add(GtkWidget *w, gpointer data)
{
    App *app = data;
    (void)w;
    if (!has_archive(app)) { msg_dialog(app, GTK_MESSAGE_INFO, "No open archive."); return; }
    RpfArchive *a = g_ptr_array_index(app->archives, (guint)app->cur.archive);
    list_add("favorites", a->path, 20);
    rebuild_dynamic_menus(app);
    notify(app, GTK_MESSAGE_INFO, "Pinned to favorites: %s", a->path);
}

static void fav_fill(FavOrg *fo)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(fo->listbox));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    GPtrArray *fav = list_load("favorites");
    if (fav->len == 0) {
        GtkWidget *empty = gtk_label_new("No favorite folders yet.\n"
            "Use Favorites -> Add current folder to remember one.");
        gtk_widget_set_halign(empty, GTK_ALIGN_START);
        gtk_widget_set_margin_start(empty, 6);
        gtk_widget_set_sensitive(empty, FALSE);
        gtk_list_box_insert(GTK_LIST_BOX(fo->listbox), empty, -1);
        g_ptr_array_free(fav, TRUE);
        gtk_widget_show_all(fo->listbox);
        return;
    }
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

void on_favorites_organize(GtkWidget *w, gpointer data)
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
