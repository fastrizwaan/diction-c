#include "scan-popup.h"
#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <string.h>
#include <ctype.h>

static GtkApplication *app_ref = NULL;
static AppSettings *settings_ref = NULL;
static char* (*lookup_callback)(const char *word) = NULL;

static gboolean scan_enabled = FALSE;
static GdkClipboard *primary_clipboard = NULL;
static GdkClipboard *regular_clipboard = NULL;
static char *last_primary_text = NULL;
static char *last_clipboard_text = NULL;
static guint poll_source_id = 0;
static GtkWindow *popup_window = NULL;
static WebKitWebView *popup_webview = NULL;
static guint hide_timeout_id = 0;

typedef enum {
    SCAN_READ_AUTO_PRIMARY = 0,
    SCAN_READ_AUTO_CLIPBOARD = 1,
    SCAN_READ_MANUAL_PRIMARY = 2,
    SCAN_READ_MANUAL_CLIPBOARD = 3
} ScanReadMode;

static void on_clipboard_read(GObject *source_object, GAsyncResult *res, gpointer user_data);

/* Trim leading/trailing whitespace */
static char *trim_whitespace(char *str) {
    char *end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0)
        return str;
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static gboolean hide_popup_cb(gpointer user_data) {
    (void)user_data;
    if (popup_window) {
        gtk_window_close(popup_window);
        popup_window = NULL;
    }
    hide_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static void ensure_popup_window(void) {
    if (popup_window) return;

    popup_window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_decorated(popup_window, FALSE);
    gtk_window_set_deletable(popup_window, FALSE);
    gtk_window_set_focus_visible(popup_window, FALSE);
    gtk_window_set_default_size(popup_window, 400, 300);
    gtk_widget_add_css_class(GTK_WIDGET(popup_window), "osd");
    
    // Minimal WebKit setup
    popup_webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
    WebKitSettings *ws = webkit_web_view_get_settings(popup_webview);
    if (settings_ref) {
        if (settings_ref->font_family)
            webkit_settings_set_default_font_family(ws, settings_ref->font_family);
        if (settings_ref->font_size > 0)
            webkit_settings_set_default_font_size(ws, settings_ref->font_size);
    }
    
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(popup_webview));
    gtk_window_set_child(popup_window, scroll);
    
    // Dismiss when focus lost
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect_swapped(focus, "leave", G_CALLBACK(hide_popup_cb), NULL);
    gtk_widget_add_controller(GTK_WIDGET(popup_window), focus);
}

static gboolean scan_popup_try_show_for_word(const char *word) {
    if (!word || !*word || !lookup_callback) return FALSE;

    char *word_copy = g_strdup(word);
    char *clean_word = trim_whitespace(word_copy);
    if (!*clean_word) {
        g_free(word_copy);
        return FALSE;
    }

    char *html = lookup_callback(clean_word);
    g_free(word_copy);

    if (!html) return FALSE;

    ensure_popup_window();

    webkit_web_view_load_html(popup_webview, html, "file:///");
    g_free(html);

    // Position window near cursor via GdkDisplay
    GdkDisplay *display = gdk_display_get_default();
    GdkSeat *seat = gdk_display_get_default_seat(display);
    if (seat) {
        GdkDevice *pointer = gdk_seat_get_pointer(seat);
        if (pointer) {
            double x, y;
            gdk_device_get_surface_at_position(pointer, &x, &y);
            // On Wayland, absolute positioning is restricted, but this works on X11 
            // or when using popovers. For a top-level window, it might appear in center on Wayland.
            // A more complex Wayland-specific layer-shell or xdg-popup would be needed for true absolute.
            // For now, this is best effort without wayland-specific protocols.
            // We just present it.
        }
    }

    gtk_window_present(popup_window);

    if (hide_timeout_id != 0) g_source_remove(hide_timeout_id);
    hide_timeout_id = g_timeout_add(10000, hide_popup_cb, NULL); // Auto dismiss after 10s
    return TRUE;
}

void scan_popup_show_for_word(const char *word) {
    (void)scan_popup_try_show_for_word(word);
}

static gboolean scan_modifier_satisfied(void) {
    const char *modifier = settings_ref && settings_ref->scan_modifier_key
        ? settings_ref->scan_modifier_key
        : "none";

    if (!*modifier || g_strcmp0(modifier, "none") == 0) {
        return TRUE;
    }

    GdkDisplay *display = gdk_display_get_default();
    if (!display) {
        return FALSE;
    }

    GdkSeat *seat = gdk_display_get_default_seat(display);
    if (!seat) {
        return FALSE;
    }

    GdkModifierType state = 0;
    GdkDevice *keyboard = gdk_seat_get_keyboard(seat);
    if (keyboard) {
        state |= gdk_device_get_modifier_state(keyboard);
    }
    GdkDevice *pointer = gdk_seat_get_pointer(seat);
    if (pointer) {
        state |= gdk_device_get_modifier_state(pointer);
    }

    if (g_strcmp0(modifier, "ctrl") == 0) {
        return (state & GDK_CONTROL_MASK) != 0;
    }
    if (g_strcmp0(modifier, "alt") == 0) {
        return (state & GDK_ALT_MASK) != 0;
    }
    if (g_strcmp0(modifier, "meta") == 0) {
        return (state & (GDK_META_MASK | GDK_SUPER_MASK)) != 0;
    }

    return TRUE;
}

static gboolean scan_source_enabled(ScanReadMode mode) {
    if (!scan_enabled) {
        return FALSE;
    }

    if (mode == SCAN_READ_AUTO_PRIMARY) {
        return !settings_ref || settings_ref->scan_selection_enabled;
    }
    if (mode == SCAN_READ_AUTO_CLIPBOARD) {
        return settings_ref && settings_ref->scan_clipboard_enabled;
    }

    return TRUE;
}

static void read_clipboard_auto(GdkClipboard *clipboard, ScanReadMode mode) {
    if (clipboard && lookup_callback && scan_source_enabled(mode) && scan_modifier_satisfied()) {
        gdk_clipboard_read_text_async(clipboard, NULL,
                                      on_clipboard_read,
                                      GINT_TO_POINTER(mode));
    }
}

static void read_regular_clipboard_manual(void) {
    if (regular_clipboard && lookup_callback) {
        gdk_clipboard_read_text_async(regular_clipboard, NULL,
                                      on_clipboard_read, GINT_TO_POINTER(SCAN_READ_MANUAL_CLIPBOARD));
    }
}

static void on_clipboard_read(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    ScanReadMode mode = GPOINTER_TO_INT(user_data);
    GdkClipboard *clip = GDK_CLIPBOARD(source_object);
    char *text = gdk_clipboard_read_text_finish(clip, res, NULL);
    
    if (text && *text) {
        if ((mode == SCAN_READ_AUTO_PRIMARY || mode == SCAN_READ_AUTO_CLIPBOARD) &&
            !scan_source_enabled(mode)) {
            g_free(text);
            return;
        }

        if (mode == SCAN_READ_AUTO_PRIMARY &&
            (!last_primary_text || g_strcmp0(last_primary_text, text) != 0)) {
            g_free(last_primary_text);
            last_primary_text = g_strdup(text);
            
            // Only trigger if enabled
            if (scan_enabled) {
                (void)scan_popup_try_show_for_word(text);
            }
        } else if (mode == SCAN_READ_AUTO_CLIPBOARD &&
                   (!last_clipboard_text || g_strcmp0(last_clipboard_text, text) != 0)) {
            g_free(last_clipboard_text);
            last_clipboard_text = g_strdup(text);
            if (scan_enabled) {
                (void)scan_popup_try_show_for_word(text);
            }
        } else if (mode == SCAN_READ_MANUAL_PRIMARY) {
            if (!scan_popup_try_show_for_word(text)) {
                read_regular_clipboard_manual();
            }
        } else if (mode == SCAN_READ_MANUAL_CLIPBOARD) {
            (void)scan_popup_try_show_for_word(text);
        }
    } else if (mode == SCAN_READ_MANUAL_PRIMARY) {
        read_regular_clipboard_manual();
    }
    g_free(text);
}

static gboolean poll_primary_clipboard(gpointer user_data) {
    (void)user_data;
    read_clipboard_auto(primary_clipboard, SCAN_READ_AUTO_PRIMARY);
    read_clipboard_auto(regular_clipboard, SCAN_READ_AUTO_CLIPBOARD);
    return G_SOURCE_CONTINUE;
}

static void on_primary_clipboard_changed(GdkClipboard *clipboard, gpointer user_data) {
    (void)user_data;
    read_clipboard_auto(clipboard, SCAN_READ_AUTO_PRIMARY);
}

static void on_regular_clipboard_changed(GdkClipboard *clipboard, gpointer user_data) {
    (void)user_data;
    read_clipboard_auto(clipboard, SCAN_READ_AUTO_CLIPBOARD);
}

void scan_popup_init(GtkApplication *app, AppSettings *settings,
                     char* (*lookup_cb)(const char *word)) {
    app_ref = app;
    settings_ref = settings;
    lookup_callback = lookup_cb;
    scan_enabled = settings ? settings->scan_popup_enabled : FALSE;

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        primary_clipboard = gdk_display_get_primary_clipboard(display);
        regular_clipboard = gdk_display_get_clipboard(display);
        if (primary_clipboard) {
            g_signal_connect(primary_clipboard, "changed",
                             G_CALLBACK(on_primary_clipboard_changed), NULL);
        }
        if (regular_clipboard) {
            g_signal_connect(regular_clipboard, "changed",
                             G_CALLBACK(on_regular_clipboard_changed), NULL);
        }
    }

    // Polling is kept because GdkClipboard doesn't reliably emit "changed" for PRIMARY on all backends.
    int delay = settings ? settings->scan_popup_delay_ms : 500;
    if (delay < 100) delay = 100;
    poll_source_id = g_timeout_add(delay, poll_primary_clipboard, NULL);
}

void scan_popup_destroy(void) {
    if (poll_source_id != 0) {
        g_source_remove(poll_source_id);
        poll_source_id = 0;
    }
    if (hide_timeout_id != 0) {
        g_source_remove(hide_timeout_id);
        hide_timeout_id = 0;
    }
    if (popup_window) {
        gtk_window_destroy(popup_window);
        popup_window = NULL;
    }
    if (primary_clipboard) {
        g_signal_handlers_disconnect_by_func(primary_clipboard, on_primary_clipboard_changed, NULL);
    }
    if (regular_clipboard) {
        g_signal_handlers_disconnect_by_func(regular_clipboard, on_regular_clipboard_changed, NULL);
    }
    g_free(last_primary_text);
    last_primary_text = NULL;
    g_free(last_clipboard_text);
    last_clipboard_text = NULL;
    primary_clipboard = NULL;
    regular_clipboard = NULL;
    lookup_callback = NULL;
    app_ref = NULL;
    settings_ref = NULL;
}

void scan_popup_set_enabled(gboolean enabled) {
    scan_enabled = enabled;
}

gboolean scan_popup_is_enabled(void) {
    return scan_enabled;
}

void scan_popup_trigger_manual(void) {
    if (primary_clipboard && lookup_callback) {
        gdk_clipboard_read_text_async(primary_clipboard, NULL,
                                      on_clipboard_read,
                                      GINT_TO_POINTER(SCAN_READ_MANUAL_PRIMARY));
    } else {
        read_regular_clipboard_manual();
    }
}
