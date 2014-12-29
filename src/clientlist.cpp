/** Copyright 2011-2013 Thorsten Wißmann. All rights reserved.
 *
 * This software is licensed under the "Simplified BSD License".
 * See LICENSE for details */

#include "clientlist.h"
#include "settings.h"
#include "globals.h"
#include "layout.h"
#include "stack.h"
#include "utils.h"
#include "hook.h"
#include "mouse.h"
#include "ewmh.h"
#include "rules.h"
#include "ipc-protocol.h"
#include "object.h"
#include "decoration.h"
#include "key.h"
// system
#include "glib-backports.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <regex.h>
#include <stdbool.h>
#include <string.h>
// gui
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

using namespace herbstluft;

static int g_monitor_float_treshold = 24;

static int* g_raise_on_focus;
static int* g_snap_gap;

static GHashTable* g_clients; // container of all clients
static HSObject*   g_client_object;

// atoms from dwm.c
// default atoms
enum { WMProtocols, WMDelete, WMState, WMTakeFocus, WMLast };
static Atom g_wmatom[WMLast];

static HSClient* lastfocus = NULL;
static HSDecorationScheme client_scheme_from_triple(HSClient* client, int tripidx);
static int client_get_scheme_triple_idx(HSClient* client);

static bool g_startup = true; // whether hlwm is starting up and is not in the
                              // main event loop yet

HSClient::HSClient() {
    hsobject_init(&this->object);
    window_str = NULL;
    float_size.width = 100;
    float_size.height = 100;
    title = g_string_new("");
    urgent = false;
    fullscreen = false;
    ewmhfullscreen = false;
    pseudotile = false;
    ewmhrequests = true;
    ewmhnotify = true;
    sizehints_floating = true;
    sizehints_tiling = false;
    visible = false;
    tag_ = NULL;
    dragged = false;
    neverfocus = false;
    ignore_unmaps = 0;
    decoration_init(&dec, this);
}

static void fetch_colors() {
    g_window_gap = &(settings_find("window_gap")->value.i);
    g_snap_gap = &(settings_find("snap_gap")->value.i);
    g_raise_on_focus = &(settings_find("raise_on_focus")->value.i);
}

static void client_destroy(HSClient* c) {
    delete c;
}

void clientlist_init() {
    // init regex simple..
    fetch_colors();
    g_wmatom[WMProtocols] = XInternAtom(g_display, "WM_PROTOCOLS", False);
    g_wmatom[WMDelete] = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    g_wmatom[WMState] = XInternAtom(g_display, "WM_STATE", False);
    g_wmatom[WMTakeFocus] = XInternAtom(g_display, "WM_TAKE_FOCUS", False);
    // init actual client list
    g_client_object = hsobject_create_and_link(hsobject_root(), "clients");
    g_clients = g_hash_table_new_full(g_int_hash, g_int_equal,
                                      NULL, (GDestroyNotify)client_destroy);
}

void clientlist_end_startup() {
    g_startup = false;
}

bool HSClient::ignore_unmapnotify() {
    if (ignore_unmaps > 0) {
        ignore_unmaps--;
        return true;
    } else {
        return false;
    }
}

bool clientlist_ignore_unmapnotify(Window win) {
    HSClient* c = get_client_from_window(win);
    return c && c->ignore_unmapnotify();
}

void reset_client_colors() {
    fetch_colors();
    all_monitors_apply_layout();
}

static void client_move_to_floatpos(void* key, void* client_void, void* data) {
    (void)key;
    (void)data;
    HSClient* client = (HSClient*)client_void;
    if (client) {
        int x = client->float_size.x;
        int y = client->float_size.y;
        unsigned int w = client->float_size.width;
        unsigned int h = client->float_size.height;
        XMoveResizeWindow(g_display, client->x11Window(), x, y, w, h);
        XReparentWindow(g_display, client->x11Window(), g_root, x, y);
        ewmh_update_frame_extents(client->x11Window(), 0,0,0,0);
    }
}

static void client_show_window(void* key, void* client_void, void* data) {
    (void)key;
    (void)data;
    HSClient* client = (HSClient*)client_void;
    window_set_visible(client->x11Window(), true);
}

void clientlist_destroy() {
    // move all clients to their original floating position
    g_hash_table_foreach(g_clients, client_move_to_floatpos, NULL);
    g_hash_table_foreach(g_clients, client_show_window, NULL);

    g_hash_table_destroy(g_clients);
    hsobject_unlink_and_destroy(hsobject_root(), g_client_object);
}


void clientlist_foreach(GHFunc func, gpointer data) {
    g_hash_table_foreach(g_clients, func, data);
}

HSClient* get_client_from_window(Window window) {
    return (HSClient*) g_hash_table_lookup(g_clients, &window);
}

#define CLIENT_UPDATE_ATTR(FUNC,MEMBER) do { \
        HSClient* client = container_of(attr->value.b, HSClient, MEMBER); \
        bool val = client->MEMBER; \
        client->MEMBER = ! client->MEMBER ; /* enforce update of MEMBER */ \
        client->FUNC(val); \
        return NULL; \
    }   \
    while (0);

static void client_attr_tag(void* data, GString* output) {
    HSClient* client = (HSClient*) data;
    g_string_append(output, client->tag()->display_name->str);
}

static void client_attr_class(void* data, GString* output) {
    HSClient* client = (HSClient*) data;
    GString* ret = window_class_to_g_string(g_display, client->x11Window());
    g_string_append(output, ret->str);
    g_string_free(ret, true);
}

static void client_attr_instance(void* data, GString* output) {
    HSClient* client = (HSClient*) data;
    GString* ret = window_instance_to_g_string(g_display, client->x11Window());
    g_string_append(output, ret->str);
    g_string_free(ret, true);
}

static GString* client_attr_fullscreen(HSAttribute* attr) {
    CLIENT_UPDATE_ATTR(set_fullscreen, fullscreen);
}

static GString* client_attr_pseudotile(HSAttribute* attr) {
    CLIENT_UPDATE_ATTR(set_pseudotile, pseudotile);
}

static GString* client_attr_urgent(HSAttribute* attr) {
    CLIENT_UPDATE_ATTR(set_urgent_force, urgent);
}

static GString* client_attr_sh_tiling(HSAttribute* attr) {
    HSClient* client = container_of(attr->value.b, HSClient, sizehints_tiling);
    if (!client->is_client_floated() && !client->pseudotile) {
        HSMonitor* mon = find_monitor_with_tag(client->tag());
        if (mon) {
            monitor_apply_layout(mon);
        }
    }
    return NULL;
}

static GString* client_attr_sh_floating(HSAttribute* attr) {
    HSClient* client = container_of(attr->value.b, HSClient, sizehints_floating);
    if (!client->is_client_floated() || client->pseudotile) {
        HSMonitor* mon = find_monitor_with_tag(client->tag());
        if (mon) {
            monitor_apply_layout(mon);
        }
    }
    return NULL;
}

HSClient* manage_client(Window win) {
    if (is_herbstluft_window(g_display, win)) {
        // ignore our own window
        return NULL;
    }
    if (get_client_from_window(win)) {
        return NULL;
    }
    // init client
    HSClient* client = new HSClient;
    client->pid = window_pid(g_display, win);
    HSMonitor* m = get_current_monitor();
    // set to window properties
    client->window = win;
    client->update_title();

    unsigned int border, depth;
    Window root_win;
    int x, y;
    unsigned int w, h;
    XGetGeometry(g_display, win, &root_win, &x, &y, &w, &h, &border, &depth);
    // treat wanted coordinates as floating coords
    client->float_size.x = x;
    client->float_size.y = y;
    client->float_size.width = w;
    client->float_size.height = h;
    client->last_size = client->float_size;

    // apply rules
    HSClientChanges changes;
    client_changes_init(&changes, client);
    rules_apply(client, &changes);
    if (changes.tag_name) {
        client->setTag(find_tag(changes.tag_name->str));
    }
    if (changes.monitor_name) {
        HSMonitor *monitor = string_to_monitor(changes.monitor_name->str);
        if (monitor) {
            // a valid tag was not already found, use the target monitor's tag
            if (!client->tag()) { client->setTag(monitor->tag); }
            // a tag was already found, display it on the target monitor, but
            // only if switchtag is set
            else if (changes.switchtag) {
                monitor_set_tag(monitor, client->tag());
            }
        }
    }

    // Reuse the keymask string
    client->keymask = changes.keymask;

    if (!changes.manage) {
        client_changes_free_members(&changes);
        client_destroy(client);
        // map it... just to be sure
        XMapWindow(g_display, win);
        return NULL;
    }

    // actually manage it
    decoration_setup_frame(client);
    client->fuzzy_fix_initial_position();
    g_hash_table_insert(g_clients, &(client->window), client);
    client->window_str = g_string_sized_new(10);
    g_string_printf(client->window_str, "0x%lx", win);
    hsobject_link(g_client_object, &client->object, client->window_str->str);
    // insert to layout
    if (!client->tag()) {
        client->setTag(m->tag);
    }
    // insert window to the stack
    client->slice = slice_create_client(client);
    stack_insert_slice(client->tag()->stack, client->slice);
    // insert window to the tag
    frame_insert_client(lookup_frame(client->tag()->frame, changes.tree_index->str), client);
    client->update_wm_hints();
    client->updatesizehints();
    if (changes.focus) {
        // give focus to window if wanted
        // TODO: make this faster!
        // WARNING: this solution needs O(C + exp(D)) time where W is the count
        // of clients on this tag and D is the depth of the binary layout tree
        frame_focus_client(client->tag()->frame, client);
    }

    client->object.data = client;

    HSAttribute attributes[] = {
        ATTRIBUTE_STRING(   "winid",        client->window_str,     ATTR_READ_ONLY),
        ATTRIBUTE_STRING(   "title",        client->title,          ATTR_READ_ONLY),
        ATTRIBUTE_STRING(   "keymask",      client->keymask,        ATTR_READ_ONLY),
        ATTRIBUTE_CUSTOM(   "tag",          client_attr_tag,        ATTR_READ_ONLY),
        ATTRIBUTE_INT(      "pid",          client->pid,            ATTR_READ_ONLY),
        ATTRIBUTE_CUSTOM(   "class",        client_attr_class,      ATTR_READ_ONLY),
        ATTRIBUTE_CUSTOM(   "instance",     client_attr_instance,   ATTR_READ_ONLY),
        ATTRIBUTE_BOOL(     "fullscreen",   client->fullscreen,     client_attr_fullscreen),
        ATTRIBUTE_BOOL(     "pseudotile",   client->pseudotile,     client_attr_pseudotile),
        ATTRIBUTE_BOOL(     "ewmhrequests", client->ewmhrequests,   ATTR_ACCEPT_ALL),
        ATTRIBUTE_BOOL(     "ewmhnotify",   client->ewmhnotify,     ATTR_ACCEPT_ALL),
        ATTRIBUTE_BOOL(     "sizehints_tiling",   client->sizehints_tiling, client_attr_sh_tiling),
        ATTRIBUTE_BOOL(     "sizehints_floating", client->sizehints_floating, client_attr_sh_floating),
        ATTRIBUTE_BOOL(     "urgent",       client->urgent,         client_attr_urgent),
        ATTRIBUTE_LAST,
    };
    hsobject_set_attributes(&client->object, attributes);

    ewmh_window_update_tag(client->window, client->tag());
    tag_set_flags_dirty();
    client->set_fullscreen(changes.fullscreen);
    ewmh_update_window_state(client);
    // add client after setting the correct tag for the new client
    // this ensures a panel can read the tag property correctly at this point
    ewmh_add_client(client->window);

    XSetWindowBorderWidth(g_display, client->window,0);
    // specify that the client window survives if hlwm dies, i.e. it will be
    // reparented back to root
    XChangeSaveSet(g_display, client->window, SetModeInsert);
    XReparentWindow(g_display, client->window, client->dec.decwin, 40, 40);
    if (g_startup) client->ignore_unmaps++;
    // get events from window
    XSelectInput(g_display, client->dec.decwin, (EnterWindowMask | LeaveWindowMask |
                            ButtonPressMask | ButtonReleaseMask |
                            ExposureMask |
                            SubstructureRedirectMask | FocusChangeMask));
    XSelectInput(g_display, win, CLIENT_EVENT_MASK);

    HSMonitor* monitor = find_monitor_with_tag(client->tag());
    if (monitor) {
        if (monitor != get_current_monitor()
            && changes.focus && changes.switchtag) {
            monitor_set_tag(get_current_monitor(), client->tag());
        }
        // TODO: monitor_apply_layout() maybe is called twice here if it
        // already is called by monitor_set_tag()
        monitor_apply_layout(monitor);
        client->set_visible(true);
    } else {
        if (changes.focus && changes.switchtag) {
            monitor_set_tag(get_current_monitor(), client->tag());
            client->set_visible(true);
        }
    }
    client->send_configure();

    client_changes_free_members(&changes);
    grab_client_buttons(client, false);

    return client;
}

void unmanage_client(Window win) {
    HSClient* client = get_client_from_window(win);
    if (!client) {
        return;
    }
    if (client->dragged) {
        mouse_stop_drag();
    }
    // remove from tag
    frame_remove_client(client->tag()->frame, client);
    // ignore events from it
    XSelectInput(g_display, win, 0);
    //XUngrabButton(g_display, AnyButton, AnyModifier, win);
    // permanently remove it
    XUnmapWindow(g_display, client->dec.decwin);
    XReparentWindow(g_display, win, g_root, 0, 0);
    // delete ewmh-properties and ICCCM-Properties such that the client knows
    // that he has been unmanaged and now the client is allowed to be mapped
    // again (e.g. if it is some dialog)
    ewmh_clear_client_properties(client);
    XDeleteProperty(g_display, client->window, g_wmatom[WMState]);
    HSTag* tag = client->tag();
    g_hash_table_remove(g_clients, &win);
    client = NULL;
    // and arrange monitor after the client has been removed from the stack
    HSMonitor* m = find_monitor_with_tag(tag);
    tag_update_focus_layer(tag);
    if (m) monitor_apply_layout(m);
    ewmh_remove_client(win);
    tag_set_flags_dirty();
}

// destroys a special client
HSClient::~HSClient() {
    hsobject_unlink(g_client_object, &object);
    decoration_free(&dec);
    if (lastfocus == this) {
        lastfocus = NULL;
    }
    if (tag() && slice) {
        stack_remove_slice(tag()->stack, slice);
    }
    if (slice) {
        slice_destroy(slice);
    }
    if (title) {
        /* free window title */
        g_string_free(title, true);
    }
    if (window_str) {
        g_string_free(window_str, true);
    }
    if (keymask) {
        g_string_free(keymask, true);
    }
    hsobject_free(&object);
}

static int client_get_scheme_triple_idx(HSClient* client) {
    if (client->fullscreen) return HSDecSchemeFullscreen;
    else if (client->is_client_floated()) return HSDecSchemeFloating;
    else if (client->needs_minimal_dec(NULL)) return HSDecSchemeMinimal;
    else return HSDecSchemeTiling;
}

bool HSClient::needs_minimal_dec(HSFrame* frame) {
    if (!frame) {
        frame = find_frame_with_client(this->tag()->frame, this);
        HSAssert(frame != NULL);
    }
    if (!smart_window_surroundings_active(frame)) return false;
    if (this->pseudotile) return false;
    if (this->is_client_floated()) return false;
    return true;
}

void HSClient::window_unfocus() {
    grab_client_buttons(this, false);
}

void HSClient::window_unfocus_last() {
    if (lastfocus) {
        lastfocus->window_unfocus();
    }
    hsobject_unlink_by_name(g_client_object, "focus");
    // give focus to root window
    XSetInputFocus(g_display, g_root, RevertToPointerRoot, CurrentTime);
    if (lastfocus) {
        /* only emit the hook if the focus *really* changes */
        hook_emit_list("focus_changed", "0x0", "", NULL);
        ewmh_update_active_window(None);
        tag_update_each_focus_layer();

        // Enable all keys in the root window
        key_set_keymask(get_current_monitor()->tag, 0);
    }
    lastfocus = 0;
}

void HSClient::window_focus() {
    // set keyboard focus
    if (!this->neverfocus) {
        XSetInputFocus(g_display, this->window, RevertToPointerRoot, CurrentTime);
    }
    else this->sendevent(g_wmatom[WMTakeFocus]);

    if (this != lastfocus) {
        /* FIXME: this is a workaround because window_focus always is called
         * twice.  see BUGS for more information
         *
         * only emit the hook if the focus *really* changes */
        // unfocus last one
        if (lastfocus) {
            lastfocus->window_unfocus();
        }
        hsobject_link(g_client_object, &object, "focus");
        ewmh_update_active_window(this->window);
        tag_update_each_focus_layer();
        const char* title = this->title->str;
        char winid_str[STRING_BUF_SIZE];
        snprintf(winid_str, STRING_BUF_SIZE, "0x%x", (unsigned int)this->window);
        hook_emit_list("focus_changed", winid_str, title, NULL);
    }

    // change window-colors
    //HSDebug("window_focus ACTIVE: 0x%lx\n", client->window);
    //client_setup_border(client, true);

    lastfocus = this;
    /* do some specials for the max layout */
    bool is_max_layout = frame_focused_client(g_cur_frame) == this
                         && g_cur_frame->content.clients.layout == LAYOUT_MAX
                         && get_current_monitor()->tag->floating == false;
    if (*g_raise_on_focus || is_max_layout) {
        this->raise();
    }
    tag_update_focus_layer(get_current_monitor()->tag);
    grab_client_buttons(this, true);
    key_set_keymask(this->tag(), this);
    this->set_urgent(false);
}

void HSClient::setup_border(bool focused) {
    if (focused) {
        decoration_change_scheme(this,
            g_decorations[client_get_scheme_triple_idx(this)].active);
    } else if (this->urgent) {
        decoration_change_scheme(this,
            g_decorations[client_get_scheme_triple_idx(this)].urgent);
    } else {
        decoration_change_scheme(this,
            g_decorations[client_get_scheme_triple_idx(this)].normal);
    }
}

void HSClient::resize_fullscreen(HSMonitor* m) {
    if (!!m) {
        HSDebug("client_resize_fullscreen() got invalid parameters\n");
        return;
    }
    decoration_resize_outline(this, m->rect,
        client_scheme_from_triple(this, HSDecSchemeFullscreen));
}

void HSClient::raise() {
    stack_raise_slide(this->tag()->stack, this->slice);
}

static HSDecorationScheme client_scheme_from_triple(HSClient* client, int tripidx) {
    if (get_current_client() == client) {
        return g_decorations[tripidx].active;
    } else if (client->urgent) {
        return g_decorations[tripidx].urgent;
    } else {
        return g_decorations[tripidx].normal;
    }
}

void HSClient::resize_tiling(Rectangle rect, HSFrame* frame) {
    HSMonitor* m;
    if (this->fullscreen && (m = find_monitor_with_tag(this->tag()))) {
        resize_fullscreen(m);
        return;
    }
    // apply border width
    if (!this->pseudotile && !smart_window_surroundings_active(frame)) {
        // apply window gap
        rect.width -= *g_window_gap;
        rect.height -= *g_window_gap;
    }
    HSDecorationScheme scheme = client_scheme_from_triple(this, HSDecSchemeTiling);
    if (this->pseudotile) {
        auto inner = this->float_size;
        applysizehints(&inner.width, &inner.height);
        auto outline = inner_rect_to_outline(inner, scheme);
        rect.x += MAX(0, (rect.width - outline.width)/2);
        rect.y += MAX(0, (rect.height - outline.height)/2);
        rect.width = MIN(outline.width, rect.width);
        rect.height = MIN(outline.height, rect.height);
        scheme.tight_decoration = true;
    }
    if (needs_minimal_dec(frame)) {
        scheme = client_scheme_from_triple(this, HSDecSchemeMinimal);
    }
    decoration_resize_outline(this, rect, scheme);
}

// from dwm.c
bool HSClient::applysizehints(int *w, int *h) {
    bool baseismin;

    /* set minimum possible */
    *w = MAX(1, *w);
    *h = MAX(1, *h);
    if(*h < WINDOW_MIN_HEIGHT)
        *h = WINDOW_MIN_HEIGHT;
    if(*w < WINDOW_MIN_WIDTH)
        *w = WINDOW_MIN_WIDTH;
    bool sizehints = (this->is_client_floated() || this->pseudotile)
                        ? this->sizehints_floating
                        : this->sizehints_tiling;
    if(sizehints) {
        /* see last two sentences in ICCCM 4.1.2.3 */
        baseismin = this->basew == this->minw && this->baseh == this->minh;
        if(!baseismin) { /* temporarily remove base dimensions */
            *w -= this->basew;
            *h -= this->baseh;
        }
        /* adjust for aspect limits */
        if(this->mina > 0 && this->maxa > 0) {
            if(this->maxa < (float)*w / *h)
                *w = *h * this->maxa + 0.5;
            else if(this->mina < (float)*h / *w)
                *h = *w * this->mina + 0.5;
        }
        if(baseismin) { /* increment calculation requires this */
            *w -= this->basew;
            *h -= this->baseh;
        }
        /* adjust for increment value */
        if(this->incw)
            *w -= *w % this->incw;
        if(this->inch)
            *h -= *h % this->inch;
        /* restore base dimensions */
        *w += this->basew;
        *h += this->baseh;
        *w = MAX(*w, this->minw);
        *h = MAX(*h, this->minh);
        if(this->maxw)
            *w = MIN(*w, this->maxw);
        if(this->maxh)
            *h = MIN(*h, this->maxh);
    }
    return *w != this->last_size.width || *h != this->last_size.height;
}

bool HSClient::applysizehints_xy(int *x, int *y, int *w, int *h) {
    return applysizehints(w,h) || *x != this->last_size.x
                               || *y != this->last_size.y;
}

// from dwm.c
void HSClient::updatesizehints() {
    long msize;
    XSizeHints size;

    if(!XGetWMNormalHints(g_display, this->window, &size, &msize))
        /* size is uninitialized, ensure that size.flags aren't used */
        size.flags = PSize;
    if(size.flags & PBaseSize) {
        this->basew = size.base_width;
        this->baseh = size.base_height;
    }
    else if(size.flags & PMinSize) {
        this->basew = size.min_width;
        this->baseh = size.min_height;
    } else {
        this->basew = this->baseh = 0;
    }
    if(size.flags & PResizeInc) {
        this->incw = size.width_inc;
        this->inch = size.height_inc;
    }
    else
        this->incw = this->inch = 0;
    if(size.flags & PMaxSize) {
        this->maxw = size.max_width;
        this->maxh = size.max_height;
    } else {
        this->maxw = this->maxh = 0;
    }
    if(size.flags & PMinSize) {
        this->minw = size.min_width;
        this->minh = size.min_height;
    }
    else if(size.flags & PBaseSize) {
        this->minw = size.base_width;
        this->minh = size.base_height;
    } else {
        this->minw = this->minh = 0;
    }
    if(size.flags & PAspect) {
        this->mina = (float)size.min_aspect.y / size.min_aspect.x;
        this->maxa = (float)size.max_aspect.x / size.max_aspect.y;
    } else {
        this->maxa = this->mina = 0.0;
    }
    //this->isfixed = (this->maxw && this->minw && this->maxh && this->minh
    //             && this->maxw == this->minw && this->maxh == this->minh);
}




void HSClient::send_configure() {
    XConfigureEvent ce;
    ce.type = ConfigureNotify,
    ce.display = g_display,
    ce.event = this->window,
    ce.window = this->window,
    ce.x = this->dec.last_inner_rect.x,
    ce.y = this->dec.last_inner_rect.y,
    ce.width = MAX(this->dec.last_inner_rect.width, WINDOW_MIN_WIDTH),
    ce.height = MAX(this->dec.last_inner_rect.height, WINDOW_MIN_HEIGHT),
    ce.border_width = 0,
    ce.above = None,
    ce.override_redirect = False,
    XSendEvent(g_display, this->window, False, StructureNotifyMask, (XEvent *)&ce);
}

void HSClient::resize_floating(HSMonitor* m) {
    if (!!m) return;
    if (fullscreen) {
        resize_fullscreen(m);
        return;
    }
    auto rect = this->float_size;
    rect.x += m->rect.x;
    rect.x += m->rect.y;
    rect.x += m->pad_left;
    rect.y += m->pad_up;
    // ensure position is on monitor
    int space = g_monitor_float_treshold;
    rect.x =
        CLAMP(rect.x,
              m->rect.x + m->pad_left - rect.width + space,
              m->rect.x + m->rect.width - m->pad_left - m->pad_right - space);
    rect.y =
        CLAMP(rect.y,
              m->rect.y + m->pad_up - rect.height + space,
              m->rect.y + m->rect.height - m->pad_up - m->pad_down - space);
    decoration_resize_inner(this, rect,
        client_scheme_from_triple(this, HSDecSchemeFloating));
}

Rectangle HSClient::outer_floating_rect() {
    return inner_rect_to_outline(float_size, dec.last_scheme);
}

int close_command(int argc, char** argv, GString* output) {
    Window win;
    HSClient* client = NULL;
    win = string_to_client((argc > 1) ? argv[1] : "", &client);
    if (win) window_close(win);
    else return HERBST_INVALID_ARGUMENT;
    return 0;
}

bool HSClient::is_client_floated() {
    return tag()->floating;
}

void window_close(Window window) {
    XEvent ev;
    ev.type = ClientMessage;
    ev.xclient.window = window;
    ev.xclient.message_type = g_wmatom[WMProtocols];
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = g_wmatom[WMDelete];
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(g_display, window, False, NoEventMask, &ev);
}

void window_set_visible(Window win, bool visible) {
    static int (*action[])(Display*,Window) = {
        XUnmapWindow,
        XMapWindow,
    };
    unsigned long event_mask = CLIENT_EVENT_MASK;
    XGrabServer(g_display);
    XSelectInput(g_display, win, event_mask & ~StructureNotifyMask);
    XSelectInput(g_display, g_root, ROOT_EVENT_MASK & ~SubstructureNotifyMask);
    action[visible](g_display, win);
    XSelectInput(g_display, win, event_mask);
    XSelectInput(g_display, g_root, ROOT_EVENT_MASK);
    XUngrabServer(g_display);
}

void HSClient::set_visible(bool visible) {
    if (visible == this->visible) return;
    if (visible) {
        /* Grab the server to make sure that the frame window is mapped before
           the client gets its MapNotify, i.e. to make sure the client is
           _visible_ when it gets MapNotify. */
        XGrabServer(g_display);
        window_update_wm_state(this->window, WmStateNormalState);
        XMapWindow(g_display, this->window);
        XMapWindow(g_display, this->dec.decwin);
        XUngrabServer(g_display);
    } else {
        /* we unmap the client itself so that we can get MapRequest
           events, and because the ICCCM tells us to! */
        XUnmapWindow(g_display, this->dec.decwin);
        XUnmapWindow(g_display, this->window);
        window_update_wm_state(this->window, WmStateWithdrawnState);
        this->ignore_unmaps++;
    }
    this->visible = visible;
}

// heavily inspired by dwm.c
void HSClient::set_urgent(bool state) {
    if (this->urgent == state) {
        // nothing to do
        return;
    }
    set_urgent_force(state);
}

void HSClient::set_urgent_force(bool state) {
    char winid_str[STRING_BUF_SIZE];
    snprintf(winid_str, STRING_BUF_SIZE, "0x%lx", this->window);
    hook_emit_list("urgent", state ? "on" : "off", winid_str, NULL);

    this->urgent = state;

    setup_border(this == frame_focused_client(g_cur_frame));

    XWMHints *wmh;
    if(!(wmh = XGetWMHints(g_display, this->window)))
        return;

    if (state) {
        wmh->flags |= XUrgencyHint;
    } else {
        wmh->flags &= ~XUrgencyHint;
    }

    XSetWMHints(g_display, this->window, wmh);
    XFree(wmh);
    // report changes to tags
    tag_set_flags_dirty();
}

// heavily inspired by dwm.c
void HSClient::update_wm_hints() {
    XWMHints* wmh = XGetWMHints(g_display, this->window);
    if (!wmh) {
        return;
    }

    HSClient* focused_client = frame_focused_client(g_cur_frame);
    if ((focused_client == this)
        && wmh->flags & XUrgencyHint) {
        // remove urgency hint if window is focused
        wmh->flags &= ~XUrgencyHint;
        XSetWMHints(g_display, this->window, wmh);
    } else {
        bool newval = (wmh->flags & XUrgencyHint) ? true : false;
        if (newval != this->urgent) {
            this->urgent = newval;
            char winid_str[STRING_BUF_SIZE];
            snprintf(winid_str, STRING_BUF_SIZE, "0x%lx", this->window);
            this->setup_border(focused_client == this);
            hook_emit_list("urgent", this->urgent ? "on":"off", winid_str, NULL);
            tag_set_flags_dirty();
        }
    }
    if (wmh->flags & InputHint) {
        this->neverfocus = !wmh->input;
    } else {
        this->neverfocus = false;
    }
    XFree(wmh);
}

void HSClient::update_title() {
    GString* new_name = window_property_to_g_string(g_display,
        this->window, g_netatom[NetWmName]);
    if (!new_name) {
        char* ch_new_name = NULL;
        /* if ewmh name isn't set, then fall back to WM_NAME */
        if (0 != XFetchName(g_display, this->window, &ch_new_name)) {
            new_name = g_string_new(ch_new_name);
            XFree(ch_new_name);
        } else {
            new_name = g_string_new("");
            HSDebug("no title for window %lx found, using \"\"\n",
                    this->window);
        }
    }
    bool changed = (0 != strcmp(this->title->str, new_name->str));
    g_string_free(this->title, true);
    this->title = new_name;
    if (changed && get_current_client() == this) {
        char buf[STRING_BUF_SIZE];
        snprintf(buf, STRING_BUF_SIZE, "0x%lx", this->window);
        hook_emit_list("window_title_changed", buf, this->title->str, NULL);
    }
}

HSClient* get_current_client() {
    return frame_focused_client(g_cur_frame);
}

void HSClient::set_fullscreen(bool state) {
    if (this->fullscreen == state) return;
    this->fullscreen = state;
    if (this->ewmhnotify) {
        this->ewmhfullscreen = state;
    }
    HSStack* stack = this->tag()->stack;
    if (state) {
        stack_slice_add_layer(stack, this->slice, LAYER_FULLSCREEN);
    } else {
        stack_slice_remove_layer(stack, this->slice, LAYER_FULLSCREEN);
    }
    tag_update_focus_layer(this->tag());
    monitor_apply_layout(find_monitor_with_tag(this->tag()));

    char buf[STRING_BUF_SIZE];
    snprintf(buf, STRING_BUF_SIZE, "0x%lx", this->window);
    ewmh_update_window_state(this);
    hook_emit_list("fullscreen", state ? "on" : "off", buf, NULL);
}

void HSClient::set_pseudotile(bool state) {
    this->pseudotile = state;
    monitor_apply_layout(find_monitor_with_tag(this->tag()));
}

int client_set_property_command(int argc, char** argv) {
    const char* action = (argc > 1) ? argv[1] : "toggle";

    HSClient* client = get_current_client();
    if (!client) {
        // nothing to do
        return 0;
    }

    struct {
        const char* name;
        void (HSClient::*func) (bool);
        bool* value;
    } properties[] = {
        { "fullscreen",   &HSClient::set_fullscreen, &client->fullscreen    },
        { "pseudotile",   &HSClient::set_pseudotile, &client->pseudotile    },
    };

    // find the property
    int i;
    for  (i = 0; i < LENGTH(properties); i++) {
        if (!strcmp(properties[i].name, argv[0])) {
            break;
        }
    }
    if (i >= LENGTH(properties)) {
        return HERBST_INVALID_ARGUMENT;
    }

    // if found, then change it
    bool old_value = *(properties[i].value);
    bool state = string_to_bool(action, *(properties[i].value));
    if (state != old_value) {
        (client->*(properties[i].func))(state);
    }
    return 0;
}

static bool is_client_urgent(void* key, HSClient* client, void* data) {
    (void) key;
    (void) data;
    return client->urgent;
}

HSClient* get_urgent_client() {
    return (HSClient*)g_hash_table_find(g_clients, (GHRFunc)is_client_urgent, NULL);
}

/**
 * \brief   Resolve a window description to a client or a window id
 *
 * \param   str     Describes the window: "" means the focused one, "urgent"
 *                  resolves to a arbitrary urgent window, "0x..." just
 *                  resolves to the given window given its hexadecimal window id,
 *                  a decimal number its decimal window id.
 * \param   ret_client  The client pointer is stored there if ret_client is
 *                      given and the specified window is managed.
 * \return          The resolved window id is stored there if the according
 *                  window has been found
 */
Window string_to_client(const char* str, HSClient** ret_client) {
    Window win = 0;
    if (!strcmp(str, "")) {
        HSClient* client = get_current_client();
        win = client ? client->window : 0;
        if (ret_client) {
            *ret_client = client;
        }
    } else if (!strcmp(str, "urgent")) {
        HSClient* client = get_urgent_client();
        if (client) {
            win = client->window;
        }
        if (ret_client) {
            *ret_client = client;
        }
    } else if (1 == sscanf(str, "0x%lx", (long unsigned int*)&win)) {
        if (ret_client) {
            *ret_client = get_client_from_window(win);
        }
    } else if (1 == sscanf(str, "%lu", (long unsigned int*)&win)) {
        if (ret_client) {
            *ret_client = get_client_from_window(win);
        }
    }
    return win;
}

// mainly from dwm.c
bool HSClient::sendevent(Atom proto) {
    int n;
    Atom *protocols;
    bool exists = false;
    XEvent ev;

    if (XGetWMProtocols(g_display, this->window, &protocols, &n)) {
        while (!exists && n--)
            exists = protocols[n] == proto;
        XFree(protocols);
    }
    if (exists) {
        ev.type = ClientMessage;
        ev.xclient.window = this->window;
        ev.xclient.message_type = g_wmatom[WMProtocols];
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = proto;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(g_display, this->window, False, NoEventMask, &ev);
    }
    return exists;
}

void HSClient::set_dragged(bool drag_state) {
    if (drag_state == dragged) return;
    dragged = drag_state;
    if (drag_state == true) {
        hsobject_link(g_client_object, &object, "dragged");
    } else {
        hsobject_unlink_by_name(g_client_object, "dragged");
    }
}

void HSClient::fuzzy_fix_initial_position() {
    // find out the top-left-most position of the decoration,
    // considering the current settings of possible floating decorations
    int extreme_x = float_size.x;
    int extreme_y = float_size.y;
    HSDecTriple* t = &g_decorations[HSDecSchemeFloating];
    auto r = inner_rect_to_outline(float_size, t->active);
    extreme_x = MIN(extreme_x, r.x);
    extreme_y = MIN(extreme_y, r.y);
    r = inner_rect_to_outline(float_size, t->normal);
    extreme_x = MIN(extreme_x, r.x);
    extreme_y = MIN(extreme_y, r.y);
    r = inner_rect_to_outline(float_size, t->urgent);
    extreme_x = MIN(extreme_x, r.x);
    extreme_y = MIN(extreme_y, r.y);
    // if top left corner might be outside of the monitor, move it accordingly
    if (extreme_x < 0) { float_size.x += abs(extreme_x); }
    if (extreme_y < 0) { float_size.y += abs(extreme_y); }
}

