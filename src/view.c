#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <fcntl.h>
#include <syscalls.h>
#include <keyboard.h>
#include <crime/crime.h>
#include <cabinet.h>

#define CELL_W          76
#define CELL_H          70
#define GRID_PAD        4
#define ICON_TOP        4
#define LABEL_TOP       40
#define LABEL_PAD       2
#define ROW_H           18
#define HEAD_H          20
#define SIZE_W          80
#define TYPE_W          130
#define DRAG_START      4       // how far it has to move before its a drag and not a click
#define DOUBLE_CLICK_MS 400
#define WHEEL_ROWS      3
#define SPOTS_MAX       1024
#define DIR_TYPE        0       // INODE_DIRECTORY
#define DEVICE_TYPE     2       // and INODE_DEVICE

#define KEY_ENTER       0x1C
#define KEY_BACK        0x0E
#define KEY_ESCAPE      0x01
#define KEY_DELETE      0x53

#define COLOUR_EDIT     0x0B0B0D

enum {
    SORT_NAME,
    SORT_SIZE,
    SORT_TYPE,
    SORTS,
};

static const char *sort_label[SORTS] = { "Name", "Size", "Type" };

typedef struct {
    char path[CAB_PATH_MAX];
    int  col, row;
} spot_t;

static char      path[CAB_PATH_MAX];
static entry_t  *items;
static int       count;
static int       cap;
static int       picked = -1;
static int       mode   = VIEW_ICONS;
static int       cols   = 1;

static ui_rect_t frame, inner;
static ui_scrollbar_t bar;

static int       held = -1;
static int       moved;
static int       grab_x, grab_y;
static int       from_x, from_y;
static int       drag_x, drag_y;    // where the held ones cell is, from the top left of the grid
static int       last_click = -1;
static uint64_t  last_click_ms;
static uint8_t   was_left;

static int       sort_key;
static int       sort_back;

static int       editing;
static int       show_hidden;
static int       edit_fresh;        // the whole name is picked out, the first key replaces it
static char      edit[CAB_NAME_MAX];

// where things were left in folders weve been in, the oldest goes when its full
static spot_t   *spots;
static int       spots_count;
static int       spots_next;

static void join(const char *dir, const char *name, char *out, size_t size) {
    snprintf(out, size, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

static int is_dir(const char *p) {
    if (!strcmp(p, "/"))
        return 1;

    char parent[CAB_PATH_MAX];
    strncpy(parent, p, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *slash = strrchr(parent, '/');
    if (!slash || !slash[1])
        return 0;
    const char *name = p + (slash - parent) + 1;
    if (slash == parent)
        parent[1] = '\0';
    else
        *slash = '\0';

    int fd = _open(parent, O_RDONLY);
    if (fd < 0)
        return 0;

    int      dir = 0;
    dirent_t entry;
    for (size_t i = 0; _readdir(fd, i, &entry) == 0; i++) {
        entry.name[sizeof(entry.name) - 1] = '\0';
        if (!strcmp(entry.name, name)) {
            dir = entry.type == DIR_TYPE;
            break;
        }
    }
    _close(fd);
    return dir;
}

static const char *extension(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && dot != name ? dot + 1 : NULL;
}

static int ext_in(const char *ext, const char *const *list) {
    for (int i = 0; ext && list[i]; i++) {
        if (!strcasecmp(ext, list[i]))
            return 1;
    }
    return 0;
}

// I would one day like to do a registry like system
static const char *const text_exts[]  = { "txt", "md", "c", "h", "s", "asm", "conf", "cfg", "ini", "sh", "log",
                                          "json", "xml", "html", "css", "py", "lua", "mk", "ld", "lds", NULL };
static const char *const audio_exts[] = { "mp3", "wav", "flac", "ogg", NULL };
static const char *const image_exts[] = { "png", "bmp", "jpg", "jpeg", "jpe", "jfif", "gif", "tga", "psd", "pnm", "ppm", "pgm", "hdr", "pic", NULL };

// the name says what it is, or the first four bytes do for programs that have no extension
static int kind_of(const char *full, const char *name, uint64_t *size) {
    const char *ext = extension(name);
    int         fd  = _open(full, O_RDONLY);
    char        magic[4] = {0};

    *size = 0;
    if (fd >= 0) {
        long end = _seek(fd, 0, SEEK_END);
        *size    = end > 0 ? (uint64_t)end : 0;
        if (!ext && _seek(fd, 0, SEEK_SET) == 0)
            _read(fd, magic, sizeof(magic));
        _close(fd);
    }

    if (ext_in(ext, text_exts))  return KIND_TEXT;
    if (ext_in(ext, audio_exts)) return KIND_AUDIO;
    if (ext_in(ext, image_exts)) return KIND_IMAGE;
    if (!memcmp(magic, "\x7f" "ELF", 4))
        return KIND_PROGRAM;
    return KIND_FILE;
}

// the type column, the way 9x wrote it
static void kind_name(const entry_t *e, char *out, size_t size) {
    char        ext[16] = "";
    const char *dot     = extension(e->name);
    for (int i = 0; dot && dot[i] && i < (int)sizeof(ext) - 1; i++) {
        ext[i]     = (dot[i] >= 'a' && dot[i] <= 'z') ? dot[i] - 'a' + 'A' : dot[i];
        ext[i + 1] = '\0';
    }

    switch (e->kind) {
    case KIND_FOLDER:  snprintf(out, size, "File Folder"); break;
    case KIND_PROGRAM: snprintf(out, size, "Application"); break;
    case KIND_TEXT:    snprintf(out, size, "Text Document"); break;
    case KIND_AUDIO:   snprintf(out, size, "%s Audio", ext); break;
    case KIND_IMAGE:   snprintf(out, size, "%s Image", ext); break;
    case KIND_DEVICE:  snprintf(out, size, "Device"); break;
    default:           snprintf(out, size, ext[0] ? "%s File" : "File", ext); break;
    }
}

// folders always come first, whichever way its sorted
static int compare(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if ((x->kind == KIND_FOLDER) != (y->kind == KIND_FOLDER))
        return x->kind == KIND_FOLDER ? -1 : 1;

    int r = 0;
    if (sort_key == SORT_SIZE && x->size != y->size)
        r = x->size < y->size ? -1 : 1;
    if (sort_key == SORT_TYPE) {
        char tx[32], ty[32];
        kind_name(x, tx, sizeof(tx));
        kind_name(y, ty, sizeof(ty));
        r = strcasecmp(tx, ty);
    }
    if (!r)
        r = strcasecmp(x->name, y->name);
    return sort_back ? -r : r;
}

static void sort_all(void) {
    qsort(items, count, sizeof(entry_t), compare);
}

static void sort_items(void) {
    char name[CAB_NAME_MAX] = "";
    if (picked >= 0)
        strcpy(name, items[picked].name);
    sort_all();
    if (name[0])
        view_pick_name(name);
}

// spots

void view_keep_spot(const char *p, int col, int row) {
    if (!spots && !(spots = calloc(SPOTS_MAX, sizeof(spot_t))))
        return;
    if (strlen(p) >= CAB_PATH_MAX)
        return;

    for (int i = 0; i < spots_count; i++) {
        if (!strcmp(spots[i].path, p)) {
            spots[i].col = col;
            spots[i].row = row;
            return;
        }
    }
    spot_t *s = &spots[spots_next];
    spots_next = (spots_next + 1) % SPOTS_MAX;
    if (spots_count < SPOTS_MAX)
        spots_count++;
    strcpy(s->path, p);
    s->col = col;
    s->row = row;
}

static int spot_find(const char *p, int *col, int *row) {
    for (int i = 0; i < spots_count; i++) {
        if (!strcmp(spots[i].path, p)) {
            *col = spots[i].col;
            *row = spots[i].row;
            return 1;
        }
    }
    return 0;
}

static void keep_all(void) {
    char full[CAB_PATH_MAX];
    for (int i = 0; i < count; i++) {
        join(path, items[i].name, full, sizeof(full));
        view_keep_spot(full, items[i].col, items[i].row);
    }
}

static int grid_rows(void) {
    return count + cols;
}

// a cell for everything: where it was, else where it was left last time, else the first gap
static void place(void) {
    cols = (inner.w - GRID_PAD * 2) / CELL_W;
    if (cols < 1)
        cols = 1;

    int      rows = grid_rows();
    uint8_t *used = calloc((size_t)cols * rows, 1);
    if (!used)
        return;

    char full[CAB_PATH_MAX];
    for (int i = 0; i < count; i++) {
        entry_t *e = &items[i];
        int      c = e->col, r = e->row;
        if (c < 0) {
            join(path, e->name, full, sizeof(full));
            if (!spot_find(full, &c, &r))
                c = -1;
        }
        if (c >= 0 && c < cols && r >= 0 && r < rows && !used[r * cols + c]) {
            used[r * cols + c] = 1;
            e->col = c;
            e->row = r;
        } else {
            e->col = -1;
        }
    }

    int next = 0;
    for (int i = 0; i < count; i++) {
        if (items[i].col >= 0)
            continue;
        while (used[next])
            next++;
        used[next]   = 1;
        items[i].col = next % cols;
        items[i].row = next / cols;
    }
    free(used);
}

static int last_row(void) {
    int most = 0;
    for (int i = 0; i < count; i++) {
        if (items[i].row > most)
            most = items[i].row;
    }
    return most;
}

// the grids top left on the window, scrolled
static int grid_x(void) {
    return inner.x + GRID_PAD;
}

static int grid_y(void) {
    return inner.y + GRID_PAD - bar.top * CELL_H;
}

static ui_rect_t cell_rect(int i) {
    return (ui_rect_t){ grid_x() + items[i].col * CELL_W, grid_y() + items[i].row * CELL_H, CELL_W, CELL_H };
}

static int list_top(void) {
    return inner.y + HEAD_H;
}

static ui_rect_t row_rect(int i) {
    return (ui_rect_t){ inner.x, list_top() + (i - bar.top) * ROW_H, inner.w, ROW_H };
}

static void bar_update(void) {
    if (mode == VIEW_ICONS) {
        bar.total   = count ? last_row() + 1 : 0;
        bar.visible = (inner.h - GRID_PAD) / CELL_H;
    } else {
        bar.total   = count;
        bar.visible = (inner.h - HEAD_H) / ROW_H;
    }
    if (bar.visible < 1)
        bar.visible = 1;
    ui_scrollbar_to(&bar, bar.top);
}

static void show(int i) {
    if (i < 0)
        return;
    int at = mode == VIEW_ICONS ? items[i].row : i;
    if (at < bar.top)
        ui_scrollbar_to(&bar, at);
    else if (at >= bar.top + bar.visible)
        ui_scrollbar_to(&bar, at - bar.visible + 1);
}

// the one dropped goes to the closest cell nothing else is in
static void snap(int i) {
    int      rows = grid_rows();
    uint8_t *used = calloc((size_t)cols * rows, 1);
    if (!used)
        return;
    for (int j = 0; j < count; j++) {
        if (j != i && items[j].col < cols && items[j].row < rows)
            used[items[j].row * cols + items[j].col] = 1;
    }

    long best = -1;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (used[r * cols + c])
                continue;
            long dx = c * CELL_W - drag_x, dy = r * CELL_H - drag_y;
            if (best < 0 || dx * dx + dy * dy < best) {
                best         = dx * dx + dy * dy;
                items[i].col = c;
                items[i].row = r;
            }
        }
    }
    free(used);

    char full[CAB_PATH_MAX];
    join(path, items[i].name, full, sizeof(full));
    view_keep_spot(full, items[i].col, items[i].row);
    bar_update();
}

// listing

int view_open(const char *want) {
    char clean[CAB_PATH_MAX];
    strncpy(clean, want, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    size_t len = strlen(clean);
    while (len > 1 && clean[len - 1] == '/')
        clean[--len] = '\0';
    if (clean[0] != '/' || !is_dir(clean))
        return -1;

    int fd = _open(clean, O_RDONLY);
    if (fd < 0)
        return -1;

    keep_all();
    count   = 0;
    picked  = -1;
    held    = -1;
    editing = 0;
    strcpy(path, clean);

    dirent_t entry;
    for (size_t i = 0; _readdir(fd, i, &entry) == 0; i++) {
        entry.name[sizeof(entry.name) - 1] = '\0';
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, "..") || !entry.name[0])
            continue;
        if (!show_hidden && cs_name_hidden(entry.name))
            continue;

        if (count == cap) {
            int      grow = cap ? cap * 2 : 64;
            entry_t *more = realloc(items, (size_t)grow * sizeof(entry_t));
            if (!more)
                break;
            items = more;
            cap   = grow;
        }

        entry_t *e = &items[count++];
        memset(e, 0, sizeof(*e));
        strcpy(e->name, entry.name);
        e->col = e->row = -1;

        char full[CAB_PATH_MAX];
        join(path, e->name, full, sizeof(full));
        if (entry.type == DIR_TYPE)
            e->kind = KIND_FOLDER;
        else if (entry.type == DEVICE_TYPE)
            e->kind = KIND_DEVICE;
        else
            e->kind = kind_of(full, e->name, &e->size);
        e->big   = icon_for(full, e->kind, ICON_BIG);
        e->small = icon_for(full, e->kind, ICON_SMALL);
    }
    _close(fd);

    sort_all();
    bar.top = 0;
    place();
    bar_update();
    return 0;
}

void view_refresh(void) {
    char name[CAB_NAME_MAX] = "";
    char here[CAB_PATH_MAX];
    if (picked >= 0)
        strcpy(name, items[picked].name);
    strcpy(here, path);

    int top = bar.top;
    if (view_open(here) < 0)
        return;
    ui_scrollbar_to(&bar, top);
    if (name[0])
        view_pick_name(name);
}

const char *view_path(void) {
    return path;
}

void view_show_hidden(int on) {
    show_hidden = on;
}

int view_hidden_shown(void) {
    return show_hidden;
}

void view_layout(ui_rect_t r) {
    frame    = r;
    inner    = ui_rect_shrink(r, 2);
    inner.w -= UI_SCROLL_W;
    bar.rect = (ui_rect_t){ inner.x + inner.w, inner.y, UI_SCROLL_W, inner.h };
    place();
    bar_update();
    show(picked);
}

void view_mode(int m) {
    if (m == mode)
        return;
    mode    = m;
    bar.top = 0;
    bar_update();
    show(picked);
}

int view_get_mode(void) {
    return mode;
}

int view_count(void) {
    return count;
}

const entry_t *view_entry(int i) {
    return i >= 0 && i < count ? &items[i] : NULL;
}

int view_picked(void) {
    return picked;
}

void view_pick_name(const char *name) {
    for (int i = 0; i < count; i++) {
        if (!strcmp(items[i].name, name)) {
            picked = i;
            show(i);
            return;
        }
    }
}

void view_entry_path(int i, char *out, size_t size) {
    join(path, items[i].name, out, size);
}

// drawing

// as much of the name as goes in two lines, broken at a space if theres one
static int label_lines(const char *name, int max_w, char lines[2][CAB_NAME_MAX]) {
    char text[CAB_NAME_MAX];
    ui_text_ascii(text, sizeof(text), name);

    const char *at = text;
    int         n  = 0;
    while (*at && n < 2) {
        size_t len  = strlen(at);
        size_t fits = 0, space = 0;
        char   buf[CAB_NAME_MAX];

        for (size_t i = 1; i <= len; i++) {
            memcpy(buf, at, i);
            buf[i] = '\0';
            if (ui_text_width(&ui_font_text, buf) > max_w)
                break;
            fits = i;
            if (strchr(" -_.", at[i - 1]))
                space = i;
        }
        if (!fits)
            fits = 1;
        if (fits < len && space && n == 0)
            fits = space;

        if (n == 1 && fits < len) {
            int dots = ui_text_width(&ui_font_text, "..");
            while (fits > 0) {
                memcpy(buf, at, fits);
                buf[fits] = '\0';
                if (ui_text_width(&ui_font_text, buf) + dots <= max_w)
                    break;
                fits--;
            }
            snprintf(lines[n++], CAB_NAME_MAX, "%s..", buf);
            break;
        }

        memcpy(lines[n], at, fits);
        lines[n][fits] = '\0';
        n++;
        at += fits;
    }
    return n;
}

static void edit_draw(ui_canvas_t *c, ui_rect_t r) {
    ui_fill(c, r, COLOUR_EDIT);
    ui_edge(c, r, UI_COLOUR_TEXT, UI_COLOUR_TEXT);

    char text[CAB_NAME_MAX];
    ui_text_ascii(text, sizeof(text), edit);
    int tw = ui_text_width(&ui_font_text, text);
    int tx = r.x + 3;
    if (tx + tw + 2 > r.x + r.w - 3)
        tx = r.x + r.w - 5 - tw;

    ui_rect_t old = c->clip;
    ui_clip(c, ui_rect_clip(ui_rect_shrink(r, 1), old));
    if (edit_fresh)
        ui_fill(c, (ui_rect_t){ tx - 1, r.y + 2, tw + 2, r.h - 4 }, UI_COLOUR_CAPTION_R);
    ui_text_in(c, &ui_font_text, (ui_rect_t){ tx, r.y, tw + 1, r.h }, text, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
    ui_fill(c, (ui_rect_t){ tx + tw + 1, r.y + 3, 1, r.h - 6 }, UI_COLOUR_TEXT);
    c->clip = old;
}

static void cell_draw(ui_canvas_t *c, int i, int x, int y) {
    const entry_t *e  = &items[i];
    int            on = i == picked;

    if (e->big)
        icon_draw(c, e->big, x + (CELL_W - e->big->w) / 2, y + ICON_TOP + (ICON_BIG - e->big->h) / 2, on);

    int lh = ui_font_height(&ui_font_text);
    if (on && editing) {
        int w = ui_text_width(&ui_font_text, edit) + 10;
        if (w < CELL_W - 8)
            w = CELL_W - 8;
        if (w > CELL_W + 24)
            w = CELL_W + 24;
        edit_draw(c, (ui_rect_t){ x + (CELL_W - w) / 2, y + LABEL_TOP - 2, w, lh + 4 });
        return;
    }

    char lines[2][CAB_NAME_MAX];
    int  n = label_lines(e->name, CELL_W - LABEL_PAD * 2 - 2, lines);
    for (int l = 0; l < n; l++) {
        int tw = ui_text_width(&ui_font_text, lines[l]);
        int tx = x + (CELL_W - tw) / 2;
        int ty = y + LABEL_TOP + l * lh;
        if (on)
            ui_fill(c, (ui_rect_t){ tx - LABEL_PAD, ty, tw + LABEL_PAD * 2, lh }, UI_COLOUR_CAPTION_R);
        ui_text(c, &ui_font_text, tx, ty, lines[l], UI_COLOUR_TEXT);
    }
}

static void icons_draw(ui_canvas_t *c) {
    for (int i = 0; i < count; i++) {
        if (i == held && moved)
            continue;
        ui_rect_t r = cell_rect(i);
        if (r.y + r.h < inner.y || r.y > inner.y + inner.h)
            continue;
        cell_draw(c, i, r.x, r.y);
    }
    // the one being dragged goes over the rest
    if (held >= 0 && moved)
        cell_draw(c, held, grid_x() + drag_x, grid_y() + drag_y);
}

static ui_rect_t head_rect(int col) {
    int name_w = inner.w - SIZE_W - TYPE_W;
    if (col == SORT_NAME) return (ui_rect_t){ inner.x, inner.y, name_w, HEAD_H };
    if (col == SORT_SIZE) return (ui_rect_t){ inner.x + name_w, inner.y, SIZE_W, HEAD_H };
    return (ui_rect_t){ inner.x + name_w + SIZE_W, inner.y, TYPE_W, HEAD_H };
}

static void kb_text(uint64_t size, char *out, size_t len) {
    snprintf(out, len, "%luKB", (unsigned long)((size + 1023) / 1024));
}

static void details_draw(ui_canvas_t *c) {
    for (int col = 0; col < SORTS; col++) {
        ui_rect_t h = head_rect(col);
        ui_fill(c, h, UI_COLOUR_BUTTON);
        ui_edge(c, h, UI_COLOUR_BUTTON_HI, UI_COLOUR_BUTTON_LO);
        ui_rect_t label = ui_rect_shrink(h, 1);
        label.x += 5;
        label.w -= 10;
        ui_text_in(c, &ui_font_text, label, sort_label[col], UI_COLOUR_TEXT,
                   col == SORT_SIZE ? UI_ALIGN_RIGHT : UI_ALIGN_LEFT);
    }

    ui_rect_t old = c->clip;
    ui_clip(c, ui_rect_clip((ui_rect_t){ inner.x, list_top(), inner.w, inner.h - HEAD_H }, old));

    for (int i = bar.top; i < count && i < bar.top + bar.visible + 1; i++) {
        const entry_t *e  = &items[i];
        ui_rect_t      r  = row_rect(i);
        int            on = i == picked;

        if (e->small)
            icon_draw(c, e->small, r.x + 4, r.y + (ROW_H - e->small->h) / 2, on);

        ui_rect_t name = head_rect(SORT_NAME);
        name = (ui_rect_t){ r.x + 24, r.y, name.w - 28, ROW_H };
        if (on && editing) {
            edit_draw(c, (ui_rect_t){ name.x - 2, r.y, name.w, ROW_H });
        } else {
            char text[CAB_NAME_MAX];
            ui_text_ascii(text, sizeof(text), e->name);
            if (on) {
                int tw = ui_text_width(&ui_font_text, text);
                ui_fill(c, (ui_rect_t){ name.x - 2, r.y + 1, (tw + 4 < name.w ? tw + 4 : name.w), ROW_H - 2 },
                        UI_COLOUR_CAPTION_R);
            }
            ui_rect_t clip = c->clip;
            ui_clip(c, ui_rect_clip(name, clip));
            ui_text_in(c, &ui_font_text, name, text, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
            c->clip = clip;
        }

        char text[48];
        if (e->kind != KIND_FOLDER && e->kind != KIND_DEVICE) {
            ui_rect_t s = head_rect(SORT_SIZE);
            kb_text(e->size, text, sizeof(text));
            ui_text_in(c, &ui_font_text, (ui_rect_t){ s.x, r.y, s.w - 6, ROW_H }, text, UI_COLOUR_TEXT,
                       UI_ALIGN_RIGHT);
        }
        ui_rect_t t = head_rect(SORT_TYPE);
        kind_name(e, text, sizeof(text));
        ui_text_in(c, &ui_font_text, (ui_rect_t){ t.x + 6, r.y, t.w - 6, ROW_H }, text, UI_COLOUR_TEXT,
                   UI_ALIGN_LEFT);
    }
    c->clip = old;
}

void view_draw(ui_canvas_t *c) {
    ui_panel(c, frame, UI_SUNKEN);
    ui_fill(c, inner, COLOUR_VIEW);

    ui_rect_t old = c->clip;
    ui_clip(c, ui_rect_clip(inner, old));
    if (mode == VIEW_ICONS)
        icons_draw(c);
    else
        details_draw(c);
    c->clip = old;

    if (bar.total > bar.visible)
        ui_scrollbar_draw(c, &bar);
    else
        ui_fill(c, bar.rect, COLOUR_VIEW);
}

// input

static int item_at(int x, int y) {
    if (mode == VIEW_DETAILS) {
        if (y < list_top())
            return -1;
        int i = bar.top + (y - list_top()) / ROW_H;
        return i < count ? i : -1;
    }

    // the icon and the label count, not the gaps either side
    for (int i = count - 1; i >= 0; i--) {
        ui_rect_t r = cell_rect(i);
        ui_rect_t hit = { r.x + 6, r.y, r.w - 12, r.h - 4 };
        if (ui_rect_contains(hit, x, y))
            return i;
    }
    return -1;
}

static int pick(int i) {
    if (i == picked)
        return 0;
    picked = i;
    show(i);
    return VIEW_REDRAW | VIEW_STATUS;
}

void view_pick(int i) {
    pick(i >= 0 && i < count ? i : -1);
}

int view_at(int x, int y) {
    if (!ui_rect_contains(inner, x, y))
        return -2;
    if (mode == VIEW_DETAILS && y < list_top())
        return -2;
    return item_at(x, y);
}

int view_mouse(int x, int y, int left, uint64_t now_ms) {
    int press = left && !was_left;
    was_left  = (uint8_t)left;

    if (bar.total > bar.visible && held < 0) {
        int r = ui_scrollbar_mouse(&bar, x, y, left);
        if (r & UI_TAKEN)
            return (r & UI_CHANGED) ? VIEW_REDRAW : 0;
    }

    if (held >= 0) {
        if (left) {
            int dx = x - from_x, dy = y - from_y;
            if (mode == VIEW_ICONS && !moved && dx * dx + dy * dy >= DRAG_START * DRAG_START)
                moved = 1;
            if (!moved)
                return 0;
            drag_x = x - grab_x - grid_x();
            drag_y = y - grab_y - grid_y();
            return VIEW_REDRAW;
        }

        int r = 0;
        if (moved) {
            snap(held);
            r = VIEW_REDRAW;
        }
        held  = -1;
        moved = 0;
        return r;
    }

    if (!press || !ui_rect_contains(inner, x, y))
        return 0;

    // clicking off the name box keeps what was typed
    if (editing)
        return VIEW_RENAMED;

    if (mode == VIEW_DETAILS && y < list_top()) {
        for (int col = 0; col < SORTS; col++) {
            if (!ui_rect_contains(head_rect(col), x, y))
                continue;
            sort_back = col == sort_key ? !sort_back : 0;
            sort_key  = col;
            sort_items();
            return VIEW_REDRAW;
        }
        return 0;
    }

    int i = item_at(x, y);
    int r = pick(i);

    if (i >= 0 && i == last_click && now_ms - last_click_ms < DOUBLE_CLICK_MS) {
        last_click = -1;
        return r | VIEW_OPEN;
    }
    last_click    = i;
    last_click_ms = now_ms;

    if (i >= 0) {
        ui_rect_t cell = cell_rect(i);
        held   = i;
        moved  = 0;
        from_x = x;
        from_y = y;
        grab_x = x - cell.x;
        grab_y = y - cell.y;
    }
    return r | VIEW_REDRAW;
}

int view_wheel(int notches) {
    int top = bar.top;
    ui_scrollbar_to(&bar, bar.top - notches * (mode == VIEW_ICONS ? 1 : WHEEL_ROWS));
    return bar.top != top ? VIEW_REDRAW : 0;
}

// the nearest one that way, going straight counts for more than going across
static int neighbour(int dc, int dr) {
    if (picked < 0)
        return count ? 0 : -1;

    int  best  = -1;
    long score = 0;
    for (int i = 0; i < count; i++) {
        int  c = items[i].col - items[picked].col;
        int  r = items[i].row - items[picked].row;
        long along  = dc ? (long)c * dc : (long)r * dr;
        long across = dc ? (r < 0 ? -r : r) : (c < 0 ? -c : c);
        if (i == picked || along <= 0)
            continue;
        long s = along + across * 8;
        if (best < 0 || s < score) {
            best  = i;
            score = s;
        }
    }
    return best >= 0 ? best : picked;
}

static int first_in_grid(void) {
    int best = -1;
    for (int i = 0; i < count; i++) {
        if (best < 0 || items[i].row < items[best].row ||
            (items[i].row == items[best].row && items[i].col < items[best].col))
            best = i;
    }
    return best;
}

// typing a letter goes to the next thing starting with it
static int jump_to(char ch) {
    if (ch >= 'A' && ch <= 'Z')
        ch += 'a' - 'A';
    for (int n = 1; n <= count; n++) {
        int  i = (picked + n + count) % count;
        char f = items[i].name[0];
        if (f >= 'A' && f <= 'Z')
            f += 'a' - 'A';
        if (f == ch)
            return pick(i);
    }
    return 0;
}

static int edit_key(uint16_t keycode, char ch) {
    // shift on its own is a key too, it mustnt use up the first key
    if (!ch)
        return 0;

    int fresh = edit_fresh;
    edit_fresh = 0;
    if (fresh && (keycode == KEY_BACK || (ch >= ' ' && ch != '/' && ch != 127)))
        edit[0] = '\0';

    size_t len = strlen(edit);
    switch (keycode) {
    case KEY_ENTER:
        return VIEW_RENAMED;
    case KEY_ESCAPE:
        editing = 0;
        return VIEW_REDRAW;
    case KEY_BACK:
        if (len && !fresh)
            edit[len - 1] = '\0';
        return VIEW_REDRAW;
    }
    if (ch >= ' ' && ch != '/' && ch != 127 && len < sizeof(edit) - 1) {
        edit[len]     = ch;
        edit[len + 1] = '\0';
        return VIEW_REDRAW;
    }
    return 0;
}

int view_key(uint16_t keycode, char ch) {
    if (editing)
        return edit_key(keycode, ch);
    if (!count && keycode != KEY_BACK && keycode != F5)
        return 0;

    int icons = mode == VIEW_ICONS;
    int page  = bar.visible > 1 ? bar.visible - 1 : 1;
    switch (keycode) {
    case ArrowUp:    return pick(icons ? neighbour(0, -1) : (picked > 0 ? picked - 1 : 0));
    case ArrowDown:  return pick(icons ? neighbour(0, 1) : (picked + 1 < count ? picked + 1 : count - 1));
    case ArrowLeft:  return icons ? pick(neighbour(-1, 0)) : 0;
    case ArrowRight: return icons ? pick(neighbour(1, 0)) : 0;
    case PageUp:     return icons ? 0 : pick(picked - page > 0 ? picked - page : 0);
    case PageDown:   return icons ? 0 : pick(picked + page < count ? picked + page : count - 1);
    case Home:       return pick(icons ? first_in_grid() : 0);
    case End:        return pick(count - 1);
    case KEY_ENTER:  return picked >= 0 ? VIEW_OPEN : 0;
    case KEY_BACK:   return VIEW_UP;
    case KEY_DELETE: return picked >= 0 ? VIEW_DELETE : 0;
    case F2:
        view_rename_start();
        return VIEW_REDRAW;
    case F5:
        view_refresh();
        return VIEW_REDRAW | VIEW_STATUS;
    }
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
        return jump_to(ch);
    return 0;
}

void view_rename_start(void) {
    if (picked < 0)
        return;
    editing    = 1;
    edit_fresh = 1;
    strcpy(edit, items[picked].name);
    show(picked);
}

int view_renaming(void) {
    return editing;
}

const char *view_rename_name(void) {
    return edit;
}

void view_rename_end(void) {
    editing = 0;
}
