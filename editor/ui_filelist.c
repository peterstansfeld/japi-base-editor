#include "ui_filelist.h"

#include <string.h>
#include <stdio.h>

/* Helpers ----------------------------------------------------------------- */

static int ci_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int ci_strcmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = ci_tolower((unsigned char)*a);
        int cb = ci_tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void copy_str(char *dst, int dst_max, const char *src) {
    if (dst_max <= 0) return;
    int i = 0;
    while (src[i] && i < dst_max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Build the absolute path of an entry inside cwd. The convention is:
   - root "A:"      + name  → "A:name"            (no separator before name)
   - subdir "A:foo" + name  → "A:foo/name"        (slash before name)
   - special name ".."      → strip last path component from cwd. */
static void join_path(char *out, int out_max, const char *cwd, const char *name) {
    if (strcmp(name, "..") == 0) {
        copy_str(out, out_max, cwd);
        int n = (int)strlen(out);
        /* Strip trailing slash, then strip last component up to '/' or ':' */
        while (n > 0 && out[n - 1] == '/') out[--n] = 0;
        while (n > 0 && out[n - 1] != '/' && out[n - 1] != ':') out[--n] = 0;
        /* Drop the slash separator (but keep the ':' so root stays "A:"). */
        if (n > 0 && out[n - 1] == '/') out[--n] = 0;
        return;
    }
    int cwd_len = (int)strlen(cwd);
    bool need_sep = (cwd_len > 0 && cwd[cwd_len - 1] != ':' && cwd[cwd_len - 1] != '/');
    snprintf(out, (size_t)out_max, "%s%s%s", cwd, need_sep ? "/" : "", name);
}

static bool is_directory(const char *path) {
    japi_dir_t probe;
    if (japi_opendir(&probe, path)) {
        japi_closedir(&probe);
        return true;
    }
    return false;
}

static bool is_root(const char *cwd) {
    /* "A:" or "A:/" — both treated as root: no ".." entry. */
    int n = (int)strlen(cwd);
    if (n == 0) return true;
    if (n == 2 && cwd[1] == ':') return true;
    if (n == 3 && cwd[1] == ':' && cwd[2] == '/') return true;
    return false;
}

/* Sort: directories first (".." sorts before everything else among dirs),
   then files. Within each group, case-insensitive alphabetical. */
static int entry_cmp(const ui_filelist_entry_t *a, const ui_filelist_entry_t *b) {
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    if (a->is_dir) {
        if (strcmp(a->name, "..") == 0) return -1;
        if (strcmp(b->name, "..") == 0) return 1;
    }
    return ci_strcmp(a->name, b->name);
}

static void entries_sort(ui_filelist_entry_t *e, int n) {
    /* Tiny insertion sort — fine for n <= 128. */
    for (int i = 1; i < n; i++) {
        ui_filelist_entry_t key = e[i];
        int j = i - 1;
        while (j >= 0 && entry_cmp(&e[j], &key) > 0) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = key;
    }
}

/* Public API -------------------------------------------------------------- */

void ui_filelist_default_colors(ui_filelist_t *w) {
    w->fg     = VGA_WHITE;
    w->bg     = VGA_BLUE;
    w->sel_fg = VGA_BLACK;
    w->sel_bg = VGA_CYAN;
    w->dir_fg = VGA_YELLOW;
}

bool ui_filelist_open(ui_filelist_t *w, const char *path,
                      int view_row, int view_col, int view_h, int view_w) {
    copy_str(w->cwd, sizeof w->cwd, path);
    w->view_row = view_row;
    w->view_col = view_col;
    w->view_h   = view_h;
    w->view_w   = view_w;
    w->n_entries = 0;
    w->sel       = 0;
    w->top_row   = 0;
    w->picked_valid = false;
    w->picked_name[0] = 0;
    w->confirm_delete = false;

    /* Synthetic ".." entry first when not at root, so user can always go up. */
    if (!is_root(w->cwd)) {
        ui_filelist_entry_t *e = &w->entries[w->n_entries++];
        copy_str(e->name, sizeof e->name, "..");
        e->is_dir = true;
    }

    japi_dir_t d;
    if (!japi_opendir(&d, w->cwd)) {
        /* Caller still has a valid widget with 0..1 entries. */
        return false;
    }
    char nm[UI_FILELIST_NAME_MAX + 1];
    while (w->n_entries < UI_FILELIST_MAX_ENTRIES &&
           japi_readdir(&d, nm, sizeof nm)) {
        ui_filelist_entry_t *e = &w->entries[w->n_entries];
        copy_str(e->name, sizeof e->name, nm);
        /* Probe is_dir by trying to open it as a directory. Floppy-sized
           dirs make this cheap; if JFC later needs more entries fast we
           can extend the platform seam to return the type directly. */
        char full[UI_FILELIST_PATH_MAX];
        join_path(full, sizeof full, w->cwd, e->name);
        e->is_dir = is_directory(full);
        w->n_entries++;
    }
    japi_closedir(&d);

    entries_sort(w->entries, w->n_entries);
    return true;
}

static void scroll_into_view(ui_filelist_t *w) {
    if (w->sel < 0) w->sel = 0;
    if (w->sel >= w->n_entries) w->sel = w->n_entries - 1;
    if (w->sel < w->top_row)               w->top_row = w->sel;
    if (w->sel >= w->top_row + w->view_h)  w->top_row = w->sel - w->view_h + 1;
    if (w->top_row < 0)                    w->top_row = 0;
}

ui_filelist_event_t ui_filelist_key(ui_filelist_t *w, uint16_t k) {
    /* Pending delete confirmation: Y removes the selected file and reloads the
       directory; any other key cancels. Handled first so it catches the very
       next keystroke. */
    if (w->confirm_delete) {
        w->confirm_delete = false;
        if ((k == 'y' || k == 'Y') &&
            w->sel >= 0 && w->sel < w->n_entries && !w->entries[w->sel].is_dir) {
            char full[UI_FILELIST_PATH_MAX];
            join_path(full, sizeof full, w->cwd, w->entries[w->sel].name);
            japi_remove(full);
            int keep = w->sel;
            ui_filelist_open(w, w->cwd, w->view_row, w->view_col, w->view_h, w->view_w);
            if (keep >= w->n_entries) keep = w->n_entries - 1;
            if (keep < 0) keep = 0;
            w->sel = keep;
            scroll_into_view(w);
            return UI_FILELIST_CD;   /* widget reloaded; dialog stays open */
        }
        return UI_FILELIST_NONE;     /* cancelled */
    }

    /* Drive switch (Tab) and cancel (Esc) don't touch the entry list, so they
       must work even when the current drive is empty or missing -- e.g. you can
       Tab from A: to C: when there is no SD card inserted. Handled before the
       empty-list guard below, which would otherwise swallow them. */
    if (k == JAPI_KEY_TAB) {
        /* A: (SD card) <-> C: (built-in flash), always to the volume root.
           Any unknown current drive falls back to A:. */
        char other = (w->cwd[0] == 'A' || w->cwd[0] == 'a') ? 'C' : 'A';
        char root[3] = { other, ':', 0 };
        ui_filelist_open(w, root, w->view_row, w->view_col, w->view_h, w->view_w);
        return UI_FILELIST_CD;
    }
    if (k == JAPI_KEY_ESCAPE) return UI_FILELIST_CANCEL;

    if (w->n_entries == 0) return UI_FILELIST_NONE;

    switch (k) {
        case JAPI_KEY_DELETE:
            /* Only regular files can be deleted -- never a directory or "..". */
            if (!w->entries[w->sel].is_dir) w->confirm_delete = true;
            return UI_FILELIST_NONE;
        case JAPI_KEY_UP:    w->sel--;             scroll_into_view(w); return UI_FILELIST_NONE;
        case JAPI_KEY_DOWN:  w->sel++;             scroll_into_view(w); return UI_FILELIST_NONE;
        case JAPI_KEY_PGUP:  w->sel -= w->view_h;  scroll_into_view(w); return UI_FILELIST_NONE;
        case JAPI_KEY_PGDN:  w->sel += w->view_h;  scroll_into_view(w); return UI_FILELIST_NONE;
        case JAPI_KEY_HOME:  w->sel = 0;           scroll_into_view(w); return UI_FILELIST_NONE;
        case JAPI_KEY_END:   w->sel = w->n_entries - 1; scroll_into_view(w); return UI_FILELIST_NONE;
        case JAPI_KEY_ENTER: {
            const ui_filelist_entry_t *e = &w->entries[w->sel];
            if (e->is_dir) {
                char new_cwd[UI_FILELIST_PATH_MAX];
                join_path(new_cwd, sizeof new_cwd, w->cwd, e->name);
                ui_filelist_open(w, new_cwd, w->view_row, w->view_col, w->view_h, w->view_w);
                return UI_FILELIST_CD;
            }
            copy_str(w->picked_name, sizeof w->picked_name, e->name);
            w->picked_valid = true;
            return UI_FILELIST_PICK;
        }
        default: return UI_FILELIST_NONE;
    }
}

void ui_filelist_render(const ui_filelist_t *w) {
    /* Render one entry per visible row. Pad with spaces so previously-drawn
       longer entries don't bleed through when the selection moves. */
    char line[UI_FILELIST_PATH_MAX + 8];
    for (int row = 0; row < w->view_h; row++) {
        int idx = w->top_row + row;
        bool selected = (idx == w->sel);
        uint8_t fg = selected ? w->sel_fg : w->fg;
        uint8_t bg = selected ? w->sel_bg : w->bg;

        if (idx >= 0 && idx < w->n_entries) {
            const ui_filelist_entry_t *e = &w->entries[idx];
            if (!selected && e->is_dir) fg = w->dir_fg;
            /* " name/" or " name" — leading space gives a margin from any
               dialog frame the host may draw. Trailing slash marks dirs. */
            snprintf(line, sizeof line, " %s%s", e->name, e->is_dir ? "/" : "");
        } else {
            line[0] = 0;
        }
        /* Right-pad to view_w with spaces so the highlight bar is solid. */
        int len = (int)strlen(line);
        if (len > w->view_w) { line[w->view_w] = 0; len = w->view_w; }
        for (int i = len; i < w->view_w && i < (int)sizeof line - 1; i++) line[i] = ' ';
        if (w->view_w < (int)sizeof line) line[w->view_w] = 0;

        vga_print(w->view_row + row, w->view_col, line, fg, bg);
    }
}

bool ui_filelist_picked_path(const ui_filelist_t *w, char *out_path, int out_max) {
    if (!w->picked_valid) { if (out_max > 0) out_path[0] = 0; return false; }
    join_path(out_path, out_max, w->cwd, w->picked_name);
    return true;
}
