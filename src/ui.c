/* ui.c - the main window: its widgets, styling, signal wiring and layout. */

#include "app.h"

/* this file */
static gboolean on_window_map(GtkWidget *w, GdkEvent *ev, gpointer data);

int g_win_w, g_win_h;        /* window size negotiated by build_ui */

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

void build_ui(App *app)
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
    app->col[0] = add_list_column(app, "Name", COL_NAME, 150, TRUE, 0.0f);
    app->col[1] = add_list_column(app, "Type", COL_TYPE, 80, FALSE, 0.0f);
    app->col[2] = add_list_column(app, "Size", COL_SIZE, 70, FALSE, 1.0f);
    app->col[3] = add_list_column(app, "Attributes", COL_ATTR, 90, FALSE, 0.0f);
    app->col[4] = add_list_column(app, "Compressed", COL_PACKED, 110, FALSE, 1.0f);
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
    g_signal_connect(app->tree, "button-press-event", G_CALLBACK(on_tree_button_press), app);

    /* drag and drop */
    static const GtkTargetEntry targets[] = { { "text/uri-list", 0, 0 } };
    gtk_drag_dest_set(app->window, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
    g_signal_connect(app->window, "drag-data-received", G_CALLBACK(on_drag_data), app);

    rebuild_dynamic_menus(app);
    apply_view_mode(app);
    refresh_list(app);
}
