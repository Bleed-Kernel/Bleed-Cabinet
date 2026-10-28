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
    uint8_t           chosen;       // part of the selection
    uint8_t           cut;          // on the clipboard to be moved, drawn faded
    uint8_t           kept;         // chosen before a ctrl drag box started, it stays chosen
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

enum {
    LOOK_PLAIN,
    LOOK_PICKED,
    LOOK_GHOST,
};

// drawn over whatevers there
void icon_draw(ui_canvas_t *c, const ui_image_t *img, int x, int y, int look);

// the drive pictures on the home page, 32px
enum {
    HOME_DRIVE,
    HOME_USB,
    HOME_MEMORY,
    HOME_ICONS,
};

const ui_image_t *home_icon(int which);

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
#define VIEW_DROP       (1 << 6)    // the chosen ones were dragged onto view_drop_target
#define VIEW_DROP_OUT   (1 << 7)    // or let go outside the view, at view_drop_point

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

// the one with the focus, -1 for none. its usually chosen but ctrl click can unchoose it
int  view_picked(void);

// only i chosen, -1 for nothing
void view_pick(int i);
void view_pick_all(void);

// the focus moves to i
void view_focus(int i);
int  view_chosen_count(void);

// full paths of everything chosen
char **view_chosen_paths(int *n);

// adds name to whats chosen
void view_choose_name(const char *name);

// a drag is going, and what its over that would take it. -1 for nothing
int  view_dragging(void);
int  view_drop_target(void);
void view_drop_point(int *x, int *y);

// reads the clipboard again to see whats been cut
void view_mark_cut(void);

// puts the icons back in rows in order
void view_line_up(void);

// the folder changed under us
int  view_poll(void);

// whats under x y, -1 for empty space. -2 if its not in the list at all
int  view_at(int x, int y);
void view_pick_name(const char *name);
void view_entry_path(int i, char *out, size_t size);

// what the type column says, File Folder, PNG Image..
void kind_name(const entry_t *e, char *out, size_t size);

void view_draw(ui_canvas_t *c);
int  view_mouse(int x, int y, int left, uint8_t keymod, uint64_t now_ms);
int  view_wheel(int notches);
int  view_key(uint16_t keycode, char ch, uint8_t keymod);

// naming in place: the picked entrys label turns into a box to type in
void view_rename_start(void);
int  view_renaming(void);
const char *view_rename_name(void);
void view_rename_end(void);

// remember where a file was put in the grid, so the folder looks the same next time
void view_keep_spot(const char *path, int col, int row);

// home.c, my computer: shortcuts and every mounted drive with how full it is

#define HOME_PATH   "My Computer"

// reads the drives again, keeps whatever was picked if its still there
void home_open(void);
void home_layout(ui_rect_t r);
void home_draw(ui_canvas_t *c);

int  home_count(void);
int  home_picked(void);
void home_pick(int i);
void home_pick_path(const char *p);
const char *home_path(int i);
int  home_is_drive(int i);
void home_space(int i, uint64_t *total, uint64_t *free);

// same as view_at, -1 for empty space and -2 off the list
int  home_at(int x, int y);
int  home_mouse(int x, int y, int left, uint64_t now_ms);
int  home_wheel(int notches);
int  home_key(uint16_t keycode);

// drives come and go and fill up, nonzero if it needs drawing again
int  home_poll(void);

// ops.c, copying moving and deleting with a progress box, and the clipboard

#define CLIP_FILE   "/tmp/.cabinet-clip"
#define DROP_FILE   "/tmp/.cabinet-drop"
#define DROP_HEAD   "cabinet"

enum {
    OP_COPY,
    OP_MOVE,
};

// paths into dir, asking before anything is replaced
int  ops_transfer(char **paths, int n, const char *dir, int op);
const char *ops_made(int i);

// files and folders with everything in them, how many went
int  ops_delete(char **paths, int n);
int  ops_remove(const char *p);

// bytes in p and everything under it, files and folders are counted if theyre not NULL
uint64_t ops_measure(const char *p, int *files, int *folders);
int  ops_same_drive(const char *a, const char *b);

int    list_write(const char *file, const char *head, char **paths, int n);
char **list_read(const char *file, char *head, size_t head_size, int *n);
void   list_free(char **paths, int n);

void   clip_set(char **paths, int n, int cut);
char **clip_get(int *n, int *cut);
void   clip_clear(void);

void size_text(uint64_t size, char *out, size_t len);
uint64_t now_ms(void);
