/* util.c - small shared helpers: size and type formatting, entry icons and
 * attributes, the clipboard, message boxes, notifications and the
 * text/hex dump helpers. */

#include "app.h"

/* helpers                                                             */
void fmt_size(uint64_t n, char *buf, size_t len)
{
    if (n < 1024) snprintf(buf, len, "%llu B", (unsigned long long)n);
    else if (n < 1024ull * 1024) snprintf(buf, len, "%.1f KB", n / 1024.0);
    else if (n < 1024ull * 1024 * 1024) snprintf(buf, len, "%.2f MB", n / (1024.0 * 1024));
    else snprintf(buf, len, "%.2f GB", n / (1024.0 * 1024 * 1024));
}

const char *entry_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

void entry_type(const RpfEntry *e, char *buf, size_t len)
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

uint64_t entry_display_size(const RpfEntry *e)
{
    if (e->isDir) return 0;
    if (e->isResource) return e->resourceSize;
    return e->size;
}

int find_parent(RpfArchive *a, int idx)
{
    for (uint32_t i = 0; i < a->entryCount; i++) {
        const RpfEntry *p = &a->entries[i];
        if (!p->isDir) continue;
        if ((uint32_t)idx >= p->size && (uint32_t)idx < p->size + p->flags) return (int)i;
    }
    return -1;
}

void archive_label(RpfArchive *a, char *buf, size_t len)
{
    const char *base = strrchr(a->path, '/');
    snprintf(buf, len, "%s", base ? base + 1 : a->path);
}

/* small helpers                                                       */

/* Icon used for an entry in the tree and in the list. */
const char *entry_icon_name(const RpfEntry *e)
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
void entry_attrs(const RpfEntry *e, char *buf, size_t len)
{
    buf[0] = 0;
    if (e->isDir) { snprintf(buf, len, "Directory"); return; }
    if (e->encrypted) g_strlcat(buf, "Encrypted", len);
}

/* Jenkins one-at-a-time hash, the hash RAGE uses for names. */
uint32_t rage_joaat(const char *s)
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

void copy_to_clipboard(App *app, const char *text)
{
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, text ? text : "", -1);
    notify(app, GTK_MESSAGE_INFO, "Copied to clipboard: %s", text ? text : "");
}

/* Modal message box, GLib style formatting.  Built as a plain GtkDialog so it
 * behaves exactly like the other dialogs. */
void msg_dialog(App *app, GtkMessageType type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    GtkWidget *d = gtk_dialog_new_with_buttons("OpenIV", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, "OK", GTK_RESPONSE_OK, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(h), 14);
    gtk_box_pack_start(GTK_BOX(box), h, FALSE, FALSE, 0);

    const char *icon = "dialog-information";
    if (type == GTK_MESSAGE_ERROR) icon = "dialog-error";
    else if (type == GTK_MESSAGE_WARNING) icon = "dialog-warning";
    else if (type == GTK_MESSAGE_QUESTION) icon = "dialog-question";
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(h), img, FALSE, FALSE, 0);

    GtkWidget *lbl = gtk_label_new(s);
    gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 64);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_set_valign(lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(h), lbl, FALSE, FALSE, 0);

    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(s);
}

/* Message in the notification pane (DFM: paneNotifications). */
void notify(App *app, GtkMessageType type, const char *fmt, ...)
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

gboolean has_archive(App *app)
{
    return app->cur.archive >= 0 && app->cur.archive < (int)app->archives->len;
}

/* First selected entry of the list, or -1.  The list allows a multiple
 * selection (DFM: MultiSelect), so this goes through the selected rows. */
int selected_entry(App *app, int *isdir)
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

/* text / hex preview helpers                                          */
int looks_like_text(const uint8_t *d, size_t n)
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

char *hexdump_lim(const uint8_t *d, size_t n, size_t lim)
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

char *hexdump(const uint8_t *d, size_t n)
{
    return hexdump_lim(d, n, 4096);
}

void set_text_view(GtkWidget *tv, const char *s)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(b, s ? s : "", -1);
}
