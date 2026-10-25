#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <syscalls.h>
#include <sysinfo.h>
#include <keyboard.h>
#include <usbstorage.h>
#include <cabinet.h>

#define PLACES_MAX      32
#define MOUNTS_MAX      16
#define HEAD_H          26
#define GROUP_GAP       10
#define TILE_GAP        6
#define FOLDER_W        170
#define FOLDER_H        44
#define DRIVE_W         250
#define DRIVE_H         58
#define BAR_H           12
#define SCROLL_STEP     16
#define DOUBLE_CLICK_MS 400

#define KEY_ENTER       0x1C

#define COLOUR_DIM      UI_COLOUR_BACK_TEXT

typedef struct {
    char              name[CAB_NAME_MAX];
    char              path[CAB_PATH_MAX];
    int               drive;
    uint64_t          total, free;
    const ui_image_t *icon;
    ui_rect_t         rect;
} place_t;

// shortcuts shown above the drives, the ones that arent there get left out
static const char *const shortcuts[][2] = {
    { "bin",     "/initrd/bin" },
    { "lib",     "/initrd/lib" },
    { "include", "/initrd/include" },
    { "dev",     "/dev" },
};
#define SHORTCUTS   (int)(sizeof(shortcuts) / sizeof(shortcuts[0]))

static place_t        places[PLACES_MAX];
static int            count;
static int            folders;
static int            picked = -1;
static ui_rect_t      frame, inner;
static ui_scrollbar_t bar;
static int            content_h;
static int            heads[2];
static int            last_click = -1;
static uint64_t       last_click_ms;
static uint8_t        was_left;

static int exists(const char *p) {
    int fd = _open(p, O_RDONLY);
    if (fd < 0)
        return 0;
    _close(fd);
    return 1;
}

static int usb_product(const char *device, char *out, size_t size) {
    char  dev[32];
    char *p;
    snprintf(dev, sizeof(dev), "%s", device);
    if ((p = strrchr(dev, 'p')) && p > dev + 8)
        *p = '\0';

    int fd = _open(dev, O_RDONLY);
    if (fd < 0)
        return -1;
    struct usb_storage_info info;
    long r = _ioctl(fd, USB_STORAGE_IOC_INFO, &info);
    _close(fd);
    if (r != 0 || !info.product[0])
        return -1;

    snprintf(out, size, "%s", info.product);
    for (size_t n = strlen(out); n && out[n - 1] == ' '; n--)
        out[n - 1] = '\0';
    return out[0] ? 0 : -1;
}

static const char *drive_kind(const user_mount_info_t *m, int *icon, char *usb, size_t size) {
    const char *dev = m->device;
    *icon = HOME_DRIVE;

    if (!strcmp(m->path, "/")) {
        *icon = HOME_MEMORY;
        return "Memory";
    }
    if (!strncmp(dev, "/dev/usb", 8)) {
        *icon = HOME_USB;
        return usb_product(dev, usb, size) == 0 ? usb : "USB Drive";
    }
    if (!strncmp(dev, "/dev/nvme", 9))
        return "NVMe Disk";
    if (!strncmp(dev, "/dev/sd", 7))
        return "SATA Disk";
    if (!strncmp(dev, "/dev/hd", 7))
        return "IDE Disk";
    if (!dev[0] && !strcmp(m->path, "/mnt/data"))
        return "Bleed Data";
    return dev[0] ? "Local Disk" : "Disk Image";
}

static void read_places(void) {
    count = 0;
    for (int i = 0; i < SHORTCUTS && count < PLACES_MAX; i++) {
        if (!exists(shortcuts[i][1]))
            continue;
        place_t *p = &places[count++];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "%s", shortcuts[i][0]);
        snprintf(p->path, sizeof(p->path), "%s", shortcuts[i][1]);
        p->icon = icon_for(p->path, KIND_FOLDER, ICON_BIG);
    }
    folders = count;

    user_mount_info_t m;
    for (int i = 0; i < MOUNTS_MAX && count < PLACES_MAX && _mountinfo(i, &m) == 0; i++) {
        char     usb[32], label[64];
        int      icon;
        place_t *p = &places[count++];
        memset(p, 0, sizeof(*p));

        const char *kind = drive_kind(&m, &icon, usb, sizeof(usb));
        if (_mountlabel(i, label, sizeof(label)) == 0 && label[0])
            kind = label;
        snprintf(p->name, sizeof(p->name), "%s (%s)", kind, m.path);
        snprintf(p->path, sizeof(p->path), "%s", m.path);
        p->drive = 1;
        p->total = m.blocks_total * m.block_size;
        p->free  = m.blocks_free * m.block_size;
        p->icon  = home_icon(icon);
    }
}

static void place(void) {
    int y = 0;
    for (int g = 0; g < 2; g++) {
        int from = g ? folders : 0;
        int to   = g ? count : folders;
        int w    = g ? DRIVE_W : FOLDER_W;
        int h    = g ? DRIVE_H : FOLDER_H;
        if (from == to) {
            heads[g] = -1;
            continue;
        }

        heads[g] = y;
        y += HEAD_H;
        int x = TILE_GAP;
        for (int i = from; i < to; i++) {
            if (x > TILE_GAP && x + w > inner.w) {
                x = TILE_GAP;
                y += h + TILE_GAP;
            }
            places[i].rect = (ui_rect_t){ x, y, w, h };
            x += w + TILE_GAP;
        }
        y += h + GROUP_GAP;
    }
    content_h = y;
}

static void bar_update(void) {
    bar.total   = (content_h + SCROLL_STEP - 1) / SCROLL_STEP;
    bar.visible = inner.h / SCROLL_STEP;
    if (bar.visible < 1)
        bar.visible = 1;
    ui_scrollbar_to(&bar, bar.top);
}

static ui_rect_t on_screen(ui_rect_t r) {
    return (ui_rect_t){ inner.x + r.x, inner.y + r.y - bar.top * SCROLL_STEP, r.w, r.h };
}

static void show(int i) {
    if (i < 0)
        return;
    ui_rect_t r   = places[i].rect;
    int       top = bar.top * SCROLL_STEP;
    if (r.y < top)
        ui_scrollbar_to(&bar, r.y / SCROLL_STEP);
    else if (r.y + r.h > top + inner.h)
        ui_scrollbar_to(&bar, (r.y + r.h - inner.h + SCROLL_STEP - 1) / SCROLL_STEP);
}

void home_open(void) {
    char was[CAB_PATH_MAX] = "";
    if (picked >= 0)
        strcpy(was, places[picked].path);

    read_places();
    picked = -1;
    for (int i = 0; was[0] && i < count; i++) {
        if (!strcmp(places[i].path, was))
            picked = i;
    }
    place();
    bar_update();
}

void home_layout(ui_rect_t r) {
    frame    = r;
    inner    = ui_rect_shrink(r, 2);
    inner.w -= UI_SCROLL_W;
    bar.rect = (ui_rect_t){ inner.x + inner.w, inner.y, UI_SCROLL_W, inner.h };
    place();
    bar_update();
    show(picked);
}

int home_count(void) {
    return count;
}

int home_picked(void) {
    return picked;
}

const char *home_path(int i) {
    return i >= 0 && i < count ? places[i].path : NULL;
}

int home_is_drive(int i) {
    return i >= 0 && i < count && places[i].drive;
}

void home_space(int i, uint64_t *total, uint64_t *free) {
    *total = places[i].total;
    *free  = places[i].free;
}

static void heading(ui_canvas_t *c, int g) {
    static const char *names[2] = { "Folders", "Devices and drives" };
    char text[48];
    snprintf(text, sizeof(text), "%s (%d)", names[g], g ? count - folders : folders);

    int y  = inner.y + heads[g] - bar.top * SCROLL_STEP;
    int lh = ui_font_height(&ui_font_text);
    int tw = ui_text_width(&ui_font_text, text);
    ui_text(c, &ui_font_text, inner.x + TILE_GAP, y + (HEAD_H - lh) / 2, text, UI_COLOUR_TEXT);

    int gx = inner.x + TILE_GAP * 2 + tw;
    if (gx < inner.x + inner.w - TILE_GAP)
        ui_separator(c, gx, y + HEAD_H / 2, inner.x + inner.w - TILE_GAP - gx);
}

static void tile_draw(ui_canvas_t *c, int i) {
    const place_t *p  = &places[i];
    ui_rect_t      r  = on_screen(p->rect);
    int            on = i == picked;
    int            lh = ui_font_height(&ui_font_text);

    if (on)
        ui_edge(c, r, UI_COLOUR_CAPTION_R, UI_COLOUR_CAPTION_R);

    int ix = r.x + 6;
    int iy = r.y + (r.h - ICON_BIG) / 2;
    if (p->icon)
        icon_draw(c, p->icon, ix + (ICON_BIG - p->icon->w) / 2, iy + (ICON_BIG - p->icon->h) / 2, on);

    int       tx   = ix + ICON_BIG + 8;
    int       tw   = r.x + r.w - 6 - tx;
    char      name[CAB_NAME_MAX];
    ui_text_ascii(name, sizeof(name), p->name);

    if (!p->drive) {
        int ty = r.y + (r.h - lh * 2) / 2;
        int nw = ui_text_width(&ui_font_text, name);
        if (on)
            ui_fill(c, (ui_rect_t){ tx - 2, ty, (nw < tw ? nw : tw) + 4, lh }, UI_COLOUR_CAPTION_R);
        ui_text_in(c, &ui_font_text, (ui_rect_t){ tx, ty, tw, lh }, name, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
        ui_text_in(c, &ui_font_text, (ui_rect_t){ tx, ty + lh, tw, lh }, p->path, COLOUR_DIM, UI_ALIGN_LEFT);
        return;
    }

    int ty = r.y + (r.h - lh * 2 - BAR_H - 4) / 2;
    int nw = ui_text_width(&ui_font_text, name);
    if (on)
        ui_fill(c, (ui_rect_t){ tx - 2, ty, (nw < tw ? nw : tw) + 4, lh }, UI_COLOUR_CAPTION_R);
    ui_text_in(c, &ui_font_text, (ui_rect_t){ tx, ty, tw, lh }, name, UI_COLOUR_TEXT, UI_ALIGN_LEFT);

    int used = p->total ? (int)((p->total - p->free) * 1000 / p->total) : 0;
    ui_progress(c, (ui_rect_t){ tx, ty + lh + 2, tw, BAR_H }, used);

    char space[64], f[24], t[24];
    if (p->total) {
        size_text(p->free, f, sizeof(f));
        size_text(p->total, t, sizeof(t));
        snprintf(space, sizeof(space), "%s free of %s", f, t);
    } else {
        snprintf(space, sizeof(space), "Size unknown");
    }
    ui_text_in(c, &ui_font_text, (ui_rect_t){ tx, ty + lh + BAR_H + 4, tw, lh }, space, COLOUR_DIM, UI_ALIGN_LEFT);
}

void home_draw(ui_canvas_t *c) {
    ui_panel(c, frame, UI_SUNKEN);
    ui_fill(c, inner, COLOUR_VIEW);

    ui_rect_t old = c->clip;
    ui_clip(c, ui_rect_clip(inner, old));
    for (int g = 0; g < 2; g++) {
        if (heads[g] >= 0)
            heading(c, g);
    }
    for (int i = 0; i < count; i++)
        tile_draw(c, i);
    c->clip = old;

    if (bar.total > bar.visible)
        ui_scrollbar_draw(c, &bar);
    else
        ui_fill(c, bar.rect, COLOUR_VIEW);
}

static int item_at(int x, int y) {
    for (int i = 0; i < count; i++) {
        if (ui_rect_contains(on_screen(places[i].rect), x, y))
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

void home_pick(int i) {
    pick(i >= 0 && i < count ? i : -1);
}

void home_pick_path(const char *p) {
    for (int i = 0; i < count; i++) {
        if (!strcmp(places[i].path, p)) {
            pick(i);
            return;
        }
    }
}

int home_at(int x, int y) {
    if (!ui_rect_contains(inner, x, y))
        return -2;
    return item_at(x, y);
}

int home_mouse(int x, int y, int left, uint64_t now_ms) {
    int press = left && !was_left;
    was_left  = (uint8_t)left;

    if (bar.total > bar.visible) {
        int r = ui_scrollbar_mouse(&bar, x, y, left);
        if (r & UI_TAKEN)
            return (r & UI_CHANGED) ? VIEW_REDRAW : 0;
    }
    if (!press || !ui_rect_contains(inner, x, y))
        return 0;

    int i = item_at(x, y);
    int r = pick(i);
    if (i >= 0 && i == last_click && now_ms - last_click_ms < DOUBLE_CLICK_MS) {
        last_click = -1;
        return r | VIEW_OPEN;
    }
    last_click    = i;
    last_click_ms = now_ms;
    return r | VIEW_REDRAW;
}

int home_wheel(int notches) {
    int top = bar.top;
    ui_scrollbar_to(&bar, bar.top - notches * 3);
    return bar.top != top ? VIEW_REDRAW : 0;
}

static int neighbour(int dx, int dy) {
    if (picked < 0)
        return count ? 0 : -1;

    ui_rect_t from = places[picked].rect;
    int       best = -1;
    long      score = 0;
    for (int i = 0; i < count; i++) {
        ui_rect_t to     = places[i].rect;
        long      cx     = (to.x + to.w / 2) - (from.x + from.w / 2);
        long      cy     = (to.y + to.h / 2) - (from.y + from.h / 2);
        long      along  = dx ? cx * dx : cy * dy;
        long      across = dx ? (cy < 0 ? -cy : cy) : (cx < 0 ? -cx : cx);
        if (i == picked || along <= 0)
            continue;
        long s = along + across * 4;
        if (best < 0 || s < score) {
            best  = i;
            score = s;
        }
    }
    return best >= 0 ? best : picked;
}

int home_key(uint16_t keycode) {
    switch (keycode) {
    case ArrowUp:    return pick(neighbour(0, -1));
    case ArrowDown:  return pick(neighbour(0, 1));
    case ArrowLeft:  return pick(neighbour(-1, 0));
    case ArrowRight: return pick(neighbour(1, 0));
    case Home:       return pick(count ? 0 : -1);
    case End:        return pick(count - 1);
    case KEY_ENTER:  return picked >= 0 ? VIEW_OPEN : 0;
    case F5:
        home_open();
        return VIEW_REDRAW | VIEW_STATUS;
    }
    return 0;
}

// the drives agai
int home_poll(void) {
    static place_t was[PLACES_MAX];
    int            was_count = count;
    memcpy(was, places, sizeof(place_t) * (size_t)count);

    home_open();
    if (count != was_count)
        return 1;
    for (int i = 0; i < count; i++) {
        if (strcmp(was[i].path, places[i].path) || was[i].free != places[i].free || was[i].total != places[i].total)
            return 1;
    }
    return 0;
}
