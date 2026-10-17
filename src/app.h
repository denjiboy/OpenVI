/* app.h - the App state and the interfaces between the source files.
 *
 * main.c keeps only the entry point and the command line modes; the rest of
 * the window lives in ui.c (construction), browser.c (archive tree and file
 * list), preview.c (preview pane and viewers), archive.c (opening archives,
 * extraction, game folders), actions.c (file and edit actions), favorites.c
 * (saved folders and recent archives), tools.c (hash, options, help),
 * menus.c (menus, tool bar, context menu) and util.c (small helpers).
 */
#ifndef OPENVI_APP_H
#define OPENVI_APP_H

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

/* ------------------------------------------------------------------ */
/* shared state                                                        */
/* ------------------------------------------------------------------ */

extern GtkWidget *g_mi_editmode;    /* File -> Edit mode */
extern GtkWidget *g_mi_menubar;     /* View -> Layout -> Menu bar */
extern GtkWidget *g_fav_menu;
extern GtkWidget *g_fav_marker;
extern GtkWidget *g_recent_menu;
extern int g_win_w, g_win_h;        /* window size negotiated by build_ui */

/* ------------------------------------------------------------------ */
/* shared functions                                                    */
/* ------------------------------------------------------------------ */

/* util.c */
void fmt_size(uint64_t n, char *buf, size_t len);
const char *entry_ext(const char *name);
void entry_type(const RpfEntry *e, char *buf, size_t len);
uint64_t entry_display_size(const RpfEntry *e);
int find_parent(RpfArchive *a, int idx);
void archive_label(RpfArchive *a, char *buf, size_t len);
const char *entry_icon_name(const RpfEntry *e);
void entry_attrs(const RpfEntry *e, char *buf, size_t len);
uint32_t rage_joaat(const char *s);
void copy_to_clipboard(App *app, const char *text);
void msg_dialog(App *app, GtkMessageType type, const char *fmt, ...);
void notify(App *app, GtkMessageType type, const char *fmt, ...);
gboolean has_archive(App *app);
int selected_entry(App *app, int *isdir);
int looks_like_text(const uint8_t *d, size_t n);
char *hexdump_lim(const uint8_t *d, size_t n, size_t lim);
char *hexdump(const uint8_t *d, size_t n);
void set_text_view(GtkWidget *tv, const char *s);

/* browser.c */
void add_archive(App *app, RpfArchive *a, gboolean select);
void refresh_list(App *app);
gint list_sort_cmp(GtkTreeModel *m, GtkTreeIter *x, GtkTreeIter *y, gpointer data);
void set_sort(App *app, int col);
void nav_to(App *app, int archive, int entry, gboolean push);
void on_list_selection(GtkTreeSelection *sel, gpointer data);
void on_list_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data);
void on_tree_selection(GtkTreeSelection *sel, gpointer data);
GtkTreeViewColumn *add_list_column(App *app, const char *title, int col, int width,
                                          gboolean icon, gfloat xalign);
void apply_view_mode(App *app);
GtkWidget *status_label(const char *text, gboolean expand);
GtkWidget *status_sep(void);

/* preview.c */
void update_preview(App *app, int archiveIdx, int entryIdx);
void open_viewer(App *app, gboolean hex);
GtkWidget *info_grid_field(GtkWidget *grid, int row, const char *label,
                                  const char *value);
void on_properties(GtkWidget *w, gpointer data);
void on_archive_properties(GtkWidget *w, gpointer data);

/* archive.c */
void on_extract_clicked(GtkWidget *w, gpointer data);
void on_extract_all_clicked(GtkWidget *w, gpointer data);
void open_archive_path(App *app, const char *path, gboolean select);
void on_open_clicked(GtkWidget *w, gpointer data);
void scan_game_folder(App *app, const char *dir);
void on_open_folder_clicked(GtkWidget *w, gpointer data);
void on_games_clicked(GtkWidget *w, gpointer data);
void on_drag_data(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                         GtkSelectionData *sel, guint info, guint time, gpointer data);

/* actions.c */
void refresh_cb(GtkWidget *w, gpointer data);
void on_back(GtkWidget *w, gpointer data);
void on_forward(GtkWidget *w, gpointer data);
void on_up(GtkWidget *w, gpointer data);
void on_filter_changed(GtkWidget *w, gpointer data);
void on_about(GtkWidget *w, gpointer data);
void on_quit(GtkWidget *w, gpointer data);
void on_new_archive(GtkWidget *w, gpointer data);
void on_editor_info(GtkWidget *w, gpointer data);
void on_view_hex(GtkWidget *w, gpointer data);
void on_view_text(GtkWidget *w, gpointer data);
void on_defrag(GtkWidget *w, gpointer data);
void on_edit_mode(GtkWidget *w, gpointer data);
void on_select_all(GtkWidget *w, gpointer data);
void on_copy_name(GtkWidget *w, gpointer data);
void on_copy_path(GtkWidget *w, gpointer data);
void on_copy_address(GtkWidget *w, gpointer data);
void on_filter_focus(GtkWidget *w, gpointer data);
void on_filter_clear(GtkWidget *w, gpointer data);
void on_close_archive(GtkWidget *w, gpointer data);
void on_close_all(GtkWidget *w, gpointer data);

/* favorites.c */
void list_add(const char *name, const char *value, int max);
void rebuild_dynamic_menus(App *app);
void on_favorites_add(GtkWidget *w, gpointer data);
void on_favorites_organize(GtkWidget *w, gpointer data);

/* tools.c */
void on_hash_generator(GtkWidget *w, gpointer data);
void on_options(GtkWidget *w, gpointer data);
void on_help_verify(GtkWidget *w, gpointer data);
void on_web(GtkWidget *w, gpointer data);

/* menus.c */
void mi_add(GtkWidget *menu, GtkWidget *item);
GtkWidget *mi(const char *label, const char *accel, GCallback cb, App *app);
void menu_clear_after(GtkWidget *menu, GtkWidget *marker);
void on_infobar_response(GtkInfoBar *bar, gint resp, gpointer data);
gboolean on_list_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data);
gboolean on_tree_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data);
GtkWidget *build_menubar(App *app);
GtkWidget *build_toolbar(App *app);

/* ui.c */
void build_ui(App *app);

#endif /* OPENVI_APP_H */
