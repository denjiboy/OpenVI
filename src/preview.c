/* preview.c - the live preview pane, the text/hex viewer windows and the entry
 * and archive property dialogs. */

#include "app.h"

/* this file */
static void preview_message(App *app, const char *text);
static void preview_text(App *app, const char *text);
static void entry_info_text(RpfArchive *a, int entryIdx, char *buf, size_t len);

/* live preview pane (DFM: panePreView)                                */
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

void update_preview(App *app, int archiveIdx, int entryIdx)
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
void open_viewer(App *app, gboolean hex)
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

GtkWidget *info_grid_field(GtkWidget *grid, int row, const char *label,
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
void on_properties(GtkWidget *w, gpointer data)
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
void on_archive_properties(GtkWidget *w, gpointer data)
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
