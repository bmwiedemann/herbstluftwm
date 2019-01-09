#include "stack.h"

#include "client.h"
#include "ewmh.h"
#include "globals.h"
#include "monitor.h"
#include "tag.h"
#include "utils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <iomanip>


static struct HSTreeInterface stack_nth_child(std::shared_ptr<Stack> root, size_t idx);
static size_t                  stack_child_count(std::shared_ptr<Stack> root);

const std::array<const char*, LAYER_COUNT>g_layer_names =
    ArrayInitializer<const char*, LAYER_COUNT>({
     { LAYER_FOCUS	 , "Focus-Layer"      },
     { LAYER_FULLSCREEN  , "Fullscreen-Layer" },
     { LAYER_NORMAL      , "Normal Layer"     },
     { LAYER_FRAMES      , "Frame Layer"      },
}).a;

void stacklist_init() {
}

void stacklist_destroy() {
}

Stack::~Stack() {
    for (int i = 0; i < LAYER_COUNT; i++) {
        if (!top[i].empty()) {
            HSDebug("Warning: %s of stack %p was not empty on destroy\n",
                    g_layer_names[i], (void*)this);
        }
    }
}

static Slice* slice_create() {
    Slice* s = new Slice();
    s->layers.insert(LAYER_NORMAL);
    return s;
}

Slice* slice_create_window(Window window) {
    Slice* s = slice_create();
    s->type = SLICE_WINDOW;
    s->data.window = window;
    return s;
}

Slice* slice_create_frame(Window window) {
    Slice* s = slice_create_window(window);
    s->layers.clear();
    s->layers.insert(LAYER_FRAMES);
    return s;
}


Slice* slice_create_client(HSClient* client) {
    Slice* s = slice_create();
    s->type = SLICE_CLIENT;
    s->data.client = client;
    return s;
}

Slice* slice_create_monitor(HSMonitor* monitor) {
    Slice* s = slice_create();
    s->type = SLICE_MONITOR;
    s->data.monitor = monitor;
    return s;
}

void slice_destroy(Slice* slice) {
    delete slice;
}

HSLayer slice_highest_layer(Slice* slice) {
    if (slice->layers.empty()) {
        return LAYER_COUNT;
    } else {
        return *(slice->layers.begin());
    }
}

void Stack::insert_slice(Slice* elem) {
    for (auto layer : elem->layers) {
        top[layer].insert(top[layer].begin(), elem);
    }
    dirty = true;
}

void Stack::remove_slice(Slice* elem) {
    for (auto layer : elem->layers) {
        auto &v = top[layer];
        v.erase(std::remove(v.begin(), v.end(), elem));
    }
    dirty = true;
}

static void slice_append_caption(HSTree root, Output output) {
    Slice* slice = (Slice*)root;
    GString* monitor_name = g_string_new("");
    switch (slice->type) {
        case SLICE_WINDOW:
            output << "Window 0x" << std::hex << slice->data.window << std::dec;
            break;
        case SLICE_CLIENT:
            output << "Client 0x"
                   << std::hex << slice->data.client->x11Window() << std::dec
                   << " \"" << slice->data.client->title_() << "\"";
            break;
        case SLICE_MONITOR:
            if (slice->data.monitor->name != "") {
                g_string_append_printf(monitor_name, " (\"%s\")",
                                       slice->data.monitor->name->c_str());
            }
            output << "Monitor "
                   << slice->data.monitor->index()
                   << monitor_name->str
                   << " with tag \""
                   << *(slice->data.monitor->tag->name)
                   << "\"";
            break;
    }
    g_string_free(monitor_name, true);
}

static struct HSTreeInterface slice_nth_child(HSTree root, size_t idx) {
    Slice* slice = (Slice*)root;
    assert(slice->type == SLICE_MONITOR);
    return stack_nth_child(slice->data.monitor->tag->stack, idx);
}

static size_t slice_child_count(HSTree root) {
    Slice* slice = (Slice*)root;
    if (slice->type == SLICE_MONITOR) {
        return stack_child_count(slice->data.monitor->tag->stack);
    } else {
        return 0;
    }
}

struct TmpLayer {
    Stack* stack;
    HSLayer    layer;
};

static struct HSTreeInterface layer_nth_child(HSTree root, size_t idx) {
    struct TmpLayer* l = (struct TmpLayer*) root;
    Slice* slice = l->stack->top[l->layer].at(idx);
    HSTreeInterface intface = {
        /* .nth_child      = */ slice_nth_child,
        /* .child_count    = */ slice_child_count,
        /* .append_caption = */ slice_append_caption,
        /* .data           = */ slice,
        /* .destructor     = */ nullptr,
    };
    return intface;
}

static size_t layer_child_count(HSTree root) {
    struct TmpLayer* l = (struct TmpLayer*) root;
    return l->stack->top[l->layer].size();
}

static void layer_append_caption(HSTree root, Output output) {
    struct TmpLayer* l = (struct TmpLayer*) root;
    output << g_layer_names[l->layer];
}


static struct HSTreeInterface stack_nth_child(std::shared_ptr<Stack> root, size_t idx) {
    struct TmpLayer* l = g_new(struct TmpLayer, 1);
    l->stack = root.get(); // TODO: Turn lhs of this assignment into a shared_ptr
    l->layer = (HSLayer) idx;

    HSTreeInterface intface = {
        /* .nth_child      = */ layer_nth_child,
        /* .child_count    = */ layer_child_count,
        /* .append_caption = */ layer_append_caption,
        /* .data           = */ l,
        /* .destructor     = */ (void (*)(HSTree))g_free,
    };
    return intface;
}

static size_t stack_child_count(std::shared_ptr<Stack> root) {
    return LAYER_COUNT;
}

static void monitor_stack_append_caption(HSTree root, Output output) {
    // g_string_append_printf(*output, "Stack of all monitors");
}

int print_stack_command(int argc, char** argv, Output output) {
    struct TmpLayer tl = {
        /* .stack = */ get_monitor_stack(),
        /* .layer = */ LAYER_NORMAL,
    };
    HSTreeInterface intface = {
        /* .nth_child      = */ layer_nth_child,
        /* .child_count    = */ layer_child_count,
        /* .append_caption = */ monitor_stack_append_caption,
        /* .data           = */ &tl,
        /* .destructor     = */ nullptr,
    };
    tree_print_to(&intface, output);
    return 0;
}

int Stack::window_count(bool real_clients) {
    int counter = 0;
    to_window_buf(nullptr, 0, real_clients, &counter);
    return -counter;
}

/* stack to window buf */
struct s2wb {
    int     len;
    Window* buf;
    int     missing; /* number of slices that could not find space in buf */
    bool    real_clients; /* whether to include windows that aren't clients */
    HSLayer layer;  /* the layer the slice should be added to */
};

static void slice_to_window_buf(Slice* s, struct s2wb* data) {
    HSTag* tag;
    if (slice_highest_layer(s) != data->layer) {
        /** slice only is added to its highest layer.
         * just skip it if the slice is not shown on this data->layer */
        return;
    }
    switch (s->type) {
        case SLICE_CLIENT:
            if (data->len) {
                if (data->real_clients) {
                    data->buf[0] = s->data.client->x11Window();
                } else {
                    data->buf[0] = s->data.client->decorationWindow();
                }
                data->buf++;
                data->len--;
            } else {
                data->missing++;
            }
            break;
        case SLICE_WINDOW:
            if (!data->real_clients) {
                if (data->len) {
                    data->buf[0] = s->data.window;
                    data->buf++;
                    data->len--;
                } else {
                    data->missing++;
                }
            }
            break;
        case SLICE_MONITOR:
            tag = s->data.monitor->tag;
            if (!data->real_clients) {
                if (data->len) {
                    data->buf[0] = s->data.monitor->stacking_window;
                    data->buf++;
                    data->len--;
                } else {
                    data->missing++;
                }
            }
            int remain_len = 0; /* remaining length */
            tag->stack->to_window_buf(data->buf, data->len,
                                      data->real_clients, &remain_len);
            int len_used = data->len - remain_len;
            if (remain_len >= 0) {
                data->buf += len_used;
                data->len = remain_len;
            } else {
                data->len = 0;
                data->missing += -remain_len;
            }
            break;
    }
}

void Stack::to_window_buf(Window* buf, int len,
                         bool real_clients, int* remain_len) {
    struct s2wb data = {};
    data.len = len;
    data.buf = buf;
    data.missing = 0;
    data.real_clients = real_clients;

    for (int i = 0; i < LAYER_COUNT; i++) {
        data.layer = (HSLayer)i;
        for (auto slice : top[i]) {
            slice_to_window_buf(slice, &data);
        }
    }
    if (!remain_len) {
        // nothing to do
        return;
    }
    if (data.missing == 0) {
        *remain_len = data.len;
    } else {
        *remain_len = -data.missing;
    }
}

void Stack::restack() {
    if (!dirty) {
        return;
    }
    int count = window_count(false);
    Window* buf = g_new0(Window, count);
    to_window_buf(buf, count, false, nullptr);
    XRestackWindows(g_display, buf, count);
    dirty = false;
    ewmh_update_client_list_stacking();
    g_free(buf);
}

void Stack::raise_slide(Slice* slice) {
    for (auto layer : slice->layers) {
        auto &v = top[layer];
        auto it = std::find(v.begin(), v.end(), slice);
        assert(it != v.end());
        // rotate the range [begin, it+1) in such a way
        // that it becomes the new first element
        std::rotate(v.begin(), it, it + 1);
    }
    dirty = true;
    // TODO: maybe only update the specific range and not the entire stack
    // update
    restack();
}

void Stack::mark_dirty() {
    dirty = true;
}

void Stack::slice_add_layer(Slice* slice, HSLayer layer) {
    if (slice->layers.count(layer) != 0) {
        /* nothing to do */
        return;
    }

    slice->layers.insert(layer);
    top[layer].insert(top[layer].begin(), slice);
    dirty = true;
}

void Stack::slice_remove_layer(Slice* slice, HSLayer layer) {
    /* remove slice from layer in the stack */
    auto &v = top[layer];
    v.erase(std::remove(v.begin(), v.end(), slice));
    dirty = true;

    if (slice->layers.count(layer) == 0) {
        HSDebug("remove layer: slice %p not in %s\n", (void*)slice,
                g_layer_names[layer]);
        return;
    }

    slice->layers.erase(layer);
}

Window Stack::lowest_window() {
    for (int i = LAYER_COUNT - 1; i >= 0; i--) {
        auto &v = top[i];
        for (auto it = v.rbegin(); it != v.rend(); it++) {
            auto slice = *it;
            Window w = 0;
            switch (slice->type) {
                case SLICE_CLIENT:
                    w = slice->data.client->decorationWindow();
                    break;
                case SLICE_WINDOW:
                    w = slice->data.window;
                    break;
                case SLICE_MONITOR:
                    w = slice->data.monitor->tag->stack->lowest_window();
                    break;
            }
            if (w) {
                return w;
            }
        }
    }
    // if no window was found
    return 0;
}

bool Stack::is_layer_empty(HSLayer layer) {
    return top[layer].empty();
}

void Stack::clear_layer(HSLayer layer) {
    while (!is_layer_empty(layer)) {
        slice_remove_layer(top[layer].front(), layer);
        dirty = true;
    }
}

