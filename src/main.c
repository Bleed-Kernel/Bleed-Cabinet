#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define NEW_FOLDER      "New Folder"
#define NEW_TEXT        "New Text Document"

#define KEY_ENTER       0x1C
#define KEY_BACK        0x0E
#define KEY_ESCAPE      0x01

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

static ui_menu_t     menu;
static int           menu_acts[UI_MENU_ITEMS];

static char          back[HISTORY][CAB_PATH_MAX];
static char          forward[HISTORY][CAB_PATH_MAX];
static int           back_count, forward_count;

// typing a path into the address bar
static int          typing;
static char         typed[CAB_PATH_MAX];

static int          hpet_fd = -2;

static uint64_t now_ms(void) {
    static uint64_t fallback;
    uint64_t        fs = 0;

    if (hpet_fd == -2)
        hpet_fd = _open("/dev/hpet", O_RDONLY);
    if (hpet_fd >= 0 && _ioctl(hpet_fd, HPET_IOCTL_GET_FEMTOSECONDS, &fs) == 0 && fs)
        return fs / femtosecondsPerMillisecond;
    return fallback += 10;
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
}

static void size_text(uint64_t size, char *out, size_t len) {
    if (size < 1024)
        snprintf(out, len, "%lu bytes", (unsigned long)size);
    else if (size < 1024 * 1024)
        snprintf(out, len, "%lu.%02luKB", (unsigned long)(size / 1024), (unsigned long)(size % 1024 * 100 / 1024));
    else
        snprintf(out, len, "%lu.%02luMB", (unsigned long)(size >> 20),
                 (unsigned long)((size & 0xFFFFF) * 100 >> 20));
}

static void status_draw(ui_canvas_t *c) {
    char left[64], right[48] = "";
    int  i    = view_picked();
    int  hint = -1;
    for (int b = 0; b < BTN_COUNT; b++) {
        if (buttons[b].hover)
            hint = b;
    }

    if (hint >= 0) {
        snprintf(left, sizeof(left), "%s", btn_hint[hint]);
    } else if (i >= 0) {
        const entry_t *e = view_entry(i);
        snprintf(left, sizeof(left), "1 object(s) selected");
        if (e->kind != KIND_FOLDER && e->kind != KIND_DEVICE)
            size_text(e->size, right, sizeof(right));
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
    ui_text_ascii(text, sizeof(text), typing ? typed : view_path());

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
    buttons[BTN_UP].disabled      = !strcmp(view_path(), "/");
    buttons[BTN_DELETE].disabled  = view_picked() < 0;
    for (int i = 0; i < BTN_COUNT; i++)
        ui_button_draw(&c, &buttons[i]);
    for (int g = 0; g < 2; g++) {
        ui_fill(&c, (ui_rect_t){ grooves[g], 4, 1, TOOL_H - 8 }, UI_COLOUR_SHADOW);
        ui_fill(&c, (ui_rect_t){ grooves[g] + 1, 4, 1, TOOL_H - 8 }, UI_COLOUR_HILIGHT);
    }
    ui_separator(&c, 0, TOOL_H, win->width);

    address_draw(&c);
    view_draw(&c);
    status_draw(&c);

    // these go over everything else
    ui_dropdown_draw(&c, &view_list);
    ui_menu_draw(&c, &menu);
    cs_window_commit(win, 0, 0, win->width, win->height);
}

static void title_update(void) {
    const char *p    = view_path();
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
    strcpy(from, view_path());

    if (view_open(p) < 0) {
        char text[CAB_PATH_MAX + 64];
        snprintf(text, sizeof(text), "Cabinet cant open '%s'.\nMake sure the folder is there and try again.", p);
        ui_message("Cabinet", text, UI_ICON_ERROR);
        return -1;
    }
    if (strcmp(from, view_path())) {
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
    strcpy(from, view_path());
    strcpy(to, stack[--(*count)]);
    if (go_to(to, how) < 0)
        return;

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

// the folder above, with the one we came out of picked
static void go_up(void) {
    char p[CAB_PATH_MAX];
    strcpy(p, view_path());
    if (!strcmp(p, "/"))
        return;

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

static void run(const char *program, const char *file) {
    const char *argv[] = { program, file, NULL };
    if (!exists(program) || cs_run(argv) < 0) {
        char text[CAB_PATH_MAX + 64];
        snprintf(text, sizeof(text), "Cabinet couldnt start '%s'.", program);
        ui_message("Cabinet", text, UI_ICON_ERROR);
    }
}

static void open_picked(void) {
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

// everything in it first, a folder has to be empty to go
static int remove_tree(const char *p, int folder) {
    if (folder) {
        int fd = _open(p, O_RDONLY);
        if (fd < 0)
            return -1;

        // read the lot before taking any out, the entries move up as they go
        int       n     = 0;
        dirent_t *names = NULL;
        dirent_t  entry;
        for (size_t i = 0; _readdir(fd, i, &entry) == 0; i++) {
            dirent_t *more = realloc(names, (size_t)(n + 1) * sizeof(dirent_t));
            if (!more)
                break;
            names      = more;
            names[n++] = entry;
        }
        _close(fd);

        for (int i = 0; i < n; i++) {
            names[i].name[sizeof(names[i].name) - 1] = '\0';
            if (!strcmp(names[i].name, ".") || !strcmp(names[i].name, ".."))
                continue;
            char child[CAB_PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", p, names[i].name);
            remove_tree(child, names[i].type == 0);
        }
        free(names);
    }
    return _unlink(p) < 0 ? -1 : 0;
}

static void delete_picked(void) {
    int i = view_picked();
    if (i < 0)
        return;

    const entry_t *e = view_entry(i);
    char           text[CAB_NAME_MAX + 96];
    if (e->kind == KIND_FOLDER)
        snprintf(text, sizeof(text), "Are you sure you want to remove the folder '%s' and all its contents?", e->name);
    else
        snprintf(text, sizeof(text), "Are you sure you want to delete '%s'?", e->name);
    if (!ui_ask(e->kind == KIND_FOLDER ? "Confirm Folder Delete" : "Confirm File Delete", text))
        return;

    char full[CAB_PATH_MAX];
    view_entry_path(i, full, sizeof(full));
    if (remove_tree(full, e->kind == KIND_FOLDER) < 0) {
        snprintf(text, sizeof(text), "Cannot delete '%s'.\nIt may be read only or in use.", e->name);
        ui_message("Error Deleting File", text, UI_ICON_ERROR);
    }
    view_refresh();
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

    char text[CAB_NAME_MAX + 96];
    if (name_taken(name)) {
        snprintf(text, sizeof(text), "Cannot rename '%s': a file with the name you specified already exists.", e->name);
        ui_message("Error Renaming File", text, UI_ICON_ERROR);
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
        delete_picked();
    if (r)
        draw_all();
}

static void button_clicked(int i) {
    switch (i) {
    case BTN_BACK:    go_history(GO_BACK); break;
    case BTN_FORWARD: go_history(GO_FORWARD); break;
    case BTN_UP:      go_up(); break;
    case BTN_REFRESH: view_refresh(); break;
    case BTN_NEW:     new_folder(); break;
    case BTN_DELETE:  delete_picked(); break;
    }
}

static void menu_add(const char *label, int act) {
    menu.items[menu.count]    = act == ACT_LINE ? UI_MENU_LINE : label;
    menu.disabled[menu.count] = 0;
    menu_acts[menu.count++]   = act;
}

// on something its what you can do to it, on empty space its what you can make here
static void menu_show(int x, int y) {
    int hit = view_at(x, y);
    if (hit == -2)
        return;
    view_pick(hit);

    menu.count = 0;
    if (hit >= 0) {
        const entry_t *e = view_entry(hit);
        menu_add("Open", ACT_OPEN);
        if (e->kind == KIND_FOLDER)
            menu_add("Open Terminal Here", ACT_TERMINAL_IN);
        menu_add(NULL, ACT_LINE);
        menu_add("Rename", ACT_RENAME);
        menu_add("Delete", ACT_DELETE);
        menu.disabled[0] = e->kind == KIND_DEVICE || e->kind == KIND_FILE;
    } else {
        menu_add("New Folder", ACT_NEW_FOLDER);
        menu_add("New Text Document", ACT_NEW_TEXT);
        menu_add(NULL, ACT_LINE);
        menu_add("Open Terminal Here", ACT_TERMINAL);
        menu_add(NULL, ACT_LINE);
        menu_add("Refresh", ACT_REFRESH);
    }
    ui_menu_open(&menu, x, y, (ui_rect_t){ 0, 0, win->width, win->height });
}

static void menu_do(int act) {
    char full[CAB_PATH_MAX];
    switch (act) {
    case ACT_OPEN:       open_picked(); break;
    case ACT_RENAME:     view_rename_start(); break;
    case ACT_DELETE:     delete_picked(); break;
    case ACT_NEW_FOLDER: new_folder(); break;
    case ACT_NEW_TEXT:   new_text(); break;
    case ACT_REFRESH:    view_refresh(); break;
    case ACT_TERMINAL:   terminal_at(view_path()); break;
    case ACT_TERMINAL_IN:
        if (view_picked() >= 0) {
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
            strcpy(typed, view_path());
            draw_all();
        } else if (!in_address && typing) {
            typing = 0;
            draw_all();
        }
    }
    handle(view_mouse(ev->x, ev->y, left, now_ms()));
}

static void on_key(const cs_event_t *ev) {
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
    handle(view_key(ev->keycode, ch));
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
        handle(view_wheel(ev->wheel));
        break;
    case CS_EV_KEY:
        on_key(ev);
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
    if (view_open(argc > 1 ? argv[1] : START_DIR) < 0)
        view_open(START_DIR);
    layout();
    title_update();
    draw_all();

    cs_event_t ev;
    while (!quit) {
        if (cs_event_wait(&ev, -1) > 0)
            on_event(&ev);
    }

    cs_window_close(win);
    return 0;
}
