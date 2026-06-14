/* ui_filelist — generic scrollable directory-listing widget.
 *
 * Pure widget: no JBE state, no global state, no malloc. Walks the platform
 * directory-listing API (japi_opendir/readdir/closedir) and renders entries
 * into a caller-supplied rectangle on the VGA grid. Same widget powers
 * JBE's File→Open dialog and is intended for reuse by the future Japi File
 * Commander (JFC) — that's why it lives in its own translation unit and
 * takes no jbe_state_t pointer.
 *
 * Callers drive it in three phases:
 *   1. ui_filelist_open(w, path, view_row, view_col, view_h, view_w)
 *      Reads the directory, sorts entries (dirs first, then files, both
 *      case-insensitive alphabetical), and parks the selection on entry 0.
 *   2. Forward each keystroke through ui_filelist_key(w, k). The return
 *      value tells the caller whether the user just picked a file, stepped
 *      into a subdirectory (widget already reloaded itself), cancelled,
 *      or just navigated within the list.
 *   3. After UI_FILELIST_PICK: read ui_filelist_picked_path(w, out, max)
 *      to get the absolute path of the selected file.
 *
 * Caller is responsible for drawing the dialog frame around the widget;
 * ui_filelist_render only fills the view rectangle with entries + cursor.
 */
#ifndef UI_FILELIST_H
#define UI_FILELIST_H

#include <stdint.h>
#include <stdbool.h>
#include "japi_base.h"

#define UI_FILELIST_MAX_ENTRIES  128
#define UI_FILELIST_NAME_MAX      63           /* matches JBE_NAME_MAX */
#define UI_FILELIST_PATH_MAX     128

typedef struct {
    char name[UI_FILELIST_NAME_MAX + 1];
    bool is_dir;
} ui_filelist_entry_t;

typedef struct {
    /* Current directory the widget is showing (absolute path, no trailing
       slash except for the root "A:"). */
    char cwd[UI_FILELIST_PATH_MAX];

    ui_filelist_entry_t entries[UI_FILELIST_MAX_ENTRIES];
    int  n_entries;
    int  sel;            /* selected index in entries[] */
    int  top_row;        /* index of entry shown on the top visible row */

    /* Caller-set render rectangle. Inclusive bounds in VGA cells. */
    int  view_row, view_col, view_h, view_w;

    /* Color scheme — caller fills these so the widget blends with its
       host dialog. Defaults can be set with ui_filelist_default_colors. */
    uint8_t fg, bg;          /* normal entries */
    uint8_t sel_fg, sel_bg;  /* selected entry (highlight bar) */
    uint8_t dir_fg;          /* directories use this fg over bg */

    /* Latched result for the most recent ui_filelist_key call. */
    char picked_name[UI_FILELIST_NAME_MAX + 1];   /* file the user picked */
    bool picked_valid;

    /* Set after Delete is pressed on a regular file: the host should render a
       confirmation, and the next key (Y deletes, anything else cancels) is
       handled inside ui_filelist_key. */
    bool confirm_delete;
} ui_filelist_t;

typedef enum {
    UI_FILELIST_NONE   = 0,    /* key consumed, no externally-visible event */
    UI_FILELIST_PICK,          /* user pressed Enter on a regular file */
    UI_FILELIST_CD,             /* user pressed Enter on a directory; widget already reloaded */
    UI_FILELIST_CANCEL          /* user pressed Esc */
} ui_filelist_event_t;

/* Sets sensible defaults — black on white list, cyan-on-black selection,
   directories rendered yellow. Caller can override fields afterwards. */
void ui_filelist_default_colors(ui_filelist_t *w);

/* Open a directory. Sets cwd, reads entries, sorts them, parks selection at
   entry 0 (or special "..", see below), zeros picked_valid. Returns true on
   success; on failure cwd is set but n_entries is 0 (caller can still render
   an empty list). The widget yields a synthetic ".." entry whenever cwd is
   not the volume root, so the user can always navigate up. */
bool ui_filelist_open(ui_filelist_t *w, const char *path,
                      int view_row, int view_col, int view_h, int view_w);

/* Forward one JAPI_KEY_* code to the widget. Updates selection / scroll on
   navigation keys; on Enter it either returns PICK (file) or CD (subdir,
   widget already reloaded itself); Esc returns CANCEL. All other keys are
   ignored and return NONE. */
ui_filelist_event_t ui_filelist_key(ui_filelist_t *w, uint16_t k);

/* Draw the widget into its view rectangle. Does not clear outside the rect
   and does not draw a frame — the host dialog handles its own chrome. */
void ui_filelist_render(const ui_filelist_t *w);

/* After UI_FILELIST_PICK: writes the absolute path of the picked file into
   out_path (NUL-terminated, truncated to out_max). Returns false if no pick
   is pending. */
bool ui_filelist_picked_path(const ui_filelist_t *w, char *out_path, int out_max);

#endif /* UI_FILELIST_H */
