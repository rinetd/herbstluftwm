/** Copyright 2011-2012 Thorsten Wißmann. All rights reserved.
 *
 * This software is licensed under the "Simplified BSD License".
 * See LICENSE for details */

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif /* XINERAMA */

#include "globals.h"
#include "ipc-protocol.h"
#include "utils.h"
#include "mouse.h"
#include "hook.h"
#include "layout.h"
#include "tag.h"
#include "ewmh.h"
#include "monitor.h"
#include "settings.h"
#include "stack.h"
#include "clientlist.h"

int* g_monitors_locked;
int* g_swap_monitors_to_get_tag;
int* g_smart_frame_surroundings;
int* g_mouse_recenter_gap;
HSStack* g_monitor_stack;
GArray*     g_monitors; // Array of HSMonitor*

typedef struct RectList {
    XRectangle rect;
    struct RectList* next;
} RectList;

static RectList* reclist_insert_disjoint(RectList* head, RectList* mt);

void monitor_init() {
    g_monitors_locked = &(settings_find("monitors_locked")->value.i);
    g_cur_monitor = 0;
    g_monitors = g_array_new(false, false, sizeof(HSMonitor*));
    g_swap_monitors_to_get_tag = &(settings_find("swap_monitors_to_get_tag")->value.i);
    g_smart_frame_surroundings = &(settings_find("smart_frame_surroundings")->value.i);
    g_mouse_recenter_gap       = &(settings_find("mouse_recenter_gap")->value.i);
    g_monitor_stack = stack_create();
}

void monitor_destroy() {
    for (int i = 0; i < g_monitors->len; i++) {
        HSMonitor* m = monitor_with_index(i);
        stack_remove_slice(g_monitor_stack, m->slice);
        slice_destroy(m->slice);
        g_free(m);
    }
    stack_destroy(g_monitor_stack);
    g_array_free(g_monitors, true);
}

void monitor_apply_layout(HSMonitor* monitor) {
    if (monitor) {
        if (*g_monitors_locked) {
            monitor->dirty = true;
            return;
        }
        monitor->dirty = false;
        XRectangle rect = monitor->rect;
        // apply pad
        rect.x += monitor->pad_left;
        rect.width -= (monitor->pad_left + monitor->pad_right);
        rect.y += monitor->pad_up;
        rect.height -= (monitor->pad_up + monitor->pad_down);
        if (!*g_smart_frame_surroundings || monitor->tag->frame->type == TYPE_FRAMES ) {
            // apply frame gap
            rect.x += *g_frame_gap;
            rect.y += *g_frame_gap;
            rect.height -= *g_frame_gap;
            rect.width -= *g_frame_gap;
        }
        monitor_restack(monitor);
        if (monitor->tag->floating) {
            frame_apply_floating_layout(monitor->tag->frame, monitor);
        } else {
            frame_apply_layout(monitor->tag->frame, rect);
            if (!monitor->lock_frames && !monitor->tag->floating) {
                frame_update_frame_window_visibility(monitor->tag->frame);
            }
        }
        if (get_current_monitor() == monitor) {
            frame_focus_recursive(monitor->tag->frame);
        }
        // remove all enternotify-events from the event queue that were
        // generated while arranging the clients on this monitor
        XEvent ev;
        XSync(g_display, False);
        while(XCheckMaskEvent(g_display, EnterWindowMask, &ev));
    }
}

int list_monitors(int argc, char** argv, GString* output) {
    (void)argc;
    (void)argv;
    int i;
    GString* monitor_name = g_string_new("");
    for (i = 0; i < g_monitors->len; i++) {
        HSMonitor* monitor = monitor_with_index(i);
        if (monitor->name != NULL ) {
            g_string_printf(monitor_name, ", named \"%s\"",
                            monitor->name->str);
        } else {
            g_string_truncate(monitor_name, 0);
        }
        g_string_append_printf(output, "%d: %dx%d%+d%+d with tag \"%s\"%s%s%s\n",
            i,
            monitor->rect.width, monitor->rect.height,
            monitor->rect.x, monitor->rect.y,
            monitor->tag ? monitor->tag->name->str : "???",
            monitor_name->str,
            (g_cur_monitor == i) ? " [FOCUS]" : "",
            monitor->lock_tag ? " [LOCKED]" : "");
    }
    g_string_free(monitor_name, true);
    return 0;
}

int list_padding(int argc, char** argv, GString* output) {
    HSMonitor* monitor;
    if (argc < 2) {
        monitor = get_current_monitor();
    } else {
        monitor = string_to_monitor(argv[1]);
        if (monitor == NULL) {
            g_string_append_printf(output,
                "%s: Monitor \"%s\" not found!\n", argv[0], argv[1]);
            return HERBST_INVALID_ARGUMENT;
        }
    }
    g_string_append_printf(output, "%d %d %d %d\n",
        monitor->pad_up,
        monitor->pad_right,
        monitor->pad_down,
        monitor->pad_left);
    return 0;
}

static bool rects_intersect(RectList* m1, RectList* m2) {
    XRectangle *r1 = &m1->rect, *r2 = &m2->rect;
    bool is = TRUE;
    is = is && intervals_intersect(r1->x, r1->x + r1->width,
                                   r2->x, r2->x + r2->width);
    is = is && intervals_intersect(r1->y, r1->y + r1->height,
                                   r2->y, r2->y + r2->height);
    return is;
}

static XRectangle intersection_area(RectList* m1, RectList* m2) {
    XRectangle r; // intersection between m1->rect and m2->rect
    r.x = MAX(m1->rect.x, m2->rect.x);
    r.y = MAX(m1->rect.y, m2->rect.y);
    // the bottom right coordinates of the rects
    int br1_x = m1->rect.x + m1->rect.width;
    int br1_y = m1->rect.y + m1->rect.height;
    int br2_x = m2->rect.x + m2->rect.width;
    int br2_y = m2->rect.y + m2->rect.height;
    r.width = MIN(br1_x, br2_x) - r.x;
    r.height = MIN(br1_y, br2_y) - r.y;
    return r;
}

static RectList* rectlist_create_simple(int x1, int y1, int x2, int y2) {
    if (x1 >= x2 || y1 >= y2) {
        return NULL;
    }
    RectList* r = g_new0(RectList, 1);
    r->rect.x = x1;
    r->rect.y = y1;
    r->rect.width  = x2 - x1;
    r->rect.height = y2 - y1;
    r->next = NULL;
    return r;
}

static RectList* insert_rect_border(RectList* head,
                                    XRectangle large, XRectangle center)
{
    // given a large rectangle and a center which guaranteed to be a subset of
    // the large rect, the task is to split "large" into pieces and insert them
    // like this:
    //
    // +------- large ---------+
    // |         top           |
    // |------+--------+-------|
    // | left | center | right |
    // |------+--------+-------|
    // |        bottom         |
    // +-----------------------+
    RectList *top, *left, *right, *bottom;
    // coordinates of the bottom right corner of large
    int br_x = large.x + large.width, br_y = large.y + large.height;
    RectList* (*r)(int,int,int,int) = rectlist_create_simple;
    top   = r(large.x, large.y, large.x + large.width, center.y);
    left  = r(large.x, center.y, center.x, center.y + center.height);
    right = r(center.x + center.width, center.y, br_x, center.y + center.height);
    bottom= r(large.x, center.y + center.height, br_x, br_y);

    RectList* parts[] = { top, left, right, bottom };
    for (int i = 0; i < LENGTH(parts); i++) {
        head = reclist_insert_disjoint(head, parts[i]);
    }
    return head;
}

// insert a new element without any intersections into the given list
RectList* reclist_insert_disjoint(RectList* head, RectList* element) {
    if (!element) {
        return head;
    } else if (!head) {
        // if the list is empty, then intersection-free insertion is trivial
        element->next = NULL;
        return element;
    } else if (!rects_intersect(head, element)) {
        head->next = reclist_insert_disjoint(head->next, element);
        return head;
    } else {
        // element intersects with the head rect
        XRectangle center = intersection_area(head, element);
        XRectangle large = head->rect;
        head->rect = center;
        head->next = insert_rect_border(head->next, large, center);
        head->next = insert_rect_border(head->next, element->rect, center);
        g_free(element);
        return head;
    }
}

static void rectlist_free(RectList* head) {
    if (!head) return;
    RectList* next = head->next;
    g_free(head);
    rectlist_free(next);
}

static size_t rectlist_len(RectList* head) {
    if (!head) return 0;
    return 1 + rectlist_len(head->next);
}

static void rectlist_to_array(RectList* head, XRectangle* rects) {
    if (!head) return;
    *rects = head->rect;
    rectlist_to_array(head->next, rects + 1);
}

static RectList* disjoin_rects(XRectangle* buf, size_t count) {
    RectList* cur;
    struct RectList* rects = NULL;
    for (int i = 0; i < count; i++) {
        cur = g_new0(RectList, 1);
        cur->rect = buf[i];
        rects = reclist_insert_disjoint(rects, cur);
    }
    return rects;
}


int disjoin_rects_command(int argc, char** argv, GString* output) {
    (void)SHIFT(argc, argv);
    if (argc < 1) {
        return HERBST_NEED_MORE_ARGS;
    }
    XRectangle* buf = g_new(XRectangle, argc);
    for (int i = 0; i < argc; i++) {
        buf[i] = parse_rectangle(argv[i]);
    }

    RectList* rects = disjoin_rects(buf, argc);
    for (RectList* cur = rects; cur; cur = cur->next) {
        XRectangle r = cur->rect;
        g_string_append_printf(output, "%dx%d%+d%+d\n",
            r.width, r.height, r.x, r.y);
    }
    rectlist_free(rects);
    g_free(buf);
    return 0;
}

int set_monitor_rects_command(int argc, char** argv, GString* output) {
    (void)SHIFT(argc, argv);
    if (argc < 1) {
        return HERBST_NEED_MORE_ARGS;
    }
    XRectangle* templates = g_new0(XRectangle, argc);
    for (int i = 0; i < argc; i++) {
        templates[i] = parse_rectangle(argv[i]);
    }
    int status = set_monitor_rects(templates, argc);
    g_free(templates);
    if (status == HERBST_TAG_IN_USE) {
        g_string_append_printf(output,
            "%s: There are not enough free tags\n", argv[0]);
    } else if (status == HERBST_INVALID_ARGUMENT) {
        g_string_append_printf(output,
            "%s: Need at least one rectangle\n", argv[0]);
    }
    return status;
}

int set_monitor_rects(XRectangle* templates, size_t count) {
    if (count < 1) {
        return HERBST_INVALID_ARGUMENT;
    }
    HSTag* tag = NULL;
    int i;
    for (i = 0; i < MIN(count, g_monitors->len); i++) {
        HSMonitor* m = monitor_with_index(i);
        m->rect = templates[i];
    }
    // add additional monitors
    for (; i < count; i++) {
        tag = find_unused_tag();
        if (!tag) {
            return HERBST_TAG_IN_USE;
        }
        add_monitor(templates[i], tag, NULL);
        frame_show_recursive(tag->frame);
    }
    // remove monitors if there are too much
    while (i < g_monitors->len) {
        remove_monitor(i);
    }
    all_monitors_apply_layout();
    return 0;
}

int find_monitor_index_by_name(char* name) {
    int i;
    for (i = 0; i < g_monitors->len; i++) {
        HSMonitor* mon = monitor_with_index(i);
        if (mon != NULL && mon->name != NULL && !strcmp(mon->name->str, name)) {
            return i;
        }
    }
    return -1;
}

HSMonitor* find_monitor_by_name(char* name) {
    int i = find_monitor_index_by_name(name);
    if (i == -1) {
        return NULL;
    } else {
        return monitor_with_index(i);
    }
}

int string_to_monitor_index(char* string) {
    if (string[0] == '\0') {
        return g_cur_monitor;
    } else if (string[0] == '-' || string[0] == '+') {
        // relative monitor index
        int idx = g_cur_monitor + atoi(string);
        idx %= g_monitors->len;
        idx += g_monitors->len;
        idx %= g_monitors->len;
        return idx;
    } else if (isdigit(string[0])) {
        // absolute monitor index
        int idx = atoi(string);
        if (idx < 0 || idx >= g_monitors->len) {
            return -1;
        }
        return idx;
    } else {
        // monitor string
        return find_monitor_index_by_name(string);
    }
}

HSMonitor* string_to_monitor(char* string) {
    int idx = string_to_monitor_index(string);
    return monitor_with_index(idx);
}

HSMonitor* add_monitor(XRectangle rect, HSTag* tag, char* name) {
    assert(tag != NULL);
    HSMonitor* m = g_new0(HSMonitor, 1);
    m->rect = rect;
    m->tag = tag;
    m->name = (name ? g_string_new(name) : NULL);
    m->mouse.x = 0;
    m->mouse.y = 0;
    m->dirty = true;
    m->slice = slice_create_monitor(m);
    m->stacking_window = XCreateSimpleWindow(g_display, g_root,
                                             42, 42, 42, 42, 1, 0, 0);
    stack_insert_slice(g_monitor_stack, m->slice);
    g_array_append_val(g_monitors, m);
    return g_array_index(g_monitors, HSMonitor*, g_monitors->len-1);
}

int add_monitor_command(int argc, char** argv, GString* output) {
    // usage: add_monitor RECTANGLE [TAG [NAME]]
    if (argc < 2) {
        return HERBST_NEED_MORE_ARGS;
    }
    XRectangle rect = parse_rectangle(argv[1]);
    HSTag* tag = NULL;
    char* name = NULL;
    if (argc == 2 || !strcmp("", argv[2])) {
        tag = find_unused_tag();
        if (!tag) {
            g_string_append_printf(output,
                "%s: There are not enough free tags\n", argv[0]);
            return HERBST_TAG_IN_USE;
        }
    }
    else {
        tag = find_tag(argv[2]);
        if (!tag) {
            g_string_append_printf(output,
                "%s: The tag \"%s\" does not exist\n", argv[0], argv[2]);
            return HERBST_INVALID_ARGUMENT;
        }
    }
    if (find_monitor_with_tag(tag)) {
        g_string_append_printf(output,
            "%s: The tag \"%s\" is already viewed on a monitor\n", argv[0], argv[2]);
        return HERBST_TAG_IN_USE;
    }
    if (argc > 3) {
        name = argv[3];
        if (isdigit(name[0])) {
            g_string_append_printf(output,
                "%s: The monitor name may not start with a number\n", argv[0]);
            return HERBST_INVALID_ARGUMENT;
        }
        if (!strcmp("", name)) {
            g_string_append_printf(output,
                "%s: An empty monitor name is not permitted\n", argv[0]);
            return HERBST_INVALID_ARGUMENT;
        }
        if (find_monitor_by_name(name)) {
            g_string_append_printf(output,
                "%s: A monitor with the same name already exists\n", argv[0]);
            return HERBST_INVALID_ARGUMENT;
        }
    }
    HSMonitor* monitor = add_monitor(rect, tag, name);
    frame_show_recursive(tag->frame);
    monitor_apply_layout(monitor);
    emit_tag_changed(tag, g_monitors->len - 1);
    return 0;
}

int remove_monitor_command(int argc, char** argv, GString* output) {
    // usage: remove_monitor INDEX
    if (argc < 2) {
        return HERBST_NEED_MORE_ARGS;
    }
    int index = string_to_monitor_index(argv[1]);
    if (index == -1) {
        g_string_append_printf(output,
            "%s: Monitor \"%s\" not found!\n", argv[0], argv[1]);
        return HERBST_INVALID_ARGUMENT;
    }
    int ret = remove_monitor(index);
    if (ret == HERBST_INVALID_ARGUMENT) {
        g_string_append_printf(output,
            "%s: Index needs to be between 0 and %d\n", argv[0], g_monitors->len - 1);
    } else if (ret == HERBST_FORBIDDEN) {
        g_string_append_printf(output,
            "%s: Can't remove the last monitor\n", argv[0]);
    }
    return ret;
}

int remove_monitor(int index) {
    if (index < 0 || index >= g_monitors->len) {
        return HERBST_INVALID_ARGUMENT;
    }
    if (g_monitors->len <= 1) {
        return HERBST_FORBIDDEN;
    }
    HSMonitor* monitor = monitor_with_index(index);
    // adjust selection
    if (g_cur_monitor > index) {
        // same monitor shall be selected after remove
        g_cur_monitor--;
    }
    assert(monitor->tag);
    assert(monitor->tag->frame);
    // hide clients
    frame_hide_recursive(monitor->tag->frame);
    // remove from monitor stack
    stack_remove_slice(g_monitor_stack, monitor->slice);
    slice_destroy(monitor->slice);
    XDestroyWindow(g_display, monitor->stacking_window);
    // and remove monitor completely
    g_free(monitor);
    g_array_remove_index(g_monitors, index);
    if (g_cur_monitor >= g_monitors->len) {
        g_cur_monitor--;
        // if selection has changed, then relayout focused monitor
        monitor_apply_layout(get_current_monitor());
    }
    return 0;
}

int move_monitor_command(int argc, char** argv, GString* output) {
    // usage: move_monitor INDEX RECT [PADUP [PADRIGHT [PADDOWN [PADLEFT]]]]
    // moves monitor with number to RECT
    if (argc < 3) {
        return HERBST_NEED_MORE_ARGS;
    }
    HSMonitor* monitor = string_to_monitor(argv[1]);
    if (monitor == NULL) {
        g_string_append_printf(output,
            "%s: Monitor \"%s\" not found!\n", argv[0], argv[1]);
        return HERBST_INVALID_ARGUMENT;
    }
    XRectangle rect = parse_rectangle(argv[2]);
    if (rect.width < WINDOW_MIN_WIDTH || rect.height < WINDOW_MIN_HEIGHT) {
        g_string_append_printf(output,
            "%s: Rectangle is too small\n", argv[0]);
        return HERBST_INVALID_ARGUMENT;
    }
    // else: just move it:
    monitor->rect = rect;
    if (argc > 3 && argv[3][0] != '\0') monitor->pad_up       = atoi(argv[3]);
    if (argc > 4 && argv[4][0] != '\0') monitor->pad_right    = atoi(argv[4]);
    if (argc > 5 && argv[5][0] != '\0') monitor->pad_down     = atoi(argv[5]);
    if (argc > 6 && argv[6][0] != '\0') monitor->pad_left     = atoi(argv[6]);
    monitor_apply_layout(monitor);
    return 0;
}

int rename_monitor_command(int argc, char** argv, GString* output) {
    if (argc < 3) {
        return HERBST_NEED_MORE_ARGS;
    }
    HSMonitor* mon = string_to_monitor(argv[1]);
    if (mon == NULL) {
        g_string_append_printf(output,
            "%s: Monitor \"%s\" not found!\n", argv[0], argv[1]);
        return HERBST_INVALID_ARGUMENT;
    }
    if (isdigit(argv[2][0])) {
        g_string_append_printf(output,
            "%s: The monitor name may not start with a number\n", argv[0]);
        return HERBST_INVALID_ARGUMENT;
    } else if (!strcmp("", argv[2])) {
        // empty name -> clear name
        if (mon->name != NULL) {
            g_string_free(mon->name, true);
            mon->name = NULL;
        }
        return 0;
    }
    if (find_monitor_by_name(argv[2])) {
        g_string_append_printf(output,
            "%s: A monitor with the same name already exists\n", argv[0]);
        return HERBST_INVALID_ARGUMENT;
    }
    if (mon->name == NULL) {
        // not named before
        GString* name = g_string_new(argv[2]);
        mon->name = name;
    } else {
        // already named
        g_string_assign(mon->name, argv[2]);
    }
    return 0;
}

int monitor_rect_command(int argc, char** argv, GString* output) {
    // usage: monitor_rect [[-p] INDEX]
    char* monitor_str = NULL;
    HSMonitor* m = NULL;
    bool with_pad = false;

    // if monitor is supplied
    if (argc > 1) {
        monitor_str = argv[1];
    }
    // if -p is supplied
    if (argc > 2) {
        monitor_str = argv[2];
        if (!strcmp("-p", argv[1])) {
            with_pad = true;
        } else {
            g_string_append_printf(output,
                "%s: Invalid argument \"%s\"\n", argv[0], argv[1]);
            return HERBST_INVALID_ARGUMENT;
        }
    }
    // if an index is set
    if (monitor_str) {
        m = string_to_monitor(monitor_str);
        if (m == NULL) {
            g_string_append_printf(output,
                "%s: Monitor \"%s\" not found!\n", argv[0], monitor_str);
            return HERBST_INVALID_ARGUMENT;
        }
    } else {
        m = get_current_monitor();
    }
    XRectangle rect = m->rect;
    if (with_pad) {
        rect.x += m->pad_left;
        rect.width -= m->pad_left + m->pad_right;
        rect.y += m->pad_up;
        rect.height -= m->pad_up + m->pad_down;
    }
    g_string_append_printf(output, "%d %d %d %d",
                    rect.x, rect.y, rect.width, rect.height);
    return 0;
}

int monitor_set_pad_command(int argc, char** argv, GString* output) {
    if (argc < 2) {
        return HERBST_NEED_MORE_ARGS;
    }
    HSMonitor* monitor = string_to_monitor(argv[1]);
    if (monitor == NULL) {
        g_string_append_printf(output,
            "%s: Monitor \"%s\" not found!\n", argv[0], argv[1]);
        return HERBST_INVALID_ARGUMENT;
    }
    if (argc > 2 && argv[2][0] != '\0') monitor->pad_up       = atoi(argv[2]);
    if (argc > 3 && argv[3][0] != '\0') monitor->pad_right    = atoi(argv[3]);
    if (argc > 4 && argv[4][0] != '\0') monitor->pad_down     = atoi(argv[4]);
    if (argc > 5 && argv[5][0] != '\0') monitor->pad_left     = atoi(argv[5]);
    monitor_apply_layout(monitor);
    return 0;
}

HSMonitor* find_monitor_with_tag(HSTag* tag) {
    int i;
    for (i = 0; i < g_monitors->len; i++) {
        HSMonitor* m = monitor_with_index(i);
        if (m->tag == tag) {
            return m;
        }
    }
    return NULL;
}

void ensure_monitors_are_available() {
    if (g_monitors->len > 0) {
        // nothing to do
        return;
    }
    // add monitor if necessary
    XRectangle rect = {
        .x = 0, .y = 0,
        .width = DisplayWidth(g_display, DefaultScreen(g_display)),
        .height = DisplayHeight(g_display, DefaultScreen(g_display)),
    };
    ensure_tags_are_available();
    // add monitor with first tag
    HSMonitor* m = add_monitor(rect, g_array_index(g_tags, HSTag*, 0), NULL);
    g_cur_monitor = 0;
    g_cur_frame = m->tag->frame;
}

HSMonitor* monitor_with_frame(HSFrame* frame) {
    // find toplevel Frame
    while (frame->parent) {
        frame = frame->parent;
    }
    HSTag* tag = find_tag_with_toplevel_frame(frame);
    return find_monitor_with_tag(tag);
}

HSMonitor* get_current_monitor() {
    return g_array_index(g_monitors, HSMonitor*, g_cur_monitor);
}

int monitor_count() {
    return g_monitors->len;
}

void all_monitors_apply_layout() {
    for (int i = 0; i < g_monitors->len; i++) {
        HSMonitor* m = monitor_with_index(i);
        monitor_apply_layout(m);
    }
}

int monitor_set_tag(HSMonitor* monitor, HSTag* tag) {
    HSMonitor* other = find_monitor_with_tag(tag);
    if (monitor == other) {
        // nothing to do
        return 0;
    }
    if (monitor->lock_tag) {
        // If the monitor tag is locked, do not change the tag
        if (other != NULL) {
            // but if the tag is already visible, change to the
            // displaying monitor
            monitor_focus_by_index(monitor_index_of(other));
        }
        return 1;
    }
    if (other != NULL) {
        if (*g_swap_monitors_to_get_tag) {
            if (other->lock_tag) {
                // the monitor we want to steal the tag from is
                // locked. focus that monitor instead
                monitor_focus_by_index(monitor_index_of(other));
                return 1;
            }
            // swap tags
            other->tag = monitor->tag;
            monitor->tag = tag;
            // reset focus
            frame_focus_recursive(tag->frame);
            /* TODO: find the best order of restacking and layouting */
            monitor_restack(other);
            monitor_restack(monitor);
            monitor_apply_layout(other);
            monitor_apply_layout(monitor);
            // discard enternotify-events
            XEvent ev;
            XSync(g_display, False);
            while (XCheckMaskEvent(g_display, EnterWindowMask, &ev));
            ewmh_update_current_desktop();
            emit_tag_changed(other->tag, monitor_index_of(other));
            emit_tag_changed(tag, g_cur_monitor);
        }
        return 0;
    }
    HSTag* old_tag = monitor->tag;
    // 1. show new tag
    monitor->tag = tag;
    // first reset focus and arrange windows
    frame_focus_recursive(tag->frame);
    monitor_restack(monitor);
    monitor->lock_frames = true;
    monitor_apply_layout(monitor);
    monitor->lock_frames = false;
    // then show them (should reduce flicker)
    frame_show_recursive(tag->frame);
    if (!monitor->tag->floating) {
        frame_update_frame_window_visibility(monitor->tag->frame);
    }
    // 2. hide old tag
    frame_hide_recursive(old_tag->frame);
    // focus window just has been shown
    // focus again to give input focus
    frame_focus_recursive(tag->frame);
    // discard enternotify-events
    XEvent ev;
    XSync(g_display, False);
    while (XCheckMaskEvent(g_display, EnterWindowMask, &ev));
    ewmh_update_current_desktop();
    emit_tag_changed(tag, g_cur_monitor);
    return 0;
}

int monitor_set_tag_command(int argc, char** argv, GString* output) {
    if (argc < 2) {
        return HERBST_NEED_MORE_ARGS;
    }
    HSMonitor* monitor = get_current_monitor();
    HSTag*  tag = find_tag(argv[1]);
    if (monitor && tag) {
        int ret = monitor_set_tag(monitor, tag);
        if (ret != 0) {
            g_string_append_printf(output,
                "%s: Could not change tag (maybe monitor is locked?)\n", argv[0]);
        }
        return ret;
    } else {
        g_string_append_printf(output,
            "%s: Invalid monitor or tag\n", argv[0]);
        return HERBST_INVALID_ARGUMENT;
    }
}

int monitor_set_tag_by_index_command(int argc, char** argv, GString* output) {
    if (argc < 2) {
        return HERBST_NEED_MORE_ARGS;
    }
    bool skip_visible = false;
    if (argc >= 3 && !strcmp(argv[2], "--skip-visible")) {
        skip_visible = true;
    }
    HSTag* tag = get_tag_by_index_str(argv[1], skip_visible);
    if (!tag) {
        g_string_append_printf(output,
            "%s: Invalid index \"%s\"\n", argv[0], argv[1]);
        return HERBST_INVALID_ARGUMENT;
    }
    int ret = monitor_set_tag(get_current_monitor(), tag);
    if (ret != 0) {
        g_string_append_printf(output,
            "%s: Could not change tag (maybe monitor is locked?)\n", argv[0]);
    }
    return ret;
}

int monitor_focus_command(int argc, char** argv, GString* output) {
    if (argc < 2) {
        return HERBST_NEED_MORE_ARGS;
    }
    int new_selection = string_to_monitor_index(argv[1]);
    if (new_selection == -1) {
        g_string_append_printf(output,
            "%s: Monitor \"%s\" not found!\n", argv[0], argv[1]);
        return HERBST_INVALID_ARGUMENT;
    }
    // really change selection
    monitor_focus_by_index(new_selection);
    return 0;
}

int monitor_cycle_command(int argc, char** argv) {
    int delta = 1;
    int count = g_monitors->len;
    if (argc >= 2) {
        delta = atoi(argv[1]);
    }
    int new_selection = g_cur_monitor + delta;
    // fix range of index
    new_selection %= count;
    new_selection += count;
    new_selection %= count;
    // really change selection
    monitor_focus_by_index(new_selection);
    return 0;
}

int monitor_index_of(HSMonitor* monitor) {
    for (int i = 0; i < g_monitors->len; i++) {
        if (monitor_with_index(i) == monitor) {
            return i;
        }
    }
    return -1;
}

void monitor_focus_by_index(int new_selection) {
    new_selection = CLAMP(new_selection, 0, g_monitors->len - 1);
    HSMonitor* old = get_current_monitor();
    HSMonitor* monitor = monitor_with_index(new_selection);
    if (old == monitor) {
        // nothing to do
        return;
    }
    // change selection globals
    assert(monitor->tag);
    assert(monitor->tag->frame);
    g_cur_monitor = new_selection;
    frame_focus_recursive(monitor->tag->frame);
    // repaint monitors
    monitor_apply_layout(old);
    monitor_apply_layout(monitor);
    int rx, ry;
    {
        // save old mouse position
        Window win, child;
        int wx, wy;
        unsigned int mask;
        if (True == XQueryPointer(g_display, g_root, &win, &child,
            &rx, &ry, &wx, &wy, &mask)) {
            old->mouse.x = rx - old->rect.x;
            old->mouse.y = ry - old->rect.y;
            old->mouse.x = CLAMP(old->mouse.x, 0, old->rect.width-1);
            old->mouse.y = CLAMP(old->mouse.y, 0, old->rect.height-1);
        }
    }
    // restore position of new monitor
    // but only if mouse pointer is not already on new monitor
    int new_x, new_y;
    if ((monitor->rect.x <= rx) && (rx < monitor->rect.x + monitor->rect.width)
        && (monitor->rect.y <= ry) && (ry < monitor->rect.y + monitor->rect.height)) {
        // mouse already is on new monitor
    } else {
        // If the mouse is located in a gap indicated by
        // mouse_recenter_gap at the outer border of the monitor,
        // recenter the mouse.
        if (min(monitor->mouse.x, abs(monitor->mouse.x - monitor->rect.width)) < *g_mouse_recenter_gap
            || min(monitor->mouse.y, abs(monitor->mouse.y - monitor->rect.height)) < *g_mouse_recenter_gap) {
            monitor->mouse.x = monitor->rect.width / 2;
            monitor->mouse.y = monitor->rect.height / 2;
        }
        new_x = monitor->rect.x + monitor->mouse.x;
        new_y = monitor->rect.y + monitor->mouse.y;
        XWarpPointer(g_display, None, g_root, 0, 0, 0, 0, new_x, new_y);
        // discard all mouse events caused by this pointer movage from the
        // event queue, so the focus really stays in the last focused window on
        // this monitor and doesn't jump to the window hovered by the mouse
        XEvent ev;
        XSync(g_display, False);
        while(XCheckMaskEvent(g_display, EnterWindowMask, &ev));
    }
    // emit hooks
    ewmh_update_current_desktop();
    emit_tag_changed(monitor->tag, new_selection);
}

int monitor_get_relative_x(HSMonitor* m, int x_root) {
    return x_root - m->rect.x - m->pad_left;
}

int monitor_get_relative_y(HSMonitor* m, int y_root) {
    return y_root - m->rect.y - m->pad_up;
}

HSMonitor* monitor_with_coordinate(int x, int y) {
    int i;
    for (i = 0; i < g_monitors->len; i++) {
        HSMonitor* m = monitor_with_index(i);
        if (m->rect.x + m->pad_left <= x
            && m->rect.x + m->rect.width - m->pad_right > x
            && m->rect.y + m->pad_up <= y
            && m->rect.y + m->rect.height - m->pad_down > y) {
            return m;
        }
    }
    return NULL;
}

HSMonitor* monitor_with_index(int index) {
    if (index < 0 || index >= g_monitors->len) {
        return NULL;
    }
    return g_array_index(g_monitors, HSMonitor*, index);
}

int monitors_lock_command(int argc, char** argv) {
    // lock-number must never be negative
    // ensure that lock value is valid
    if (*g_monitors_locked < 0) {
        *g_monitors_locked = 0;
    }
    // increase lock => it is definitely > 0, i.e. locked
    (*g_monitors_locked)++;
    monitors_lock_changed();
    return 0;
}

int monitors_unlock_command(int argc, char** argv) {
    // lock-number must never be lower than 1 if unlocking
    // so: ensure that lock value is valid
    if (*g_monitors_locked < 1) {
        *g_monitors_locked = 1;
    }
    // decrease lock => unlock
    (*g_monitors_locked)--;
    monitors_lock_changed();
    return 0;
}

void monitors_lock_changed() {
    if (*g_monitors_locked < 0) {
        *g_monitors_locked = 0;
        HSDebug("fixing invalid monitors_locked value to 0\n");
    }
    if (!*g_monitors_locked) {
        // if not locked anymore, then repaint all the dirty monitors
        for (int i = 0; i < g_monitors->len; i++) {
            HSMonitor* m = monitor_with_index(i);
            if (m->dirty) {
                monitor_apply_layout(m);
            }
        }
    }
}

int monitor_lock_tag_command(int argc, char** argv, GString* output) {
    char* cmd_name = argv[0];
    (void)SHIFT(argc, argv);
    HSMonitor *monitor;
    if (argc >= 1) {
        monitor = string_to_monitor(argv[0]);
        if (monitor == NULL) {
            g_string_append_printf(output,
                "%s: Monitor \"%s\" not found!\n", cmd_name, argv[0]);
            return HERBST_INVALID_ARGUMENT;
        }
    } else {
        monitor = get_current_monitor();
    }
    monitor->lock_tag = true;
    return 0;
}

int monitor_unlock_tag_command(int argc, char** argv, GString* output) {
    char* cmd_name = argv[0];
    (void)SHIFT(argc, argv);
    HSMonitor *monitor;
    if (argc >= 1) {
        monitor = string_to_monitor(argv[0]);
        if (monitor == NULL) {
            g_string_append_printf(output,
                "%s: Monitor \"%s\" not found!\n", cmd_name, argv[0]);
            return HERBST_INVALID_ARGUMENT;
        }
    } else {
        monitor = get_current_monitor();
    }
    monitor->lock_tag = false;
    return 0;
}

// monitor detection using xinerama (if available)
#ifdef XINERAMA
// inspired by dwm's isuniquegeom()
static bool geom_unique(XineramaScreenInfo *unique, size_t n, XineramaScreenInfo *info) {
    while (n--)
        if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org
        &&  unique[n].width == info->width && unique[n].height == info->height)
            return false;
    return true;
}

// inspired by dwm's updategeom()
bool detect_monitors_xinerama(XRectangle** ret_rects, size_t* ret_count) {
    int i, j, n;
    XineramaScreenInfo *info, *unique;
    XRectangle *monitors;

    if (!XineramaIsActive(g_display)) {
        return false;
    }
    info = XineramaQueryScreens(g_display, &n);
    unique = g_new(XineramaScreenInfo, n);
    /* only consider unique geometries as separate screens */
    for (i = 0, j = 0; i < n; i++) {
        if (geom_unique(unique, j, &info[i]))
        {
            memcpy(&unique[j++], &info[i], sizeof(XineramaScreenInfo));
        }
    }
    XFree(info);
    n = j;

    monitors = g_new(XRectangle, n);
    for (i = 0; i < n; i++) {
        monitors[i].x = unique[i].x_org;
        monitors[i].y = unique[i].y_org;
        monitors[i].width = unique[i].width;
        monitors[i].height = unique[i].height;
    }
    *ret_count = n;
    *ret_rects = monitors;
    g_free(unique);
    return true;
}
#else  /* XINERAMA */

bool detect_monitors_xinerama(XRectangle** ret_rects, size_t* ret_count) {
    return false;
}

#endif /* XINERAMA */

// monitor detection that always works: one monitor across the entire screen
bool detect_monitors_simple(XRectangle** ret_rects, size_t* ret_count) {
    *ret_count = 1;
    *ret_rects = g_new0(XRectangle, 1);
    (*ret_rects)->x = 0;
    (*ret_rects)->y = 0;
    (*ret_rects)->width = g_screen_width;
    (*ret_rects)->height = g_screen_height;
    return true;
}

int detect_monitors_command(int argc, char **argv, GString* output) {
    MonitorDetection detect[] = {
        detect_monitors_xinerama,
        detect_monitors_simple,
    };
    XRectangle* monitors = NULL;
    size_t count = 0;
    // search for a working monitor detection
    // at least the simple detection must work
    for (int i = 0; i < LENGTH(detect); i++) {
        if (detect[i](&monitors, &count)) {
            break;
        }
    }
    assert(count && monitors);

    // apply it
    int ret = set_monitor_rects(monitors, count);
    g_free(monitors);
    if (ret == HERBST_TAG_IN_USE && output != NULL) {
        g_string_append_printf(output,
            "%s: There are not enough free tags\n", argv[0]);
    }
    return ret;
}

int monitor_stack_window_count(bool only_clients) {
    return stack_window_count(g_monitor_stack, only_clients);
}

void monitor_stack_to_window_buf(Window* buf, int len, bool only_clients,
                                 int* remain_len) {
    stack_to_window_buf(g_monitor_stack, buf, len, only_clients, remain_len);
}

HSStack* get_monitor_stack() {
    return g_monitor_stack;
}

int monitor_raise_command(int argc, char** argv, GString* output) {
    char* cmd_name = argv[0];
    (void)SHIFT(argc, argv);
    HSMonitor* monitor;
    if (argc >= 1) {
        monitor = string_to_monitor(argv[0]);
        if (monitor == NULL) {
            g_string_append_printf(output,
                "%s: Monitor \"%s\" not found!\n", cmd_name, argv[0]);
            return HERBST_INVALID_ARGUMENT;
        }
    } else {
        monitor = get_current_monitor();
    }
    stack_raise_slide(g_monitor_stack, monitor->slice);
    return 0;
}

void monitor_restack(HSMonitor* monitor) {
    int count = 1 + stack_window_count(monitor->tag->stack, false);
    Window* buf = g_new(Window, count);
    buf[0] = monitor->stacking_window;
    stack_to_window_buf(monitor->tag->stack, buf + 1, count - 1, false, NULL);
    /* remove a focused fullscreen client */
    Window win = frame_focused_window(monitor->tag->frame);
    HSClient* client = win ? get_client_from_window(win) : NULL;
    if (client && client->fullscreen) {
        XRaiseWindow(g_display, client->window);
        int idx = array_find(buf, count, sizeof(*buf), &client->window);
        assert(idx >= 0);
        count--;
        memmove(buf + idx, buf + idx + 1, sizeof(*buf) * (count - idx));
    }
    XRestackWindows(g_display, buf, count);
    g_free(buf);
}

int shift_to_monitor(int argc, char** argv, GString* output) {
    if (argc <= 1) {
        return HERBST_NEED_MORE_ARGS;
    }
    char* monitor_str = argv[1];
    HSMonitor* monitor = string_to_monitor(monitor_str);
    if (!monitor) {
        g_string_append_printf(output,
            "%s: Invalid monitor\n", monitor_str);
        return HERBST_INVALID_ARGUMENT;
    }
    tag_move_focused_client(monitor->tag);
    return 0;
}

