#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <syscalls.h>
#include <keyboard.h>
#include <hpet.h>
#include <crime/crime.h>
#include <cabinet.h>

#define WINDOW_W        600
#define WINDOW_H        420
#define PAD             6
#define TOOL_H          32
#define BUTTON_W        28
#define BUTTON_H        26
#define BUTTON_GAP      2
#define GROUP_GAP       10      // the groove between groups of buttons sits in this
#define VIEW_W          120
#define VIEW_H          24
#define HISTORY         32
#define ADDRESS_H       24
#define STATUS_H        22
#define STATUS_RIGHT_W  150
#define START_DIR       "/"
#define HOME_POLL_MS    2000    // how often my computer looks at the drives again
#define FOLDER_POLL_MS  1500
#define RUN_FILES_MAX   14
#define NEW_FOLDER      "New Folder"
#define NEW_TEXT        "New Text Document"

#define KEY_ENTER       0x1C
#define KEY_BACK        0x0E
#define KEY_ESCAPE      0x01
#define KEY_A           0x1E
#define KEY_C           0x2E
#define KEY_N           0x31
#define KEY_V           0x2F
#define KEY_X           0x2D

enum {
    BTN_BACK,
    BTN_FORWARD,
    BTN_UP,
    BTN_REFRESH,
    BTN_NEW,
    BTN_DELETE,
    BTN_COUNT,
};

// what the status bar says with the pointer on a button
static const char *btn_hint[BTN_COUNT] = { "Back", "Forward", "Up One Level", "Refresh", "New Folder", "Delete" };
static const int   btn_glyph[BTN_COUNT] = { GLYPH_BACK, GLYPH_FORWARD, GLYPH_UP, GLYPH_REFRESH, GLYPH_NEW_FOLDER, GLYPH_DELETE };

// what the right click menu items do
enum {
    ACT_OPEN,
    ACT_TERMINAL,
    ACT_TERMINAL_IN,
    ACT_RENAME,
    ACT_DELETE,
    ACT_NEW_FOLDER,
    ACT_NEW_TEXT,
    ACT_REFRESH,
    ACT_HIDDEN,
    ACT_CUT,
    ACT_COPY,
    ACT_PASTE,
    ACT_PASTE_IN,
    ACT_SELECT_ALL,
    ACT_ARRANGE,
    ACT_PROPERTIES,
    ACT_LINE,
};

enum {
    GO_NEW,
    GO_BACK,
    GO_FORWARD,
};

static cs_window_t  *win;
static ui_button_t   buttons[BTN_COUNT];
static ui_dropdown_t view_list = { .items = { "Large Icons", "Details" }, .count = 2 };
static ui_rect_t     address_r, view_r, status_r;
static int           grooves[2];
static uint32_t      mouse_was;
static int           quit;
static int           at_home;
static uint8_t       keymod;

static ui_menu_t     menu;
static int           menu_acts[UI_MENU_ITEMS];

static char          back[HISTORY][CAB_PATH_MAX];
static char          forward[HISTORY][CAB_PATH_MAX];
static int           back_count, forward_count;

// typing a path into the address bar
static int          typing;
static char         typed[CAB_PATH_MAX];

static int          hpet_fd = -2;

uint64_t now_ms(void) {
    static uint64_t fallback;
    uint64_t        fs = 0;

    if (hpet_fd == -2)
        hpet_fd = _open("/dev/hpet", O_RDONLY);
    if (hpet_fd >= 0 && _ioctl(hpet_fd, HPET_IOCTL_GET_FEMTOSECONDS, &fs) == 0 && fs)
        return fs / femtosecondsPerMillisecond;
    return fallback += 10;
}

static const char *here(void) {
    return at_home ? HOME_PATH : view_path();
}

static ui_canvas_t canvas(void) {
    return ui_canvas(win->pixels, win->width, win->height, win->pitch);
}

static void layout(void) {
    int w = win->width, h = win->height;
    int x = PAD;

    for (int i = 0, g = 0; i < BTN_COUNT; i++) {
        if (i == BTN_NEW) {
            grooves[g++] = x + GROUP_GAP / 2 - 2;
            x += GROUP_GAP;
        }
        buttons[i].rect = (ui_rect_t){ x, (TOOL_H - BUTTON_H) / 2, BUTTON_W, BUTTON_H };
        x += BUTTON_W + BUTTON_GAP;
    }
    grooves[1] = x + GROUP_GAP / 2 - 2;
    view_list.rect = (ui_rect_t){ x + GROUP_GAP, (TOOL_H - VIEW_H) / 2, VIEW_W, VIEW_H };

    int ax    = PAD + ui_text_width(&ui_font_text, "Address") + 8;
    address_r = (ui_rect_t){ ax, TOOL_H + 4, w - ax - PAD, ADDRESS_H };
    status_r  = (ui_rect_t){ 0, h - STATUS_H, w, STATUS_H };

    int vy = address_r.y + ADDRESS_H + 6;
    view_r = (ui_rect_t){ PAD, vy, w - PAD * 2, status_r.y - 4 - vy };
    if (view_r.h < 0)
        view_r.h = 0;
    view_layout(view_r);
    home_layout(view_r);
}

void size_text(uint64_t size, char *out, size_t len) {
    if (size < 1024)
        snprintf(out, len, "%lu bytes", (unsigned long)size);
    else if (size < 1024 * 1024)
        snprintf(out, len, "%lu.%02luKB", (unsigned long)(size / 1024), (unsigned long)(size % 1024 * 100 / 1024));
    else if (size < 1024ul * 1024 * 1024)
        snprintf(out, len, "%lu.%02luMB", (unsigned long)(size >> 20),
                 (unsigned long)((size & 0xFFFFF) * 100 >> 20));
    else
        snprintf(out, len, "%lu.%02luGB", (unsigned long)(size >> 30),
                 (unsigned long)((size & 0x3FFFFFFF) * 100 >> 30));
}

static int drop_op(const char *from, const char *to, uint8_t mods) {
    if (mods & KEYMOD_CTRL)
        return OP_COPY;
    if (mods & KEYMOD_SHIFT)
        return OP_MOVE;
    return ops_same_drive(from, to) ? OP_MOVE : OP_COPY;
}

static void status_draw(ui_canvas_t *c) {
    char left[64], right[48] = "";
    int  i    = view_picked();
    int  hint = -1;
    for (int b = 0; b < BTN_COUNT; b++) {
        if (buttons[b].hover)
            hint = b;
    }

    int chosen = at_home ? 0 : view_chosen_count();
    int target = at_home ? -1 : view_drop_target();

    if (hint >= 0) {
        snprintf(left, sizeof(left), "%s", btn_hint[hint]);
    } else if (!at_home && view_dragging() && target >= 0) {
        const entry_t *t = view_entry(target);
        char           to[CAB_PATH_MAX], name[40];
        view_entry_path(target, to, sizeof(to));
        ui_text_ascii(name, sizeof(name), t->name);
        if (t->kind == KIND_PROGRAM)
            snprintf(left, sizeof(left), "Open with '%s'", name);
        else
            snprintf(left, sizeof(left), "%s to '%s'", drop_op(view_path(), to, keymod) == OP_MOVE ? "Move" : "Copy",
                     name);
    } else if (at_home) {
        int h = home_picked();
        snprintf(right, sizeof(right), HOME_PATH);
        if (home_is_drive(h)) {
            uint64_t total, free;
            char     f[24], t[24];
            home_space(h, &total, &free);
            size_text(free, f, sizeof(f));
            size_text(total, t, sizeof(t));
            snprintf(left, sizeof(left), "Free Space: %s, Capacity: %s", f, t);
        } else if (h >= 0) {
            snprintf(left, sizeof(left), "1 object(s) selected");
        } else {
            snprintf(left, sizeof(left), "%d object(s)", home_count());
        }
    } else if (chosen) {
        uint64_t total = 0;
        for (int j = 0; j < view_count(); j++) {
            if (view_entry(j)->chosen)
                total += view_entry(j)->size;
        }
        snprintf(left, sizeof(left), "%d object(s) selected", chosen);
        if (chosen > 1 || (i >= 0 && view_entry(i)->kind != KIND_FOLDER && view_entry(i)->kind != KIND_DEVICE))
            size_text(total, right, sizeof(right));
    } else {
        uint64_t total = 0;
        for (int j = 0; j < view_count(); j++)
            total += view_entry(j)->size;
        snprintf(left, sizeof(left), "%d object(s)", view_count());
        size_text(total, right, sizeof(right));
    }

    ui_fill(c, status_r, UI_COLOUR_FACE);
    ui_rect_t l = { 2, status_r.y + 2, status_r.w - STATUS_RIGHT_W - 6, STATUS_H - 4 };
    ui_rect_t r = { status_r.w - STATUS_RIGHT_W - 2, status_r.y + 2, STATUS_RIGHT_W, STATUS_H - 4 };
    ui_edge(c, l, UI_COLOUR_SHADOW, UI_COLOUR_HILIGHT);
    ui_edge(c, r, UI_COLOUR_SHADOW, UI_COLOUR_HILIGHT);
    ui_text_in(c, &ui_font_text, (ui_rect_t){ l.x + 4, l.y, l.w - 8, l.h }, left, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
    ui_text_in(c, &ui_font_text, (ui_rect_t){ r.x + 4, r.y, r.w - 8, r.h }, right, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
}

static void address_draw(ui_canvas_t *c) {
    ui_text_in(c, &ui_font_text, (ui_rect_t){ PAD, address_r.y, address_r.x - PAD, ADDRESS_H }, "Address",
               UI_COLOUR_TEXT, UI_ALIGN_LEFT);
    ui_panel(c, address_r, UI_SUNKEN);

    char text[CAB_PATH_MAX];
    ui_text_ascii(text, sizeof(text), typing ? typed : here());

    ui_rect_t inside = ui_rect_shrink(address_r, 2);
    ui_rect_t old    = c->clip;
    ui_clip(c, ui_rect_clip(inside, old));

    int tw = ui_text_width(&ui_font_text, text);
    int tx = inside.x + 4;
    if (typing && tx + tw + 2 > inside.x + inside.w)
        tx = inside.x + inside.w - tw - 3;
    ui_text_in(c, &ui_font_text, (ui_rect_t){ tx, inside.y, tw + 1, inside.h }, text, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
    if (typing)
        ui_fill(c, (ui_rect_t){ tx + tw + 1, inside.y + 3, 1, inside.h - 6 }, UI_COLOUR_TEXT);
    c->clip = old;
}

static void draw_all(void) {
    ui_canvas_t c = canvas();
    ui_fill(&c, (ui_rect_t){ 0, 0, win->width, win->height }, UI_COLOUR_FACE);

    buttons[BTN_BACK].disabled    = !back_count;
    buttons[BTN_FORWARD].disabled = !forward_count;
    buttons[BTN_UP].disabled      = at_home;
    buttons[BTN_NEW].disabled     = at_home;
    buttons[BTN_DELETE].disabled  = at_home || !view_chosen_count();
    for (int i = 0; i < BTN_COUNT; i++)
        ui_button_draw(&c, &buttons[i]);
    for (int g = 0; g < 2; g++) {
        ui_fill(&c, (ui_rect_t){ grooves[g], 4, 1, TOOL_H - 8 }, UI_COLOUR_SHADOW);
        ui_fill(&c, (ui_rect_t){ grooves[g] + 1, 4, 1, TOOL_H - 8 }, UI_COLOUR_HILIGHT);
    }
    ui_separator(&c, 0, TOOL_H, win->width);

    address_draw(&c);
    if (at_home)
        home_draw(&c);
    else
        view_draw(&c);
    status_draw(&c);

    // these go over everything else
    ui_dropdown_draw(&c, &view_list);
    ui_menu_draw(&c, &menu);
    cs_window_commit(win, 0, 0, win->width, win->height);
}

static void title_update(void) {
    const char *p    = here();
    const char *base = strrchr(p, '/');
    char        title[CS_TITLE_MAX];

    ui_text_ascii(title, sizeof(title), base && base[1] ? base + 1 : p);
    cs_window_title(win, title);
}

static void push(char (*stack)[CAB_PATH_MAX], int *count, const char *p) {
    if (*count == HISTORY) {
        memmove(stack[0], stack[1], (HISTORY - 1) * CAB_PATH_MAX);
        (*count)--;
    }
    strcpy(stack[(*count)++], p);
}

static int go_to(const char *p, int how) {
    char from[CAB_PATH_MAX];
    strcpy(from, here());

    if (!strcasecmp(p, HOME_PATH)) {
        home_open();
        at_home = 1;
    } else if (view_open(p) == 0) {
        at_home = 0;
    } else {
        char text[CAB_PATH_MAX + 64];
        snprintf(text, sizeof(text), "Cabinet cant open '%s'.\nMake sure the folder is there and try again.", p);
        ui_message("Cabinet", text, UI_ICON_ERROR);
        return -1;
    }
    if (strcmp(from, here())) {
        if (how == GO_BACK) {
            push(forward, &forward_count, from);
        } else {
            push(back, &back_count, from);
            if (how == GO_NEW)
                forward_count = 0;
        }
    }
    title_update();
    return 0;
}

static int go(const char *p) {
    return go_to(p, GO_NEW);
}

// the one we came out of gets picked, so its easy to see where we were
static void go_history(int how) {
    char (*stack)[CAB_PATH_MAX] = how == GO_BACK ? back : forward;
    int  *count                 = how == GO_BACK ? &back_count : &forward_count;
    if (!*count)
        return;

    char from[CAB_PATH_MAX], to[CAB_PATH_MAX];
    strcpy(from, here());
    strcpy(to, stack[--(*count)]);
    if (go_to(to, how) < 0)
        return;
    if (at_home) {
        home_pick_path(from);
        return;
    }

    size_t len = strlen(to);
    if (!strncmp(from, to, len) && (from[len] == '/' || len == 1)) {
        char        child[CAB_NAME_MAX];
        const char *rest  = from + len + (len > 1);
        const char *slash = strchr(rest, '/');
        size_t      n     = slash ? (size_t)(slash - rest) : strlen(rest);
        memcpy(child, rest, n);
        child[n] = '\0';
        view_pick_name(child);
    }
}

// the folder above, with the one we came out of picked. above / is my computer
static void go_up(void) {
    if (at_home)
        return;

    char p[CAB_PATH_MAX];
    strcpy(p, view_path());
    if (!strcmp(p, "/")) {
        if (go(HOME_PATH) == 0)
            home_pick_path(p);
        return;
    }

    char  child[CAB_NAME_MAX];
    char *slash = strrchr(p, '/');
    strcpy(child, slash + 1);
    if (slash == p)
        p[1] = '\0';
    else
        *slash = '\0';

    if (go(p) == 0)
        view_pick_name(child);
}

static int exists(const char *p) {
    int fd = _open(p, O_RDONLY);
    if (fd < 0)
        return 0;
    _close(fd);
    return 1;
}

static void run_with(const char *program, char **files, int n) {
    const char *argv[RUN_FILES_MAX + 2] = { program };
    for (int i = 0; i < n && i < RUN_FILES_MAX; i++)
        argv[i + 1] = files[i];
    if (!exists(program) || cs_run(argv) < 0) {
        char text[CAB_PATH_MAX + 64];
        snprintf(text, sizeof(text), "Cabinet couldnt start '%s'.", program);
        ui_message("Cabinet", text, UI_ICON_ERROR);
    }
}

static void run(const char *program, const char *file) {
    char *files[] = { (char *)file };
    run_with(program, files, file ? 1 : 0);
}

static void open_picked(void) {
    if (at_home) {
        if (home_picked() >= 0)
            go(home_path(home_picked()));
        return;
    }

    int i = view_picked();
    if (i < 0)
        return;

    const entry_t *e = view_entry(i);
    char           full[CAB_PATH_MAX];
    view_entry_path(i, full, sizeof(full));

    switch (e->kind) {
    case KIND_FOLDER:  go(full); return;
    case KIND_PROGRAM: run(full, NULL); return;
    case KIND_AUDIO:   run(PROGRAM_PLAYER, full); return;
    case KIND_TEXT:    run(PROGRAM_EDITOR, full); return;
    case KIND_IMAGE:   run(PROGRAM_VIEWER, full); return;
    case KIND_DEVICE:
        ui_message("Cabinet", "This is a device, theres nothing in it Cabinet can open.", UI_ICON_INFO);
        return;
    }

    char text[CAB_NAME_MAX + 96];
    snprintf(text, sizeof(text), "Theres nothing on this computer that opens '%s'.", e->name);
    ui_message("Open With", text, UI_ICON_INFO);
}

// the window may have been resized while a job had it, and the folder has changed
static void after_job(void) {
    layout();
    if (at_home)
        home_open();
    else
        view_refresh();
}

static void delete_chosen(void) {
    int    n;
    char **paths = view_chosen_paths(&n);
    if (!n) {
        list_free(paths, n);
        return;
    }

    const entry_t *e = NULL;
    for (int i = 0; i < view_count() && !e; i++) {
        if (view_entry(i)->chosen)
            e = view_entry(i);
    }

    char        text[CAB_NAME_MAX + 96];
    const char *title = "Confirm Multiple File Delete";
    if (n > 1)
        snprintf(text, sizeof(text), "Are you sure you want to delete these %d items?", n);
    else if (e->kind == KIND_FOLDER)
        snprintf(text, sizeof(text), "Are you sure you want to remove the folder '%s' and all its contents?", e->name);
    else
        snprintf(text, sizeof(text), "Are you sure you want to delete '%s'?", e->name);
    if (n == 1)
        title = e->kind == KIND_FOLDER ? "Confirm Folder Delete" : "Confirm File Delete";

    if (ui_ask(title, text))
        ops_delete(paths, n);
    list_free(paths, n);
    after_job();
}

// everything that was made in dir is what gets chosen, if were looking at it
static void transfer_into(char **paths, int n, const char *dir, int op) {
    int made = ops_transfer(paths, n, dir, op);
    after_job();
    if (at_home || strcmp(dir, view_path()) || !made)
        return;
    view_pick(-1);
    for (int i = 0; i < made; i++)
        view_choose_name(ops_made(i));
}

static int clip_full(void) {
    return exists(CLIP_FILE);
}

static void clip_chosen(int cut) {
    int    n;
    char **paths = view_chosen_paths(&n);
    if (n) {
        clip_set(paths, n, cut);
        view_mark_cut();
    }
    list_free(paths, n);
}

static void paste_into(const char *dir) {
    int    n, cut;
    char **paths = clip_get(&n, &cut);
    if (n) {
        transfer_into(paths, n, dir, cut ? OP_MOVE : OP_COPY);
        if (cut) {
            clip_clear();
            if (!at_home)
                view_mark_cut();
        }
    }
    list_free(paths, n);
}

// dragged onto a folder of ours, or a program which gets them to open
static void drop_on(char **paths, int n, const char *to, int kind, uint8_t mods) {
    if (kind == KIND_PROGRAM)
        run_with(to, paths, n);
    else
        transfer_into(paths, n, to, drop_op(paths[0], to, mods));
}

static void drop_file(char *out, size_t size) {
    snprintf(out, size, DROP_FILE "-%lu", (unsigned long)_getpid());
}

static void dropped_in(void) {
    int i = view_drop_target();
    if (i < 0)
        return;

    int    n;
    char **paths = view_chosen_paths(&n);
    char   to[CAB_PATH_MAX];
    view_entry_path(i, to, sizeof(to));
    if (n)
        drop_on(paths, n, to, view_entry(i)->kind, keymod);
    list_free(paths, n);
}

// let go off our window, the desktop hands it to whatevers there
static void dropped_out(void) {
    int x, y;
    view_drop_point(&x, &y);
    if (x >= 0 && y >= 0 && x < win->width && y < win->height)
        return;

    int    n;
    char **paths = view_chosen_paths(&n);
    char   file[64];
    drop_file(file, sizeof(file));
    if (n && list_write(file, DROP_HEAD, paths, n) == 0)
        cs_drop(win, x, y, keymod, file);
    list_free(paths, n);
}

static void on_drop(const cs_event_t *ev) {
    char file[CS_TEXT_MAX];
    memcpy(file, ev->text, sizeof(file));
    file[sizeof(file) - 1] = '\0';
    if (strncmp(file, DROP_FILE "-", strlen(DROP_FILE) + 1))
        return;

    char   head[16] = "";
    int    n;
    char **paths = list_read(file, head, sizeof(head), &n);
    if (!n || strcmp(head, DROP_HEAD)) {
        list_free(paths, n);
        return;
    }

    char to[CAB_PATH_MAX];
    int  kind = KIND_FOLDER;
    if (at_home) {
        int i = home_at(ev->x, ev->y);
        if (i < 0) {
            list_free(paths, n);
            return;
        }
        snprintf(to, sizeof(to), "%s", home_path(i));
    } else {
        int            i = view_at(ev->x, ev->y);
        const entry_t *e = view_entry(i);
        if (e && (e->kind == KIND_FOLDER || e->kind == KIND_PROGRAM)) {
            view_entry_path(i, to, sizeof(to));
            kind = e->kind;
        } else {
            snprintf(to, sizeof(to), "%s", view_path());
        }
    }
    drop_on(paths, n, to, kind, ev->keymod);
    list_free(paths, n);
}

// ui_text_ascii drops the line breaks, so it goes a line at a time
static void ascii_lines(char *out, size_t size, const char *in) {
    size_t at = 0;
    while (*in && at + 1 < size) {
        const char *end = strchr(in, '\n');
        size_t      len = end ? (size_t)(end - in) : strlen(in);
        char        line[CAB_PATH_MAX + 64];
        snprintf(line, sizeof(line), "%.*s", (int)len, in);
        ui_text_ascii(out + at, size - at, line);
        at += strlen(out + at);
        if (!end)
            break;
        if (at + 1 < size)
            out[at++] = '\n';
        out[at] = '\0';
        in = end + 1;
    }
}

static void count_text(char *out, size_t size, int files, int folders) {
    snprintf(out, size, "%d File%s, %d Folder%s", files, files == 1 ? "" : "s", folders, folders == 1 ? "" : "s");
}

static void properties(void) {
    char text[CAB_PATH_MAX * 2 + 256], title[CAB_NAME_MAX + 16], size[32], bytes[32], parts[64];

    if (at_home) {
        int h = home_picked();
        if (!home_is_drive(h))
            return;
        uint64_t total, free;
        char     f[32], t[32], u[32];
        home_space(h, &total, &free);
        size_text(total - free, u, sizeof(u));
        size_text(free, f, sizeof(f));
        size_text(total, t, sizeof(t));
        snprintf(text, sizeof(text), "Location: %s\n\nUsed space: %s\nFree space: %s\nCapacity: %s", home_path(h), u, f,
                 t);
        snprintf(title, sizeof(title), "Properties");
    } else {
        int    n;
        char **paths = view_chosen_paths(&n);
        int    files = 0, folders = 0;
        uint64_t total = 0;
        for (int i = 0; i < n; i++)
            total += ops_measure(paths[i], &files, &folders);
        if (!n)
            total = ops_measure(view_path(), &files, &folders);
        size_text(total, size, sizeof(size));
        bytes[0] = '\0';
        if (total >= 1024)
            snprintf(bytes, sizeof(bytes), " (%lu bytes)", (unsigned long)total);

        const entry_t *e = view_picked() >= 0 && n == 1 ? view_entry(view_picked()) : NULL;
        if (n > 1) {
            count_text(parts, sizeof(parts), files, folders);
            snprintf(title, sizeof(title), "Properties");
            snprintf(text, sizeof(text), "%d items selected\nLocation: %s\n\nSize: %s%s\nContains: %s", n,
                     view_path(), size, bytes, parts);
        } else if (e) {
            char type[48];
            kind_name(e, type, sizeof(type));
            snprintf(title, sizeof(title), "%s Properties", e->name);
            snprintf(text, sizeof(text), "Name: %s\nType: %s\nLocation: %s\n\nSize: %s%s", e->name, type,
                     view_path(), size, bytes);
            if (e->kind == KIND_FOLDER) {
                count_text(parts, sizeof(parts), files, folders - 1);
                snprintf(text + strlen(text), sizeof(text) - strlen(text), "\nContains: %s", parts);
            }
        } else {
            const char *p    = view_path();
            const char *base = strrchr(p, '/');
            count_text(parts, sizeof(parts), files, folders ? folders - 1 : 0);
            snprintf(title, sizeof(title), "%s Properties", base && base[1] ? base + 1 : p);
            snprintf(text, sizeof(text), "Type: File Folder\nLocation: %s\n\nSize: %s%s\nContains: %s", p,
                     size, bytes, parts);
        }
        list_free(paths, n);
    }

    char shown[sizeof(text)], named[sizeof(title)];
    ascii_lines(shown, sizeof(shown), text);
    ui_text_ascii(named, sizeof(named), title);
    ui_msgbox_t box = {
        .title   = named,
        .text    = shown,
        .buttons = { "OK" },
        .width   = 360,
        .align   = UI_ALIGN_LEFT,
    };
    ui_msgbox(&box);
    layout();
}

static void here_path(const char *name, char *out, size_t size) {
    snprintf(out, size, "%s%s%s", view_path(), strcmp(view_path(), "/") ? "/" : "", name);
}

static int name_taken(const char *name);

// an empty file, named straight away like a new folder
static void new_text(void) {
    char name[CAB_NAME_MAX] = NEW_TEXT ".txt";
    for (int n = 2; name_taken(name); n++)
        snprintf(name, sizeof(name), NEW_TEXT " (%d).txt", n);

    char full[CAB_PATH_MAX];
    here_path(name, full, sizeof(full));
    int fd = _open(full, O_CREAT | O_WRONLY);
    if (fd < 0) {
        ui_message("Cabinet", "Unable to create the file.\nThis disk may be read only.", UI_ICON_ERROR);
        return;
    }
    _close(fd);
    view_refresh();
    view_pick_name(name);
    view_rename_start();
}

static void terminal_at(const char *p) {
    if (cs_terminal_open(p) < 0)
        ui_message("Cabinet", "Cabinet couldnt open a terminal.", UI_ICON_ERROR);
}

static int name_taken(const char *name) {
    for (int i = 0; i < view_count(); i++) {
        if (!strcmp(view_entry(i)->name, name))
            return 1;
    }
    return 0;
}

// made with the next free name and straight into naming it
static void new_folder(void) {
    char name[CAB_NAME_MAX] = NEW_FOLDER;
    for (int n = 2; name_taken(name); n++)
        snprintf(name, sizeof(name), NEW_FOLDER " (%d)", n);

    char full[CAB_PATH_MAX];
    here_path(name, full, sizeof(full));
    if (_mkdir(full, 0755) < 0) {
        ui_message("Cabinet", "Unable to create the folder.\nThis disk may be read only.", UI_ICON_ERROR);
        return;
    }
    view_refresh();
    view_pick_name(name);
    view_rename_start();
}

static void rename_done(void) {
    int i = view_picked();
    if (i < 0) {
        view_rename_end();
        return;
    }

    const entry_t *e = view_entry(i);
    char           name[CAB_NAME_MAX];
    strcpy(name, view_rename_name());
    view_rename_end();
    if (!name[0] || !strcmp(name, e->name))
        return;

    char text[CAB_NAME_MAX + 256];
    if (name_taken(name)) {
        snprintf(text, sizeof(text), "Cannot rename '%s': a file with the name you specified already exists.", e->name);
        ui_message("Error Renaming File", text, UI_ICON_ERROR);
        return;
    }

    if (cs_name_hidden(name) && !cs_name_hidden(e->name)) {
        snprintf(text, sizeof(text),
                 "'%s' is a hidden name, so it will be hidden from view%s.\n\n"
                 "Names starting with a dot, and a few others like lost+found, are left out. "
                 "Right click and pick Show Hidden Files to see them.\n\nUse this name anyway?",
                 name, view_hidden_shown() ? " once Show Hidden Files is off" : "");
        if (!ui_ask("Hidden Name", text))
            return;
    }

    char from[CAB_PATH_MAX], to[CAB_PATH_MAX];
    view_entry_path(i, from, sizeof(from));
    snprintf(to, sizeof(to), "%s%s%s", view_path(), strcmp(view_path(), "/") ? "/" : "", name);
    if (_rename(from, to) < 0) {
        snprintf(text, sizeof(text), "Cannot rename '%s'.\nIt may be read only or in use.", e->name);
        ui_message("Error Renaming File", text, UI_ICON_ERROR);
        return;
    }

    // it stays in the cell it was in under its new name
    view_keep_spot(to, e->col, e->row);
    view_refresh();
    view_pick_name(name);
}

static void handle(int r) {
    if (r & VIEW_RENAMED)
        rename_done();
    if (r & VIEW_OPEN)
        open_picked();
    if (r & VIEW_UP)
        go_up();
    if (r & VIEW_DELETE)
        delete_chosen();
    if (r & VIEW_DROP)
        dropped_in();
    if (r & VIEW_DROP_OUT)
        dropped_out();
    if (r)
        draw_all();
}

static void refresh(void) {
    if (at_home)
        home_open();
    else
        view_refresh();
}

static void button_clicked(int i) {
    switch (i) {
    case BTN_BACK:    go_history(GO_BACK); break;
    case BTN_FORWARD: go_history(GO_FORWARD); break;
    case BTN_UP:      go_up(); break;
    case BTN_REFRESH: refresh(); break;
    case BTN_NEW:     new_folder(); break;
    case BTN_DELETE:  delete_chosen(); break;
    }
}

static void menu_add(const char *label, int act) {
    menu.items[menu.count]    = act == ACT_LINE ? UI_MENU_LINE : label;
    menu.disabled[menu.count] = 0;
    menu_acts[menu.count++]   = act;
}

// the last one added
static void menu_grey(int off) {
    menu.disabled[menu.count - 1] = (uint8_t)off;
}

static void home_menu(int x, int y) {
    int hit = home_at(x, y);
    if (hit == -2)
        return;
    home_pick(hit);

    menu.count = 0;
    if (hit >= 0) {
        menu_add("Open", ACT_OPEN);
        menu_add("Open Terminal Here", ACT_TERMINAL_IN);
        menu_add(NULL, ACT_LINE);
        menu_add("Paste", ACT_PASTE_IN);
        menu_grey(!clip_full());
        if (home_is_drive(hit)) {
            menu_add(NULL, ACT_LINE);
            menu_add("Properties", ACT_PROPERTIES);
        }
    } else {
        menu_add("Refresh", ACT_REFRESH);
    }
    ui_menu_open(&menu, x, y, (ui_rect_t){ 0, 0, win->width, win->height });
}

// on something its what you can do to it, on empty space its what you can make here
static void menu_show(int x, int y) {
    if (at_home) {
        home_menu(x, y);
        return;
    }

    // on one thats already chosen the rest stay chosen with it
    int hit = view_at(x, y);
    if (hit == -2)
        return;
    if (hit < 0 || !view_entry(hit)->chosen)
        view_pick(hit);
    else
        view_focus(hit);

    menu.count = 0;
    if (hit >= 0) {
        const entry_t *e = view_entry(hit);
        menu_add("Open", ACT_OPEN);
        menu_grey(e->kind == KIND_DEVICE || e->kind == KIND_FILE);
        if (e->kind == KIND_FOLDER)
            menu_add("Open Terminal Here", ACT_TERMINAL_IN);
        menu_add(NULL, ACT_LINE);
        menu_add("Cut", ACT_CUT);
        menu_add("Copy", ACT_COPY);
        if (e->kind == KIND_FOLDER) {
            menu_add("Paste", ACT_PASTE_IN);
            menu_grey(!clip_full() || view_chosen_count() > 1);
        }
        menu_add(NULL, ACT_LINE);
        menu_add("Rename", ACT_RENAME);
        menu_grey(view_chosen_count() > 1);
        menu_add("Delete", ACT_DELETE);
        menu_add(NULL, ACT_LINE);
        menu_add("Properties", ACT_PROPERTIES);
    } else {
        menu_add("New Folder", ACT_NEW_FOLDER);
        menu_add("New Text Document", ACT_NEW_TEXT);
        menu_add(NULL, ACT_LINE);
        menu_add("Paste", ACT_PASTE);
        menu_grey(!clip_full());
        menu_add(NULL, ACT_LINE);
        menu_add("Select All", ACT_SELECT_ALL);
        if (view_get_mode() == VIEW_ICONS)
            menu_add("Arrange Icons", ACT_ARRANGE);
        menu_add("Open Terminal Here", ACT_TERMINAL);
        menu_add(NULL, ACT_LINE);
        menu_add(view_hidden_shown() ? "Hide Hidden Files" : "Show Hidden Files", ACT_HIDDEN);
        menu_add("Refresh", ACT_REFRESH);
        menu_add(NULL, ACT_LINE);
        menu_add("Properties", ACT_PROPERTIES);
    }
    ui_menu_open(&menu, x, y, (ui_rect_t){ 0, 0, win->width, win->height });
}

static void menu_do(int act) {
    char full[CAB_PATH_MAX];
    switch (act) {
    case ACT_OPEN:       open_picked(); break;
    case ACT_RENAME:     view_rename_start(); break;
    case ACT_DELETE:     delete_chosen(); break;
    case ACT_CUT:        clip_chosen(1); break;
    case ACT_COPY:       clip_chosen(0); break;
    case ACT_PASTE:      paste_into(view_path()); break;
    case ACT_SELECT_ALL: view_pick_all(); break;
    case ACT_ARRANGE:    view_line_up(); break;
    case ACT_PROPERTIES: properties(); break;
    case ACT_PASTE_IN:
        if (at_home && home_picked() >= 0) {
            paste_into(home_path(home_picked()));
        } else if (!at_home && view_picked() >= 0) {
            view_entry_path(view_picked(), full, sizeof(full));
            paste_into(full);
        }
        break;
    case ACT_NEW_FOLDER: new_folder(); break;
    case ACT_NEW_TEXT:   new_text(); break;
    case ACT_REFRESH:    refresh(); break;
    case ACT_HIDDEN:
        view_show_hidden(!view_hidden_shown());
        view_refresh();
        break;
    case ACT_TERMINAL:   terminal_at(view_path()); break;
    case ACT_TERMINAL_IN:
        if (at_home && home_picked() >= 0) {
            terminal_at(home_path(home_picked()));
        } else if (!at_home && view_picked() >= 0) {
            view_entry_path(view_picked(), full, sizeof(full));
            terminal_at(full);
        }
        break;
    }
}

static void view_result(int r) {
    if (r & UI_PICKED)
        view_mode(view_list.selected == 1 ? VIEW_DETAILS : VIEW_ICONS);
    if (r)
        draw_all();
}

static void address_key(uint16_t keycode, char ch) {
    size_t len = strlen(typed);
    switch (keycode) {
    case KEY_ENTER:
        typing = 0;
        go(typed);
        break;
    case KEY_ESCAPE:
        typing = 0;
        break;
    case KEY_BACK:
        if (len)
            typed[len - 1] = '\0';
        break;
    default:
        if (ch >= ' ' && ch != 127 && len < sizeof(typed) - 1) {
            typed[len]     = ch;
            typed[len + 1] = '\0';
        }
    }
    draw_all();
}

static void on_mouse(const cs_event_t *ev) {
    int left     = (ev->buttons & CS_BUTTON_LEFT) != 0;
    int was_left = (mouse_was & CS_BUTTON_LEFT) != 0;
    int right    = (ev->buttons & CS_BUTTON_RIGHT) != 0;
    int was_right = (mouse_was & CS_BUTTON_RIGHT) != 0;
    mouse_was    = ev->buttons;

    if (menu.open) {
        int r = ui_menu_mouse(&menu, ev->x, ev->y, left, right);
        if (r & UI_PICKED)
            menu_do(menu_acts[menu.picked]);
        if (r & UI_CHANGED)
            draw_all();
        return;
    }
    if (right && !was_right) {
        if (view_renaming())
            rename_done();
        typing = 0;
        menu_show(ev->x, ev->y);
        draw_all();
        return;
    }

    int r = ui_dropdown_mouse(&view_list, ev->x, ev->y, left);
    if (r) {
        view_result(r);
        if (r & UI_TAKEN)
            return;
    }

    for (int i = 0; i < BTN_COUNT; i++) {
        int r = ui_button_mouse(&buttons[i], ev->x, ev->y, left);
        if (r & UI_CLICKED) {
            if (view_renaming())
                rename_done();
            button_clicked(i);
        }
        if (r)
            draw_all();
    }

    if (left && !was_left) {
        int in_address = ui_rect_contains(address_r, ev->x, ev->y);
        if (in_address && !typing) {
            typing = 1;
            strcpy(typed, here());
            draw_all();
        } else if (!in_address && typing) {
            typing = 0;
            draw_all();
        }
    }
    handle(at_home ? home_mouse(ev->x, ev->y, left, now_ms()) : view_mouse(ev->x, ev->y, left, keymod, now_ms()));
}

static int shortcut(const cs_event_t *ev) {
    int ctrl = (ev->keymod & KEYMOD_CTRL) != 0;
    int alt  = (ev->keymod & KEYMOD_ALT) != 0;

    if (alt && ev->keycode == KEY_ENTER) {
        properties();
        return 1;
    }
    if (alt && ev->keycode == ArrowUp) {
        go_up();
        return 1;
    }
    if (!ctrl || at_home || view_renaming())
        return 0;

    switch (ev->keycode) {
    case KEY_A: view_pick_all(); return 1;
    case KEY_C: clip_chosen(0); return 1;
    case KEY_X: clip_chosen(1); return 1;
    case KEY_V: paste_into(view_path()); return 1;
    case KEY_N:
        if (!(ev->keymod & KEYMOD_SHIFT))
            return 0;
        new_folder();
        return 1;
    }
    return 0;
}

static void on_key(const cs_event_t *ev) {
    keymod = ev->keymod;
    if (ev->key_action != KEY_DOWN)
        return;

    keyboard_event_t key = { .keycode = ev->keycode, .action = ev->key_action, .keymod = ev->keymod };
    char             ch  = tty_key_to_ascii(&key);

    if (typing) {
        address_key(ev->keycode, ch);
        return;
    }
    if (menu.open) {
        int r = ui_menu_key(&menu, ev->keycode);
        if (r & UI_PICKED)
            menu_do(menu_acts[menu.picked]);
        if (r & UI_CHANGED)
            draw_all();
        return;
    }
    if (view_list.open) {
        view_result(ui_dropdown_key(&view_list, ev->keycode));
        return;
    }
    if ((ev->keymod & KEYMOD_ALT) && (ev->keycode == ArrowLeft || ev->keycode == ArrowRight)) {
        go_history(ev->keycode == ArrowLeft ? GO_BACK : GO_FORWARD);
        draw_all();
        return;
    }
    if (shortcut(ev)) {
        draw_all();
        return;
    }
    handle(at_home ? home_key(ev->keycode) : view_key(ev->keycode, ch, ev->keymod));
}

static void on_event(const cs_event_t *ev) {
    switch (ev->type) {
    case CS_EV_CLOSE_REQUEST:
        quit = 1;
        break;
    case CS_EV_RESIZE:
        layout();
        draw_all();
        break;
    case CS_EV_MOUSE:
        on_mouse(ev);
        break;
    case CS_EV_WHEEL:
        handle(at_home ? home_wheel(ev->wheel) : view_wheel(ev->wheel));
        break;
    case CS_EV_KEY:
        on_key(ev);
        break;
    case CS_EV_DROP:
        on_drop(ev);
        draw_all();
        break;
    case CS_EV_FOCUS:
        keymod = 0;
        if (ev->focused && !at_home && view_poll())
            draw_all();
        break;
    }
}

int main(int argc, char **argv) {
    if (cs_connect() < 0) {
        printf("cabinet: needs the desktop running\n");
        return 1;
    }
    if (ui_init() < 0) {
        printf("cabinet: no font ending in " UI_FONT_REGULAR " in " UI_FONT_DIR "\n");
        return 1;
    }

    win = cs_window_open(WINDOW_W, WINDOW_H, "Cabinet");
    if (!win) {
        printf("cabinet: the desktop wouldnt give us a window\n");
        return 1;
    }

    icons_init();
    for (int i = 0; i < BTN_COUNT; i++) {
        buttons[i].image = glyph(btn_glyph[i]);
        buttons[i].flat  = 1;
    }

    layout();
    if (argc < 2 || view_open(argv[1]) < 0) {
        home_open();
        at_home = 1;
    }
    layout();
    title_update();
    draw_all();

    cs_event_t ev;
    while (!quit) {
        if (cs_event_wait(&ev, at_home ? HOME_POLL_MS : FOLDER_POLL_MS) > 0)
            on_event(&ev);
        else if (!menu.open && !view_list.open && (at_home ? home_poll() : view_poll()))
            draw_all();
    }

    char file[64];
    drop_file(file, sizeof(file));
    _unlink(file);
    cs_window_close(win);
    return 0;
}
