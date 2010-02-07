/*
 Copyright (C) 2008-2009 Christian Dywan <christian@twotoasts.de>

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 See the file COPYING for the full license text.
*/

#include "midori-history.h"

#include "midori-app.h"
#include "midori-browser.h"
#include "midori-stock.h"
#include "midori-view.h"
#include "midori-viewable.h"

#include "sokoke.h"

#include <glib/gi18n.h>
#include <string.h>

#include <katze/katze.h>
#include <gdk/gdkkeysyms.h>

void
midori_browser_edit_bookmark_dialog_new (MidoriBrowser* browser,
                                         KatzeItem*     bookmark,
                                         gboolean       new_bookmark,
                                         gboolean       is_folder);

#include "config.h"

#if HAVE_SQLITE
    #include <sqlite3.h>
#endif

struct _MidoriHistory
{
    GtkVBox parent_instance;

    GtkWidget* toolbar;
    GtkWidget* bookmark;
    GtkWidget* delete;
    GtkWidget* clear;
    GtkWidget* treeview;
    MidoriApp* app;
    KatzeArray* array;
};

struct _MidoriHistoryClass
{
    GtkVBoxClass parent_class;
};

static void
midori_history_viewable_iface_init (MidoriViewableIface* iface);

G_DEFINE_TYPE_WITH_CODE (MidoriHistory, midori_history, GTK_TYPE_VBOX,
                         G_IMPLEMENT_INTERFACE (MIDORI_TYPE_VIEWABLE,
                             midori_history_viewable_iface_init));

enum
{
    PROP_0,

    PROP_APP
};

static void
midori_history_finalize     (GObject*      object);

static void
midori_history_set_property (GObject*      object,
                             guint         prop_id,
                             const GValue* value,
                             GParamSpec*   pspec);

static void
midori_history_get_property (GObject*    object,
                             guint       prop_id,
                             GValue*     value,
                             GParamSpec* pspec);

static void
midori_history_class_init (MidoriHistoryClass* class)
{
    GObjectClass* gobject_class;
    GParamFlags flags;

    gobject_class = G_OBJECT_CLASS (class);
    gobject_class->finalize = midori_history_finalize;
    gobject_class->set_property = midori_history_set_property;
    gobject_class->get_property = midori_history_get_property;

    flags = G_PARAM_READWRITE | G_PARAM_CONSTRUCT;

    g_object_class_install_property (gobject_class,
                                     PROP_APP,
                                     g_param_spec_object (
                                     "app",
                                     "App",
                                     "The app",
                                     MIDORI_TYPE_APP,
                                     flags));
}

static const gchar*
midori_history_get_label (MidoriViewable* viewable)
{
    return _("History");
}

static const gchar*
midori_history_get_stock_id (MidoriViewable* viewable)
{
    return STOCK_HISTORY;
}

#if HAVE_SQLITE
static void
midori_history_clear_db (MidoriHistory* history)
{
    gchar* sqlcmd;
    sqlite3* db;
    char* errmsg = NULL;

    db = g_object_get_data (G_OBJECT (history->array), "db");
    sqlcmd = sqlite3_mprintf ("DELETE FROM history");

    if (sqlite3_exec (db, sqlcmd, NULL, NULL, &errmsg) != SQLITE_OK)
    {
        g_printerr (_("Failed to remove history item: %s\n"), errmsg);
        sqlite3_free (errmsg);
    }

    sqlite3_free (sqlcmd);
}

static void
midori_history_remove_item_from_db (MidoriHistory* history,
                                    KatzeItem*     item)
{
    gchar* sqlcmd;
    sqlite3* db;
    char* errmsg = NULL;

    db = g_object_get_data (G_OBJECT (history->array), "db");

    if (katze_item_get_uri (item))
        sqlcmd = sqlite3_mprintf (
            "DELETE FROM history WHERE uri = '%q' AND"
            " title = '%q' AND date = %llu",
            katze_item_get_uri (item),
            katze_item_get_name (item),
            katze_item_get_added (item));
    else
       sqlcmd = sqlite3_mprintf (
            "DELETE FROM history WHERE day = %d", katze_item_get_added (item));

    if (sqlite3_exec (db, sqlcmd, NULL, NULL, &errmsg) != SQLITE_OK)
    {
        g_printerr (_("Failed to remove history item: %s\n"), errmsg);
        sqlite3_free (errmsg);
    }

    sqlite3_free (sqlcmd);
}

static gboolean
midori_history_read_from_db (MidoriHistory* history,
                             GtkTreeStore*  model,
                             GtkTreeIter*   parent,
                             int            req_day)
{
    sqlite3* db;
    sqlite3_stmt* statement;
    gint result;
    gchar* sqlcmd;
    time_t current_time;

    GtkTreeIter iter;
    GtkTreeIter root_iter;

    db = g_object_get_data (G_OBJECT (history->array), "db");

    if (req_day == 0)
        sqlcmd = g_strdup ("SELECT day, date "
                           "FROM history GROUP BY day ORDER BY day ASC");
    else
        sqlcmd = g_strdup_printf ("SELECT uri, title, date, day "
                                  " FROM history WHERE day = '%d' "
                                  " GROUP BY uri ORDER BY date ASC", req_day);

    if (sqlite3_prepare_v2 (db, sqlcmd, -1, &statement, NULL) != SQLITE_OK)
    {
        g_free (sqlcmd);
        return FALSE;
    }
    g_free (sqlcmd);

    if (req_day == 0)
        current_time = time (NULL);

    while ((result = sqlite3_step (statement)) == SQLITE_ROW)
    {
        KatzeItem* item;
        const unsigned char* uri;
        const unsigned char* title;
        sqlite3_int64 date;
        sqlite3_int64 day;

        if (req_day == 0)
        {
            gint age;
            gchar token[50];
            gchar* sdate;

            day = sqlite3_column_int64 (statement, 0);
            date = sqlite3_column_int64 (statement, 1);

            item = katze_item_new ();
            katze_item_set_added (item, day);
            age = sokoke_days_between ((time_t*)&date, &current_time);

            /* A negative age is a date in the future, the clock is probably off */
            if (age < -1)
            {
                static gboolean clock_warning = FALSE;
                if (!clock_warning)
                {
                    midori_app_send_notification (history->app,
                        _("Erroneous clock time"),
                        _("The clock time lies in the past. "
                          "Please check the current date and time."));
                    clock_warning = TRUE;
                }
            }

            if (age > 7 || age < 0)
            {
                strftime (token, sizeof (token), "%x", localtime ((time_t*)&date));
                sdate = token;
            }
            else if (age > 6)
                sdate = _("A week ago");
            else if (age > 1)
                sdate = g_strdup_printf (ngettext ("%d day ago",
                    "%d days ago", (gint)age), (gint)age);
            else if (age == 0)
                sdate = _("Today");
            else
                sdate = _("Yesterday");

            gtk_tree_store_insert_with_values (model, &root_iter, NULL,
                                               0, 0, item, 1, sdate, -1);
            /* That's an invisible dummy, so we always have an expander */
            gtk_tree_store_insert_with_values (model, &iter, &root_iter,
                0, 0, NULL, 1, NULL, -1);

            if (age > 1 && age < 7)
                g_free (sdate);
        }
        else
        {
            uri = sqlite3_column_text (statement, 0);
            title = sqlite3_column_text (statement, 1);
            date = sqlite3_column_int64 (statement, 2);
            day = sqlite3_column_int64 (statement, 3);
            if (!uri)
                continue;

            item = katze_item_new ();
            katze_item_set_added (item, date);
            katze_item_set_uri (item, (gchar*)uri);
            katze_item_set_name (item, (gchar*)title);
            gtk_tree_store_insert_with_values (model, NULL, parent,
                0, 0, item, 1, katze_item_get_name (item), -1);
        }
    }

    if (req_day != 0)
    {
        /* Remove invisible dummy row */
        GtkTreeIter child;
        gint last = gtk_tree_model_iter_n_children (GTK_TREE_MODEL (model), parent);
        gtk_tree_model_iter_nth_child (GTK_TREE_MODEL (model), &child, parent, last - 1);
        gtk_tree_store_remove (model, &child);
    }

    if (result != SQLITE_DONE)
        g_print (_("Failed to execute database statement: %s\n"),
                 sqlite3_errmsg (db));

    sqlite3_finalize (statement);
    return FALSE;
}

static void
midori_history_add_clicked_cb (GtkWidget* toolitem)
{
    MidoriBrowser* browser = midori_browser_get_for_widget (toolitem);
    /* FIXME: Take selected folder into account */
    midori_browser_edit_bookmark_dialog_new (browser, NULL, TRUE, FALSE);
}

static void
midori_history_delete_clicked_cb (GtkWidget*     toolitem,
                                  MidoriHistory* history)
{
    GtkTreeModel* model;
    GtkTreeIter iter;

    if (katze_tree_view_get_selected_iter (GTK_TREE_VIEW (history->treeview),
                                           &model, &iter))
    {
        KatzeItem* item;

        gtk_tree_model_get (model, &iter, 0, &item, -1);
        midori_history_remove_item_from_db (history, item);
        gtk_tree_store_remove (GTK_TREE_STORE (model), &iter);
        g_object_unref (item);
    }
}

static void
midori_history_clear_clicked_cb (GtkWidget*     toolitem,
                                 MidoriHistory* history)
{
    MidoriBrowser* browser;
    GtkWidget* dialog;
    gint result;

    browser = midori_browser_get_for_widget (GTK_WIDGET (history));
    dialog = gtk_message_dialog_new (GTK_WINDOW (browser),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        _("Are you sure you want to remove all history items?"));
    result = gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);
    if (result != GTK_RESPONSE_YES)
        return;

    midori_history_clear_db (history);
}

static void
midori_history_cursor_or_row_changed_cb (GtkTreeView*   treeview,
                                         MidoriHistory* history)
{
    GtkTreeModel* model;
    GtkTreeIter iter;
    KatzeItem* item;

    if (!history->bookmark)
        return;

    if (katze_tree_view_get_selected_iter (treeview, &model, &iter))
    {
        gboolean is_page;

        gtk_tree_model_get (model, &iter, 0, &item, -1);

        is_page = item && katze_item_get_uri (item);
        gtk_widget_set_sensitive (history->bookmark, is_page);
        gtk_widget_set_sensitive (history->delete, TRUE);

        if (item)
            g_object_unref (item);
    }
    else
    {
        gtk_widget_set_sensitive (history->bookmark, FALSE);
        gtk_widget_set_sensitive (history->delete, FALSE);
    }
}
#endif

static GtkWidget*
midori_history_get_toolbar (MidoriViewable* viewable)
{
    MidoriHistory* history = MIDORI_HISTORY (viewable);

    if (!history->toolbar)
    {
        GtkWidget* toolbar;
        #if HAVE_SQLITE
        GtkToolItem* toolitem;
        #endif

        toolbar = gtk_toolbar_new ();
        gtk_toolbar_set_style (GTK_TOOLBAR (toolbar), GTK_TOOLBAR_BOTH_HORIZ);
        gtk_toolbar_set_icon_size (GTK_TOOLBAR (toolbar), GTK_ICON_SIZE_BUTTON);
        history->toolbar = toolbar;
        #if HAVE_SQLITE
        toolitem = gtk_tool_button_new_from_stock (STOCK_BOOKMARK_ADD);
        gtk_widget_set_tooltip_text (GTK_WIDGET (toolitem),
                                     _("Bookmark the selected history item"));
        gtk_tool_item_set_is_important (toolitem, TRUE);
        g_signal_connect (toolitem, "clicked",
            G_CALLBACK (midori_history_add_clicked_cb), history);
        gtk_toolbar_insert (GTK_TOOLBAR (toolbar), toolitem, -1);
        gtk_widget_show (GTK_WIDGET (toolitem));
        history->bookmark = GTK_WIDGET (toolitem);
        toolitem = gtk_tool_button_new_from_stock (GTK_STOCK_DELETE);
        gtk_widget_set_tooltip_text (GTK_WIDGET (toolitem),
                                     _("Delete the selected history item"));
        g_signal_connect (toolitem, "clicked",
            G_CALLBACK (midori_history_delete_clicked_cb), history);
        gtk_toolbar_insert (GTK_TOOLBAR (toolbar), toolitem, -1);
        gtk_widget_show (GTK_WIDGET (toolitem));
        history->delete = GTK_WIDGET (toolitem);
        toolitem = gtk_tool_button_new_from_stock (GTK_STOCK_CLEAR);
        gtk_widget_set_tooltip_text (GTK_WIDGET (toolitem),
                                     _("Clear the entire history"));
        g_signal_connect (toolitem, "clicked",
            G_CALLBACK (midori_history_clear_clicked_cb), history);
        gtk_toolbar_insert (GTK_TOOLBAR (toolbar), toolitem, -1);
        gtk_widget_show (GTK_WIDGET (toolitem));
        history->clear = GTK_WIDGET (toolitem);
        midori_history_cursor_or_row_changed_cb (
            GTK_TREE_VIEW (history->treeview), history);
        g_signal_connect (history->bookmark, "destroy",
            G_CALLBACK (gtk_widget_destroyed), &history->bookmark);
        g_signal_connect (history->delete, "destroy",
            G_CALLBACK (gtk_widget_destroyed), &history->delete);
        g_signal_connect (history->clear, "destroy",
            G_CALLBACK (gtk_widget_destroyed), &history->clear);
        #endif
    }

    return history->toolbar;
}

static void
midori_history_viewable_iface_init (MidoriViewableIface* iface)
{
    iface->get_stock_id = midori_history_get_stock_id;
    iface->get_label = midori_history_get_label;
    iface->get_toolbar = midori_history_get_toolbar;
}

static void
midori_history_set_app (MidoriHistory* history,
                        MidoriApp*     app)
{
    GtkTreeModel* model;

    if (history->array)
    {
        g_object_unref (history->array);
        model = gtk_tree_view_get_model (GTK_TREE_VIEW (history->treeview));
        gtk_tree_store_clear (GTK_TREE_STORE (model));
    }

    katze_assign (history->app, app);
    if (!app)
        return;
    g_object_ref (app);

    history->array = katze_object_get_object (app, "history");
    model = gtk_tree_view_get_model (GTK_TREE_VIEW (history->treeview));
    #if HAVE_SQLITE
    midori_history_read_from_db (history, GTK_TREE_STORE (model), NULL, 0);
    #endif
}

static void
midori_history_set_property (GObject*      object,
                             guint         prop_id,
                             const GValue* value,
                             GParamSpec*   pspec)
{
    MidoriHistory* history = MIDORI_HISTORY (object);

    switch (prop_id)
    {
    case PROP_APP:
        midori_history_set_app (history, g_value_get_object (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
midori_history_get_property (GObject*    object,
                             guint       prop_id,
                             GValue*     value,
                             GParamSpec* pspec)
{
    MidoriHistory* history = MIDORI_HISTORY (object);

    switch (prop_id)
    {
    case PROP_APP:
        g_value_set_object (value, history->app);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
midori_history_treeview_render_icon_cb (GtkTreeViewColumn* column,
                                        GtkCellRenderer*   renderer,
                                        GtkTreeModel*      model,
                                        GtkTreeIter*       iter,
                                        GtkWidget*         treeview)
{
    KatzeItem* item;
    GdkPixbuf* pixbuf;

    gtk_tree_model_get (model, iter, 0, &item, -1);

    if (!item)
        pixbuf = NULL;
    else if (katze_item_get_uri (item))
        pixbuf = katze_load_cached_icon (katze_item_get_uri (item), treeview);
    else
        pixbuf = gtk_widget_render_icon (treeview, GTK_STOCK_DIRECTORY,
                                         GTK_ICON_SIZE_MENU, NULL);

    g_object_set (renderer, "pixbuf", pixbuf, NULL);

    if (pixbuf)
    {
        g_object_unref (pixbuf);
        g_object_unref (item);
    }
}

#if HAVE_SQLITE
static void
midori_history_row_activated_cb (GtkTreeView*       treeview,
                                   GtkTreePath*       path,
                                   GtkTreeViewColumn* column,
                                   MidoriHistory*   history)
{
    GtkTreeModel* model;
    GtkTreeIter iter;
    KatzeItem* item;
    const gchar* uri;

    model = gtk_tree_view_get_model (treeview);

    if (gtk_tree_model_get_iter (model, &iter, path))
    {
        gtk_tree_model_get (model, &iter, 0, &item, -1);

        if (!item)
            return;

        uri = katze_item_get_uri (item);
        if (uri && *uri)
        {
            MidoriBrowser* browser;

            browser = midori_browser_get_for_widget (GTK_WIDGET (history));
            midori_browser_set_current_uri (browser, uri);
        }

        g_object_unref (item);
    }
}

static void
midori_history_popup_item (GtkWidget*     menu,
                           const gchar*   stock_id,
                           const gchar*   label,
                           KatzeItem*     item,
                           gpointer       callback,
                           MidoriHistory* history)
{
    const gchar* uri;
    GtkWidget* menuitem;

    uri = katze_item_get_uri (item);

    menuitem = gtk_image_menu_item_new_from_stock (stock_id, NULL);
    if (label)
        gtk_label_set_text_with_mnemonic (GTK_LABEL (gtk_bin_get_child (
        GTK_BIN (menuitem))), label);
    if (!strcmp (stock_id, GTK_STOCK_EDIT))
        gtk_widget_set_sensitive (menuitem,
            KATZE_IS_ARRAY (item) || uri != NULL);
    else if (!KATZE_IS_ARRAY (item) && strcmp (stock_id, GTK_STOCK_DELETE))
        gtk_widget_set_sensitive (menuitem, uri != NULL);
    g_object_set_data (G_OBJECT (menuitem), "KatzeItem", item);
    g_signal_connect (menuitem, "activate", G_CALLBACK (callback), history);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), menuitem);
    gtk_widget_show (menuitem);
}

static void
midori_history_open_activate_cb (GtkWidget*     menuitem,
                                 MidoriHistory* history)
{
    KatzeItem* item;
    const gchar* uri;

    item = (KatzeItem*)g_object_get_data (G_OBJECT (menuitem), "KatzeItem");
    uri = katze_item_get_uri (item);

    if (uri && *uri)
    {
        MidoriBrowser* browser = midori_browser_get_for_widget (GTK_WIDGET (history));
        midori_browser_set_current_uri (browser, uri);
    }
}

static void
midori_history_open_in_tab_activate_cb (GtkWidget*     menuitem,
                                        MidoriHistory* history)
{
    KatzeItem* item;
    const gchar* uri;
    guint n;

    item = (KatzeItem*)g_object_get_data (G_OBJECT (menuitem), "KatzeItem");
    if (KATZE_IS_ARRAY (item))
    {
        KatzeItem* child;
        guint i = 0;

        while ((child = katze_array_get_nth_item (KATZE_ARRAY (item), i)))
        {
            if ((uri = katze_item_get_uri (child)) && *uri)
            {
                MidoriBrowser* browser;
                MidoriWebSettings* settings;

                browser = midori_browser_get_for_widget (GTK_WIDGET (history));
                n = midori_browser_add_item (browser, child);
                settings = katze_object_get_object (browser, "settings");
                if (!katze_object_get_boolean (settings, "open-tabs-in-the-background"))
                    midori_browser_set_current_page (browser, n);
                g_object_unref (settings);
            }
            i++;
        }
    }
    else
    {
        if ((uri = katze_item_get_uri (item)) && *uri)
        {
            MidoriBrowser* browser;
            MidoriWebSettings* settings;

            browser = midori_browser_get_for_widget (GTK_WIDGET (history));
            n = midori_browser_add_item (browser, item);
            settings = katze_object_get_object (browser, "settings");
            if (!katze_object_get_boolean (settings, "open-tabs-in-the-background"))
                midori_browser_set_current_page (browser, n);
            g_object_unref (settings);
        }
    }
}

static void
midori_history_open_in_window_activate_cb (GtkWidget*     menuitem,
                                           MidoriHistory* history)
{
    KatzeItem* item;
    const gchar* uri;

    item = (KatzeItem*)g_object_get_data (G_OBJECT (menuitem), "KatzeItem");
    uri = katze_item_get_uri (item);

    if (uri && *uri)
    {
        MidoriBrowser* new_browser = midori_app_create_browser (history->app);
        midori_app_add_browser (history->app, new_browser);
        gtk_widget_show (GTK_WIDGET (new_browser));
        midori_browser_add_uri (new_browser, uri);
    }
}

static void
midori_history_bookmark_activate_cb (GtkWidget*     menuitem,
                                     MidoriHistory* history)
{
    KatzeItem* item;
    const gchar* uri;

    item = (KatzeItem*)g_object_get_data (G_OBJECT (menuitem), "KatzeItem");
    uri = katze_item_get_uri (item);

    if (uri && *uri)
    {
        MidoriBrowser* browser = midori_browser_get_for_widget (GTK_WIDGET (history));
        midori_browser_edit_bookmark_dialog_new (browser, item, TRUE, FALSE);
    }
}

static void
midori_history_popup (GtkWidget*      widget,
                      GdkEventButton* event,
                      KatzeItem*      item,
                      MidoriHistory*  history)
{
    GtkWidget* menu;
    GtkWidget* menuitem;

    menu = gtk_menu_new ();
    if (KATZE_IS_ARRAY (item))
        midori_history_popup_item (menu,
            STOCK_TAB_NEW, _("Open all in _Tabs"),
            item, midori_history_open_in_tab_activate_cb, history);
    else
    {
        midori_history_popup_item (menu, GTK_STOCK_OPEN, NULL,
            item, midori_history_open_activate_cb, history);
        midori_history_popup_item (menu, STOCK_TAB_NEW, _("Open in New _Tab"),
            item, midori_history_open_in_tab_activate_cb, history);
        midori_history_popup_item (menu, STOCK_WINDOW_NEW, _("Open in New _Window"),
            item, midori_history_open_in_window_activate_cb, history);
        midori_history_popup_item (menu, STOCK_BOOKMARK_ADD, NULL,
            item, midori_history_bookmark_activate_cb, history);
    }
    menuitem = gtk_separator_menu_item_new ();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), menuitem);
    gtk_widget_show (menuitem);
    midori_history_popup_item (menu, GTK_STOCK_DELETE, NULL,
        item, midori_history_delete_clicked_cb, history);

    katze_widget_popup (widget, GTK_MENU (menu), event, KATZE_MENU_POSITION_CURSOR);
}

static gboolean
midori_history_button_release_event_cb (GtkWidget*      widget,
                                        GdkEventButton* event,
                                        MidoriHistory*  history)
{
    GtkTreeModel* model;
    GtkTreeIter iter;

    if (event->button != 2 && event->button != 3)
        return FALSE;

    if (katze_tree_view_get_selected_iter (GTK_TREE_VIEW (widget), &model, &iter))
    {
        KatzeItem* item;

        gtk_tree_model_get (model, &iter, 0, &item, -1);

        if (!item)
            return FALSE;

        if (event->button == 2)
        {
            const gchar* uri = katze_item_get_uri (item);

            if (uri && *uri)
            {
                MidoriBrowser* browser;
                gint n;

                browser = midori_browser_get_for_widget (widget);
                n = midori_browser_add_uri (browser, uri);
                midori_browser_set_current_page (browser, n);
            }
        }
        else
            midori_history_popup (widget, event, item, history);

        g_object_unref (item);
        return TRUE;
    }
    return FALSE;
}

static gboolean
midori_history_key_release_event_cb (GtkWidget*     widget,
                                     GdkEventKey*   event,
                                     MidoriHistory* history)
{
    GtkTreeModel* model;
    GtkTreeIter iter;

    if (event->keyval != GDK_Delete)
        return FALSE;

    if (katze_tree_view_get_selected_iter (GTK_TREE_VIEW (widget), &model, &iter))
    {
        KatzeItem* item;

        gtk_tree_model_get (model, &iter, 0, &item, -1);
        midori_history_remove_item_from_db (history, item);
        gtk_tree_store_remove (GTK_TREE_STORE (model), &iter);
        g_object_unref (item);
    }

    return FALSE;
}

static void
midori_history_popup_menu_cb (GtkWidget*     widget,
                              MidoriHistory* history)
{
    GtkTreeModel* model;
    GtkTreeIter iter;
    KatzeItem* item;

    if (katze_tree_view_get_selected_iter (GTK_TREE_VIEW (widget), &model, &iter))
    {
        gtk_tree_model_get (model, &iter, 0, &item, -1);
        midori_history_popup (widget, NULL, item, history);
        g_object_unref (item);
    }
}

static void
midori_history_row_expanded_cb (GtkTreeView*   treeview,
                                GtkTreeIter*   iter,
                                GtkTreePath*   path,
                                MidoriHistory* history)
{
    GtkTreeModel* model;
    KatzeItem* item;

    model = gtk_tree_view_get_model (GTK_TREE_VIEW (treeview));
    gtk_tree_model_get (model, iter, 0, &item, -1);
    midori_history_read_from_db (history, GTK_TREE_STORE (model),
                                 iter, katze_item_get_added (item));
    g_object_unref (item);
}

static void
midori_history_row_collapsed_cb (GtkTreeView *treeview,
                                 GtkTreeIter *parent,
                                 GtkTreePath *path,
                                 gpointer     user_data)
{
    GtkTreeModel* model;
    GtkTreeStore* treestore;
    GtkTreeIter child;

    model = gtk_tree_view_get_model (GTK_TREE_VIEW (treeview));
    treestore = GTK_TREE_STORE (model);
    while (gtk_tree_model_iter_nth_child (model, &child, parent, 0))
        gtk_tree_store_remove (treestore, &child);
    /* That's an invisible dummy, so we always have an expander */
    gtk_tree_store_insert_with_values (treestore, &child, parent,
        0, 0, NULL, 1, NULL, -1);
}
#endif

static void
midori_history_init (MidoriHistory* history)
{
    GtkTreeStore* model;
    GtkWidget* treeview;
    GtkTreeViewColumn* column;
    GtkCellRenderer* renderer_pixbuf;
    GtkCellRenderer* renderer_text;

    /* Create the treeview */
    model = gtk_tree_store_new (2, KATZE_TYPE_ITEM, G_TYPE_STRING);
    treeview = gtk_tree_view_new_with_model (GTK_TREE_MODEL (model));
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (treeview), FALSE);
    column = gtk_tree_view_column_new ();
    renderer_pixbuf = gtk_cell_renderer_pixbuf_new ();
    gtk_tree_view_column_pack_start (column, renderer_pixbuf, FALSE);
    gtk_tree_view_column_set_cell_data_func (column, renderer_pixbuf,
        (GtkTreeCellDataFunc)midori_history_treeview_render_icon_cb,
        treeview, NULL);
    renderer_text = gtk_cell_renderer_text_new ();
    gtk_tree_view_column_pack_start (column, renderer_text, FALSE);
    gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (column), renderer_text,
        "text", 1, NULL);
    gtk_tree_view_append_column (GTK_TREE_VIEW (treeview), column);
    g_object_unref (model);
    #if HAVE_SQLITE
    g_object_connect (treeview,
                      "signal::row-activated",
                      midori_history_row_activated_cb, history,
                      "signal::cursor-changed",
                      midori_history_cursor_or_row_changed_cb, history,
                      "signal::columns-changed",
                      midori_history_cursor_or_row_changed_cb, history,
                      "signal::button-release-event",
                      midori_history_button_release_event_cb, history,
                      "signal::key-release-event",
                      midori_history_key_release_event_cb, history,
                      "signal::row-expanded",
                      midori_history_row_expanded_cb, history,
                      "signal::row-collapsed",
                      midori_history_row_collapsed_cb, history,
                      "signal::popup-menu",
                      midori_history_popup_menu_cb, history,
                      NULL);
    #endif
    gtk_widget_show (treeview);
    gtk_box_pack_start (GTK_BOX (history), treeview, TRUE, TRUE, 0);
    history->treeview = treeview;
    /* FIXME: We need to connect a signal here, to add new pages into history */
}

static void
midori_history_finalize (GObject* object)
{
    MidoriHistory* history = MIDORI_HISTORY (object);

    if (history->app)
        g_object_unref (history->app);

    /* FIXME: We don't unref items (last argument is FALSE) because
       our reference counting is incorrect. */
    g_object_unref (history->array);
}

/**
 * midori_history_new:
 *
 * Creates a new empty history.
 *
 * Return value: a new #MidoriHistory
 *
 * Since: 0.1.3
 **/
GtkWidget*
midori_history_new (void)
{
    MidoriHistory* history = g_object_new (MIDORI_TYPE_HISTORY, NULL);

    return GTK_WIDGET (history);
}
