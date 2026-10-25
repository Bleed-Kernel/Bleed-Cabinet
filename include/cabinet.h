#pragma once

#include <stdint.h>
#include <stddef.h>
#include <bleed_uikit.h>

#define CAB_PATH_MAX    UI_PATH_MAX
#define CAB_NAME_MAX    UI_NAME_MAX

#define COLOUR_FOLDER    0xE0B850   // 9x folders were yellow and thats staying
#define COLOUR_FOLDER_HI 0xF8E0A0
#define COLOUR_FOLDER_LO 0x9A7A28
#define COLOUR_PAGE      0xD8D8DC
#define COLOUR_VIEW      UI_COLOUR_BACK_L

#define PROGRAM_PLAYER   "/initrd/bin/playback"
#define PROGRAM_EDITOR   "/initrd/bin/tvi"
#define PROGRAM_VIEWER   "/initrd/bin/exhibit"

// what a file is, picked from its name and failing that its first few bytes
enum {
    KIND_FOLDER,
    KIND_PROGRAM,
    KIND_TEXT,
    KIND_AUDIO,
    KIND_IMAGE,
    KIND_FILE,
    KIND_DEVICE,                // never opened, reading one can wait forever
    KINDS,
};

typedef struct {
    char              name[CAB_NAME_MAX];
    int               kind;
    uint64_t          size;
    int               col, row;     // its grid cell in the icon view, -1 until its placed
    const ui_image_t *big;
    const ui_image_t *small;
} entry_t;

// icons.c

#define ICON_BIG    32
#define ICON_SMALL  16

void icons_init(void);

// a programs own icon if path is one that has it, otherwise the one for its kind
const ui_image_t *icon_for(const char *path, int kind, int px);

// the toolbars pictures, 16px
enum {
    GLYPH_BACK,
    GLYPH_FORWARD,
    GLYPH_UP,
    GLYPH_REFRESH,
    GLYPH_NEW_FOLDER,
    GLYPH_DELETE,
    GLYPHS,
};

const ui_image_t *glyph(int which);

// drawn over whatevers there, picked ones go dark oxblood like 9x washed them blue
void icon_draw(ui_canvas_t *c, const ui_image_t *img, int x, int y, int picked);

// view.c

enum {
    VIEW_ICONS,
    VIEW_DETAILS,
};

#define VIEW_REDRAW     (1 << 0)
#define VIEW_OPEN       (1 << 1)    // the picked entry was double clicked or entered
#define VIEW_STATUS     (1 << 2)    // the selection or the listing changed
#define VIEW_UP         (1 << 3)    // backspace, go to the folder above
#define VIEW_DELETE     (1 << 4)
#define VIEW_RENAMED    (1 << 5)    // view_rename_name has the new name, the old one is still picked

// reads path in, -1 if its not a folder that can be read and nothing changes
int  view_open(const char *path);

void view_show_hidden(int on);
int  view_hidden_shown(void);
void view_refresh(void);
const char *view_path(void);

void view_layout(ui_rect_t r);
void view_mode(int mode);
int  view_get_mode(void);

int  view_count(void);
const entry_t *view_entry(int i);
int  view_picked(void);
void view_pick(int i);

// whats under x y, -1 for empty space. -2 if its not in the list at all
int  view_at(int x, int y);
void view_pick_name(const char *name);
void view_entry_path(int i, char *out, size_t size);

void view_draw(ui_canvas_t *c);
int  view_mouse(int x, int y, int left, uint64_t now_ms);
int  view_wheel(int notches);
int  view_key(uint16_t keycode, char ch);

// naming in place: the picked entrys label turns into a box to type in
void view_rename_start(void);
int  view_renaming(void);
const char *view_rename_name(void);
void view_rename_end(void);

// remember where a file was put in the grid, so the folder looks the same next time
void view_keep_spot(const char *path, int col, int row);
