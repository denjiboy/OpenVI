/* tools.c - Tools menu dialogs: the hash generator, options, help verification
 * and the project links. */

#include "app.h"

typedef struct { GtkWidget *entry, *l1, *l2, *l3; } HashDlg;

/* this file */
static void hash_update(GtkEditable *e, gpointer data);

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

void on_hash_generator(GtkWidget *w, gpointer data)
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

void on_options(GtkWidget *w, gpointer data)
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

void on_help_verify(GtkWidget *w, gpointer data)
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

void on_web(GtkWidget *w, gpointer data)
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
