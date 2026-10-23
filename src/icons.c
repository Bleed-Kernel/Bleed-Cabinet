#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <syscalls.h>
#include <crime/crime.h>
#include <cabinet.h>

#define DESKTOP_PATH    "/initrd/bin/crime-scene"
#define CACHE_MAX       128

typedef struct {
    char       path[CAB_PATH_MAX];
    int        px;
    int        found;
    ui_image_t img;
} cached_t;

// sizes a program might carry instead of a master, biggest first
static const int sizes[] = { 48, 32, 16 };
#define SIZES   (int)(sizeof(sizes) / sizeof(sizes[0]))

static ui_image_t kinds[KINDS][2];  // big then small
static ui_image_t glyphs[GLYPHS];
static cached_t   cache[CACHE_MAX];
static int        cached;

// cabinets own pictures are pngs in res/icons, built into the binary as .bleed.cab.<file name>
static const char *kind_files[KINDS] = { "folder", "program", "text", "audio", "image", "file", "device" };
static const char *glyph_files[GLYPHS] = { "back", "forward", "up", "refresh", "newfolder", "delete" };

static char self[CAB_PATH_MAX];

static int own_load(const char *name, ui_image_t *out) {
    char section[64];
    snprintf(section, sizeof(section), ".bleed.cab.%s", name);

    size_t   size;
    uint8_t *png = cs_res_read(self, section, &size);
    if (!png)
        return -1;
    int r = ui_image_decode(out, png, size);
    free(png);
    return r;
}

// shrunk to fit px keeping its shape, never blown up past what it was drawn at
static void fit(ui_image_t *img, int px) {
    if (img->w <= px && img->h <= px)
        return;
    int w = img->w >= img->h ? px : (img->w * px + img->h / 2) / img->h;
    int h = img->h >= img->w ? px : (img->h * px + img->w / 2) / img->w;
    ui_image_scale(img, w ? w : 1, h ? h : 1);
}

// one picture per kind, both sizes are scaled down from it
static void kind_load(int kind) {
    ui_image_t master;
    if (own_load(kind_files[kind], &master) != 0)
        return;

    ui_image_t *small = &kinds[kind][1];
    size_t      bytes = (size_t)master.w * master.h * sizeof(uint32_t);
    if ((small->pixels = malloc(bytes))) {
        memcpy(small->pixels, master.pixels, bytes);
        small->w = master.w;
        small->h = master.h;
        fit(small, ICON_SMALL);
    }

    kinds[kind][0] = master;
    fit(&kinds[kind][0], ICON_BIG);
}

static int res_decode(const char *path, const char *base, int px, ui_image_t *out) {
    char name[32];
    if (px)
        snprintf(name, sizeof(name), "%s%d", base, px);
    else
        snprintf(name, sizeof(name), "%s", base);

    size_t   size;
    uint8_t *png = cs_res_read(path, name, &size);
    if (!png)
        return -1;
    int r = ui_image_decode(out, png, size);
    free(png);
    return r;
}

// the master scaled down, a program without one gets its biggest sized one instead
static int res_icon(const char *path, const char *base, int px, ui_image_t *out) {
    int found = res_decode(path, base, 0, out) == 0;
    for (int i = 0; !found && i < SIZES; i++)
        found = res_decode(path, base, sizes[i], out) == 0;
    if (!found)
        return -1;

    fit(out, px);
    return 0;
}

static const ui_image_t *cached_icon(const char *path, const char *base, int px) {
    for (int i = 0; i < cached; i++) {
        if (cache[i].px == px && strcmp(cache[i].path, path) == 0)
            return cache[i].found ? &cache[i].img : NULL;
    }
    if (cached == CACHE_MAX || strlen(path) >= CAB_PATH_MAX)
        return NULL;

    cached_t *c = &cache[cached++];
    strcpy(c->path, path);
    c->px    = px;
    c->found = res_icon(path, base, px, &c->img) == 0;
    return c->found ? &c->img : NULL;
}

const ui_image_t *glyph(int which) {
    return which >= 0 && which < GLYPHS ? &glyphs[which] : NULL;
}

void icons_init(void) {
    task_path((uint64_t)_getpid(), self, sizeof(self));
    for (int k = 0; k < KINDS; k++)
        kind_load(k);

    char name[32];
    for (int g = 0; g < GLYPHS; g++) {
        snprintf(name, sizeof(name), "tool-%s", glyph_files[g]);
        own_load(name, &glyphs[g]);
    }
}

const ui_image_t *icon_for(const char *path, int kind, int px) {
    if (kind == KIND_PROGRAM) {
        const ui_image_t *own = cached_icon(path, CS_RES_ICON, px);
        if (own)
            return own;

        // the desktop keeps the standard one 
        char key[CAB_PATH_MAX];
        snprintf(key, sizeof(key), "%s%s", DESKTOP_PATH, CS_RES_DEFAULT);
        for (int i = 0; i < cached; i++) {
            if (cache[i].px == px && strcmp(cache[i].path, key) == 0)
                return cache[i].found ? &cache[i].img : &kinds[kind][px <= ICON_SMALL];
        }
        if (cached < CACHE_MAX) {
            cached_t *c = &cache[cached++];
            strcpy(c->path, key);
            c->px    = px;
            c->found = res_icon(DESKTOP_PATH, CS_RES_DEFAULT, px, &c->img) == 0;
            if (c->found)
                return &c->img;
        }
    }
    return &kinds[kind][px <= ICON_SMALL];
}

void icon_draw(ui_canvas_t *c, const ui_image_t *img, int x, int y, int picked) {
    if (!img || !img->pixels)
        return;
    ui_image_draw(c, img, x, y);
    if (!picked)
        return;

    ui_rect_t r = ui_rect_clip((ui_rect_t){x, y, img->w, img->h}, c->clip);
    for (int row = r.y; row < r.y + r.h; row++) {
        const uint32_t *src = img->pixels + (size_t)(row - y) * img->w + (r.x - x);
        uint32_t       *dst = c->pixels + (size_t)row * c->pitch + r.x;
        for (int col = 0; col < r.w; col++) {
            if ((src[col] >> 24) < 128)
                continue;
            uint32_t p = dst[col];
            uint32_t red = (((p >> 16) & 0xFF) + 0x4A) / 2;
            uint32_t grn = (((p >> 8) & 0xFF) + 0x04) / 2;
            uint32_t blu = ((p & 0xFF) + 0x04) / 2;
            dst[col] = red << 16 | grn << 8 | blu;
        }
    }
}
