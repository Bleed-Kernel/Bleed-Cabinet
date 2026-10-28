#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <syscalls.h>
#include <keyboard.h>
#include <crime/crime.h>
#include <cabinet.h>

#define CHUNK           (64 * 1024)
#define PROGRESS_AFTER  500     // ms before the box comes up, quick ones never show it
#define PROGRESS_W      380
#define PROGRESS_H      138
#define PROGRESS_EVERY  50      // ms between redraws of it
#define CANCEL_W        84
#define CANCEL_H        24
#define MADE_MAX        256
#define MOUNTS_MAX      16
#define KEY_ESCAPE      0x01

enum {
    ASK_YES,
    ASK_ALL,
    ASK_NO,
    ASK_CANCEL,
};

enum {
    IS_MISSING = -1,
    IS_FILE,
    IS_FOLDER,
    IS_DEVICE,
};

// one copy, move or delete, however many files it takes
typedef struct {
    const char  *verb;
    const char  *error;         // the title on whatever goes wrong
    cs_window_t *win;
    ui_button_t  cancel;
    uint64_t     started;
    uint64_t     drawn;
    uint64_t     done, total;   // bytes, or files when deleting
    char         name[CAB_NAME_MAX];
    char         where[CAB_PATH_MAX];
    int          stop;          // cancelled, or something went wrong and the rest wont happen
    int          replace_all;
    int          by_files;
    int          measured;
} job_t;

static job_t    job;
static uint8_t *buf;

static char     made[MADE_MAX][CAB_NAME_MAX];
static int      made_count;

static void join(const char *dir, const char *name, char *out, size_t size) {
    snprintf(out, size, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

static const char *base_of(const char *p) {
    const char *slash = strrchr(p, '/');
    return slash && slash[1] ? slash + 1 : p;
}

static void parent_of(const char *p, char *out, size_t size) {
    snprintf(out, size, "%s", p);
    char *slash = strrchr(out, '/');
    if (slash == out)
        out[1] = '\0';
    else if (slash)
        *slash = '\0';
}

// b is somewhere under a
static int inside(const char *a, const char *b) {
    size_t len = strlen(a);
    return !strncmp(a, b, len) && (b[len] == '/' || (len == 1 && b[1]));
}

static int what_is(const char *p, uint64_t *size) {
    struct stat st;
    if (stat(p, &st) < 0)
        return IS_MISSING;
    if (size)
        *size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
    if (S_ISDIR(st.st_mode))
        return IS_FOLDER;
    return S_ISREG(st.st_mode) ? IS_FILE : IS_DEVICE;
}

static char **names_in(const char *p, int *n) {
    *n = 0;
    int fd = _open(p, O_RDONLY);
    if (fd < 0)
        return NULL;

    char   **names = NULL;
    dirent_t entry;
    for (size_t i = 0; _readdir(fd, i, &entry) == 0; i++) {
        entry.name[sizeof(entry.name) - 1] = '\0';
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, "..") || !entry.name[0])
            continue;
        char **more = realloc(names, (size_t)(*n + 1) * sizeof(char *));
        if (!more || !(more[*n] = strdup(entry.name))) {
            names = more ? more : names;
            break;
        }
        names = more;
        (*n)++;
    }
    _close(fd);
    return names;
}

void list_free(char **paths, int n) {
    for (int i = 0; i < n; i++)
        free(paths[i]);
    free(paths);
}

// progress

uint64_t ops_measure(const char *p, int *files, int *folders) {
    uint64_t size = 0;
    int      is   = what_is(p, &size);
    if (is != IS_FOLDER) {
        if (files && is != IS_MISSING)
            (*files)++;
        return size;
    }
    if (folders)
        (*folders)++;

    int    n;
    char **names = names_in(p, &n);
    char   child[CAB_PATH_MAX];
    for (int i = 0; i < n; i++) {
        join(p, names[i], child, sizeof(child));
        size += ops_measure(child, files, folders);
    }
    list_free(names, n);
    return size;
}

static void job_draw(void) {
    cs_window_t *w = job.win;
    ui_canvas_t  c = ui_canvas(w->pixels, w->width, w->height, w->pitch);
    ui_fill(&c, (ui_rect_t){ 0, 0, w->width, w->height }, UI_COLOUR_FACE);

    int  x = 14, iw = w->width - 28;
    int  permille = job.total ? (int)(job.done * 1000 / job.total) : 0;
    char text[CAB_PATH_MAX + 16];

    ui_clip(&c, (ui_rect_t){ x, 0, iw, w->height });
    ui_text_ascii(text, sizeof(text), job.name);
    ui_text_in(&c, &ui_font_text, (ui_rect_t){ x, 12, iw, 18 }, text, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
    ui_text_ascii(text, sizeof(text), job.where);
    ui_text_in(&c, &ui_font_text, (ui_rect_t){ x, 32, iw, 18 }, text, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
    ui_unclip(&c);

    ui_progress(&c, (ui_rect_t){ x, 58, iw, 18 }, permille > 1000 ? 1000 : permille);

    // the guess only means anything once its been going a second or so
    uint64_t took = now_ms() - job.started;
    if (job.done && job.total > job.done && took > 1000) {
        uint64_t left = took * (job.total - job.done) / job.done / 1000 + 1;
        if (left < 60)
            snprintf(text, sizeof(text), "%lu Second%s Remaining", (unsigned long)left, left == 1 ? "" : "s");
        else
            snprintf(text, sizeof(text), "%lu Minute%s Remaining", (unsigned long)(left / 60),
                     left / 60 == 1 ? "" : "s");
        ui_text_in(&c, &ui_font_text, (ui_rect_t){ x, 80, iw, 18 }, text, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
    }

    ui_button_draw(&c, &job.cancel);
    cs_window_commit(w, 0, 0, w->width, w->height);
}

// called as the work goes, puts the box up once its been a while and sees if cancel was hit
static void job_tick(void) {
    uint64_t now = now_ms();
    if (!job.win && now - job.started >= PROGRESS_AFTER) {
        job.win = cs_window_open(PROGRESS_W, PROGRESS_H, job.verb);
        if (!job.win) {
            job.started = now;
            return;
        }
        job.cancel.label = "Cancel";
        job.cancel.rect  = (ui_rect_t){ job.win->width - 14 - CANCEL_W, job.win->height - 12 - CANCEL_H,
                                        CANCEL_W, CANCEL_H };
        job.drawn = 0;
    }
    if (!job.win)
        return;

    int        draw = 0;
    cs_event_t ev;
    while (cs_event_wait(&ev, 0) > 0) {
        if (ev.window != job.win->id)
            continue;
        switch (ev.type) {
        case CS_EV_CLOSE_REQUEST:
            job.stop = 1;
            break;
        case CS_EV_KEY:
            if (ev.key_action == KEY_DOWN && ev.keycode == KEY_ESCAPE)
                job.stop = 1;
            break;
        case CS_EV_MOUSE: {
            int r = ui_button_mouse(&job.cancel, ev.x, ev.y, (ev.buttons & CS_BUTTON_LEFT) != 0);
            if (r & UI_CLICKED)
                job.stop = 1;
            draw |= r != 0;
            break;
        }
        case CS_EV_RESIZE:
            draw = 1;
            break;
        }
    }
    if (draw || now - job.drawn >= PROGRESS_EVERY) {
        job_draw();
        job.drawn = now;
    }
}

static void job_begin(const char *verb, const char *error) {
    memset(&job, 0, sizeof(job));
    job.verb    = verb;
    job.error   = error;
    job.started = now_ms();
    made_count  = 0;
}

static void job_end(void) {
    if (job.win)
        cs_window_close(job.win);
    job.win = NULL;
}

static void job_at(const char *name, const char *from, const char *to) {
    snprintf(job.name, sizeof(job.name), "%s", name);
    if (to)
        snprintf(job.where, sizeof(job.where), "From '%s' to '%s'", base_of(from), base_of(to));
    else
        snprintf(job.where, sizeof(job.where), "From '%s'", base_of(from));
}

// goes wrong once and the rest is called off, same as 9x
static void oops(const char *what, const char *name, const char *why) {
    char text[CAB_NAME_MAX + 160];
    snprintf(text, sizeof(text), "Cannot %s '%s': %s", what, name, why);
    ui_message(job.error, text, UI_ICON_ERROR);
    job.stop = 1;
}

// the work

int ops_remove(const char *p) {
    int is = what_is(p, NULL);
    if (is == IS_MISSING)
        return -1;

    int status = 0;
    if (is == IS_FOLDER) {
        int    n;
        char **names = names_in(p, &n);
        char   child[CAB_PATH_MAX];
        for (int i = 0; i < n && !job.stop; i++) {
            join(p, names[i], child, sizeof(child));
            status |= ops_remove(child);
        }
        list_free(names, n);
        if (job.stop)
            return -1;
    }
    if (_unlink(p) < 0)
        return -1;
    job.done += job.by_files;
    job_tick();
    return status;
}

static int copy_file(const char *src, const char *dst) {
    char from[CAB_PATH_MAX], to[CAB_PATH_MAX];
    parent_of(src, from, sizeof(from));
    parent_of(dst, to, sizeof(to));
    job_at(base_of(src), from, to);

    int in = _open(src, O_RDONLY);
    if (in < 0) {
        oops("copy", base_of(src), "Cannot read from the source file or disk.");
        return -1;
    }
    int out = _open(dst, O_CREAT | O_WRONLY | O_TRUNC);
    if (out < 0) {
        _close(in);
        oops("create", base_of(dst), "The disk may be full or read only.");
        return -1;
    }
    if (!buf && !(buf = malloc(CHUNK))) {
        _close(in);
        _close(out);
        return -1;
    }

    int  status = 0;
    long n;
    while (!job.stop && (n = _read(in, buf, CHUNK)) > 0) {
        if (_write(out, buf, n) != n) {
            oops("copy", base_of(src), "There is not enough free disk space.");
            status = -1;
            break;
        }
        job.done += (uint64_t)n;
        job_tick();
    }
    _close(in);
    _close(out);

    // half a file is worse than none
    if (status < 0 || job.stop) {
        _unlink(dst);
        return -1;
    }
    return 0;
}

static int copy_tree(const char *src, const char *dst, int is) {
    if (is == IS_FILE)
        return copy_file(src, dst);
    if (is == IS_DEVICE) {
        oops("copy", base_of(src), "It is a device, not a file.");
        return -1;
    }

    if (_mkdir(dst, 0755) < 0 && what_is(dst, NULL) != IS_FOLDER) {
        oops("create", base_of(dst), "The disk may be full or read only.");
        return -1;
    }

    int    n, status = 0;
    char **names = names_in(src, &n);
    char   s[CAB_PATH_MAX], d[CAB_PATH_MAX];
    for (int i = 0; i < n && !job.stop && !status; i++) {
        join(src, names[i], s, sizeof(s));
        join(dst, names[i], d, sizeof(d));
        status = copy_tree(s, d, what_is(s, NULL));
    }
    list_free(names, n);
    return job.stop ? -1 : status;
}

static int ask_replace(const char *name, int folder) {
    char text[CAB_NAME_MAX + 200];
    if (folder)
        snprintf(text, sizeof(text), "This folder already contains a folder named '%s'.\n\n"
                 "Would you like to replace it, and everything in it?", name);
    else
        snprintf(text, sizeof(text), "This folder already contains a file named '%s'.\n\n"
                 "Would you like to replace the existing file?", name);

    ui_msgbox_t box = {
        .title          = folder ? "Confirm Folder Replace" : "Confirm File Replace",
        .text           = text,
        .icon           = UI_ICON_QUESTION,
        .buttons        = { "Yes", "Yes to All", "No", "Cancel" },
        .default_button = ASK_YES,
        .escape_button  = ASK_CANCEL,
    };
    int r = ui_msgbox(&box);
    return r < 0 ? ASK_CANCEL : r;
}

// a copy into the same folder its in gets a new name, like 9x does it
static void copy_name(const char *dir, const char *name, char *out, size_t size) {
    char want[CAB_NAME_MAX];
    snprintf(want, sizeof(want), "Copy of %s", name);
    join(dir, want, out, size);
    for (int n = 2; what_is(out, NULL) != IS_MISSING; n++) {
        snprintf(want, sizeof(want), "Copy (%d) of %s", n, name);
        join(dir, want, out, size);
    }
}

static void transfer(const char *src, const char *dir, int op) {
    const char *name = base_of(src);
    const char *what = op == OP_MOVE ? "move" : "copy";
    char        dst[CAB_PATH_MAX];
    int         is = what_is(src, NULL);

    if (is == IS_MISSING) {
        oops(what, name, "Cannot find the file. It may have been moved or deleted.");
        return;
    }
    join(dir, name, dst, sizeof(dst));

    if (!strcmp(src, dst)) {
        if (op == OP_MOVE)
            return;
        copy_name(dir, name, dst, sizeof(dst));
    } else if (inside(src, dst)) {
        oops(what, name, "The destination folder is a subfolder of the source folder.");
        return;
    } else {
        int there = what_is(dst, NULL);
        if (there != IS_MISSING && inside(dst, src)) {
            oops(what, name, "The file is inside the one it would replace.");
            return;
        }
        if (there != IS_MISSING) {
            if (!job.replace_all) {
                int a = ask_replace(name, there == IS_FOLDER);
                if (a == ASK_CANCEL)
                    job.stop = 1;
                if (a == ASK_CANCEL || a == ASK_NO)
                    return;
                if (a == ASK_ALL)
                    job.replace_all = 1;
            }
            if (ops_remove(dst) < 0 && !job.stop) {
                oops("replace", name, "It may be read only or in use.");
                return;
            }
        }
    }

    job_at(name, src, dir);
    job_tick();

    // renames only work inside one folder for now, everything else is copy then delete
    int ok = op == OP_MOVE && _rename(src, dst) == 0;
    if (!ok) {
        if (!job.measured)
            job.total += ops_measure(src, NULL, NULL);
        ok = copy_tree(src, dst, is) == 0 && !job.stop;
        if (ok && op == OP_MOVE && ops_remove(src) < 0)
            oops("remove", name, "It was copied but the original couldnt be deleted.");
    }
    if (ok && made_count < MADE_MAX)
        snprintf(made[made_count++], CAB_NAME_MAX, "%s", base_of(dst));
}

int ops_transfer(char **paths, int n, const char *dir, int op) {
    job_begin(op == OP_MOVE ? "Moving..." : "Copying...", op == OP_MOVE ? "Error Moving File" : "Error Copying File");
    if (op == OP_COPY) {
        for (int i = 0; i < n; i++)
            job.total += ops_measure(paths[i], NULL, NULL);
        job.measured = 1;
    }
    for (int i = 0; i < n && !job.stop; i++)
        transfer(paths[i], dir, op);
    job_end();
    return made_count;
}

const char *ops_made(int i) {
    return i >= 0 && i < made_count ? made[i] : NULL;
}

int ops_delete(char **paths, int n) {
    job_begin("Deleting...", "Error Deleting File");
    job.by_files = 1;
    for (int i = 0; i < n; i++) {
        int files = 0, folders = 0;
        ops_measure(paths[i], &files, &folders);
        job.total += (uint64_t)(files + folders);
    }

    int gone = 0;
    for (int i = 0; i < n && !job.stop; i++) {
        char parent[CAB_PATH_MAX];
        parent_of(paths[i], parent, sizeof(parent));
        job_at(base_of(paths[i]), parent, NULL);
        if (ops_remove(paths[i]) == 0)
            gone++;
        else if (!job.stop)
            oops("delete", base_of(paths[i]), "It may be read only or in use.");
    }
    job_end();
    return gone;
}

// the mount a path is on, the longest one that starts it
static int drive_of(const char *p) {
    user_mount_info_t m;
    int               best = -1;
    size_t            best_len = 0;
    for (int i = 0; i < MOUNTS_MAX && _mountinfo(i, &m) == 0; i++) {
        m.path[sizeof(m.path) - 1] = '\0';
        size_t len = strlen(m.path);
        if ((!strcmp(m.path, p) || inside(m.path, p)) && (best < 0 || len > best_len)) {
            best     = i;
            best_len = len;
        }
    }
    return best;
}

int ops_same_drive(const char *a, const char *b) {
    return drive_of(a) == drive_of(b);
}

// lists of paths, one a line after a line saying what theyre for

int list_write(const char *file, const char *head, char **paths, int n) {
    int fd = _open(file, O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0)
        return -1;
    _write(fd, head, (long)strlen(head));
    _write(fd, "\n", 1);
    for (int i = 0; i < n; i++) {
        _write(fd, paths[i], (long)strlen(paths[i]));
        _write(fd, "\n", 1);
    }
    _close(fd);
    return 0;
}

char **list_read(const char *file, char *head, size_t head_size, int *n) {
    *n = 0;
    int fd = _open(file, O_RDONLY);
    if (fd < 0)
        return NULL;

    long  size = _seek(fd, 0, SEEK_END);
    char *text = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (!text || _seek(fd, 0, SEEK_SET) != 0 || _read(fd, text, (unsigned long)size) != size) {
        free(text);
        _close(fd);
        return NULL;
    }
    _close(fd);
    text[size] = '\0';

    char **paths = NULL;
    char  *line  = text;
    int    first = 1;
    while (*line) {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        if (first) {
            snprintf(head, head_size, "%s", line);
            first = 0;
        } else if (line[0] == '/') {
            char **more = realloc(paths, (size_t)(*n + 1) * sizeof(char *));
            if (!more || !(more[*n] = strdup(line))) {
                paths = more ? more : paths;
                break;
            }
            paths = more;
            (*n)++;
        }
        if (!end)
            break;
        line = end + 1;
    }
    free(text);
    return paths;
}

// the clipboard is a file so every cabinet window shares it

void clip_set(char **paths, int n, int cut) {
    list_write(CLIP_FILE, cut ? "cut" : "copy", paths, n);
}

char **clip_get(int *n, int *cut) {
    char head[16] = "";
    char **paths  = list_read(CLIP_FILE, head, sizeof(head), n);
    *cut = !strcmp(head, "cut");
    return paths;
}

void clip_clear(void) {
    _unlink(CLIP_FILE);
}
