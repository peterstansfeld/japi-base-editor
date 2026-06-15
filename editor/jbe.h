/* Japi Base Editor (JBE) — public API.
 * Uses ONLY japi_base.h symbols (the platform seam). The same source builds
 * for both the Linux simulator and the Pico firmware.
 *
 * MVP step 1: read-only viewer (line-pointer buffer + render rows 1-62 +
 * status bar + vertical/horizontal scroll + cursor; no editing yet). */
#ifndef JBE_H
#define JBE_H

#include <stdint.h>
#include <stdbool.h>
#include "japi_base.h"
#include "ui_filelist.h"

#define JBE_TITLE_ROW    0
#define JBE_VIEW_TOP     1
#define JBE_VIEW_BOTTOM  62
#define JBE_VIEW_HEIGHT  (JBE_VIEW_BOTTOM - JBE_VIEW_TOP + 1)   /* 62 */
#define JBE_SCROLLBAR_COL (VGA_COLS - 1)                         /* col 126 */
#define JBE_VIEW_WIDTH    (VGA_COLS - 1)                         /* 126 (col 126 = scrollbar) */
#define JBE_WRAP_WIDTH    (JBE_VIEW_WIDTH - 1)                   /* 125 (col 125 = wrap marker on wrapped sub-rows) */
#define JBE_WRAP_GLYPH    '>'                                     /* sim placeholder; CP437 0x1A on hardware later */

/* Behavioural family of a syntax scheme. Drives the per-line colouriser:
   each flavor has its own lexer (Z80 has labels-with-colon, $/% number
   prefixes; BASIC has line numbers, ' and REM comments, no labels). The
   data tables below are shared across flavors. */
typedef enum {
    JBE_SYN_FLAVOR_NONE = 0,
    JBE_SYN_FLAVOR_Z80,
    JBE_SYN_FLAVOR_BASIC,
} jbe_syn_flavor_t;

/* One syntax scheme as pure data. The same struct shape is used for both
   ingebakken defaults (static in the binary) and later for schemes parsed
   from .syn files on the flash floppy. Keyword lists and extensions are
   null-terminated arrays of C-strings. */
typedef struct {
    const char        *name;       /* shown in Options→Syntax menu, e.g. "Z80" */
    jbe_syn_flavor_t   flavor;     /* dispatched on by colour_line() */
    const char *const *extensions; /* {".z80",".asm",".s",NULL} — auto-detect */
    const char *const *keywords;   /* mnemonics for asm, keywords for BASIC */
    const char *const *directives; /* assembler directives (asm only; NULL otherwise) */
    const char *const *registers;  /* register names (asm only; NULL otherwise) */
    const char        *comment_chars; /* characters that start a line/EOL comment, e.g. ";" */
    uint8_t color_default;
    uint8_t color_keyword;
    uint8_t color_register;
    uint8_t color_directive;
    uint8_t color_number;
    uint8_t color_string;
    uint8_t color_comment;
    uint8_t color_label;
    /* Internal — set to true for schemes built by jbe_syn_scheme_parse; the
       parser owns the string storage. Static built-in schemes leave this
       false and must NOT be passed to jbe_syn_scheme_free. */
    bool    _owns_storage;
} jbe_syn_scheme_t;

/* Parse a .syn scheme description from a text buffer (no NUL terminator
   required; pass the byte length). Returns a malloc'd scheme that owns
   its string storage, or NULL on parse failure / out-of-memory.

   Format (one key=value per line, '#' starts a comment, blank lines OK):
     name=Z80
     flavor=z80                 # one of: none, z80
     extensions=.z80,.asm,.s    # comma-separated
     comment=;                  # any chars that start an EOL comment
     keywords=ADC ADD ...       # space-separated
     directives=ORG DB ...      # space-separated (optional)
     registers=A B C ...        # space-separated (optional)
     color_default=white
     color_keyword=white
     color_register=magenta
     color_directive=cyan
     color_number=yellow
     color_string=cyan
     color_comment=green
     color_label=orange

   Recognised colour names: black, blue, green, cyan, red, magenta,
   yellow, white, orange. Unknown colour names default to white. */
jbe_syn_scheme_t *jbe_syn_scheme_parse(const char *text, int len);

/* Load a .syn file from the given path through the platform file API.
   Returns a malloc'd scheme on success, NULL if the file does not exist,
   is empty, or fails to parse. Caller frees with jbe_syn_scheme_free. */
jbe_syn_scheme_t *jbe_syn_scheme_load(const char *path);

/* Free a scheme returned by jbe_syn_scheme_parse / jbe_syn_scheme_load.
   NULL-safe. NEVER call this on a built-in scheme (anything with
   _owns_storage == false). */
void jbe_syn_scheme_free(jbe_syn_scheme_t *scheme);
#define JBE_STATUS_ROW   63
#define JBE_NAME_MAX     63
#define JBE_CLIP_MAX     32      /* max files in one Commander cut/copy batch */
#define JBE_PATH_MAX     127

/* Largest file jbe_load will even attempt. Checked BEFORE allocating, so a huge
   file (e.g. a ~787 KB screenshot BMP, bigger than the whole RP2350 RAM) is
   refused at once and never triggers a giant malloc that could wedge the
   machine. Generous for any text program on this hardware; loading doubles the
   memory briefly (file image + line copies), so this stays well within RAM. */
#define JBE_MAX_FILE_BYTES  (64 * 1024)

/* --- Undo / Redo ----------------------------------------------------------
 *
 * Every user-visible edit pushes one undo record describing how to reverse
 * it. Ctrl+Z pops a record, applies its reverse, and pushes the mirror
 * record onto the redo stack. Ctrl+Y does the opposite. Any *new* edit
 * clears the redo stack (standard editor behaviour: branching the history
 * discards the alternative future).
 *
 * Records live in a ring buffer per direction (undo + redo). When the
 * combined text payload across both stacks exceeds JBE_UNDO_BUDGET_BYTES,
 * the oldest records are dropped from the undo stack first. Consecutive
 * single-character inserts on the same line are coalesced into one record
 * so Ctrl+Z removes a word, not a letter (added in a later step).
 *
 * Record kinds:
 *   INSERT — user inserted `text` at (row, col). Reverse: delete `text_len`
 *            characters starting at (row, col).
 *   DELETE — user deleted `text` at (row, col). Reverse: insert `text` at
 *            (row, col).
 *   SPLIT  — user split row at col (Enter). Reverse: join the line at
 *            (row+1) back onto row at col.
 *   JOIN   — user joined row+1 onto row at col (Backspace at line start /
 *            Delete at line end). Reverse: split row at col.
 *
 * Note: SPLIT/JOIN carry no `text`; only row+col. INSERT/DELETE own a
 * malloc'd `text` buffer of length `text_len` (not NUL-terminated).
 *
 * Each record also stores the cursor position from *before* the edit so
 * that undo restores not only the buffer contents but also where the
 * cursor sat — matches user expectations.
 */

#define JBE_UNDO_BUDGET_BYTES  (32 * 1024)   /* per buffer, undo + redo combined */

typedef enum {
    JBE_UNDO_INSERT,
    JBE_UNDO_DELETE,
    JBE_UNDO_SPLIT,
    JBE_UNDO_JOIN,
} jbe_undo_kind_t;

typedef struct {
    jbe_undo_kind_t kind;
    int   row;
    int   col;
    char *text;             /* malloc'd, NULL for SPLIT/JOIN. For multi-line
                               selection-delete / paste the text contains
                               '\n' separators. */
    int   text_len;
    int   cur_row_before;   /* cursor position before the edit, restored on undo */
    int   cur_col_before;
    uint64_t seq;           /* monotonic sequence number; used to detect when
                               undo/redo brings us back to the saved state. */
    uint64_t last_kpress;   /* jbe_handle_key sequence number when this record
                               was last extended — coalescing requires the
                               NEXT key to be immediately consecutive (no
                               unrelated keys in between, even if they leave
                               the cursor at the same column). */
    bool  is_block;         /* true if text describes a block (each '\n'-segment
                               applies to a separate row at the same col) */
    bool  coalesce_typing;  /* hint: next INSERT on same row at col+text_len may merge */
    /* Block-insert padding/grow metadata. Only populated for INSERT records
       with is_block == true that originally grew the buffer past EOF or
       padded short rows with spaces. Lets undo restore the original buffer
       byte-for-byte instead of leaving phantom rows / padding behind. */
    int16_t  grew_lines;    /* number of trailing rows that were appended past EOF */
    int16_t  pad_count;     /* length of pad_orig_len[]; equals segment count */
    int16_t *pad_orig_len;  /* per segment-index: original len[row] before padding,
                               or -1 if that row was newly created or did not need
                               padding. malloc'd, NULL if no padding/grow happened. */
} jbe_undo_rec_t;

typedef struct {
    jbe_undo_rec_t *recs;   /* dynamic array, grown on demand up to a cap */
    int             head;   /* index just past the newest record (modulo cap) */
    int             count;  /* number of live records */
    int             cap;    /* allocated capacity */
    int             bytes_used; /* sum of text_len across live records */
} jbe_undo_stack_t;

/* --- Pane (viewport + cursor + selection + find/replace) -----------------
 * The editor shows one or two panes and holds at most two documents, under a
 * strict rule: every buffer is shown in some pane — there are no hidden,
 * off-screen documents. With the split open the two panes may either
 *   - SHARE one buffer: two views of the same file, handy for copying between
 *     distant parts (this is what View->Split starts with), or
 *   - hold ONE buffer each: two files side by side.
 * A pane records which buffer it shows in buf_idx. File->New / File->Open in a
 * pane that is currently sharing first moves that pane to the free buffer slot,
 * so the other pane's file is left untouched. This deliberately drops the older
 * "open many files, cycle through them" model: with files loading instantly
 * from the SD a hidden buffer bought nothing but the risk of forgotten, unseen
 * unsaved edits. */
#define JBE_MAX_PANES  2    /* horizontal 2-way split; also = max open documents */

typedef struct {
    int  buf_idx;        /* which buffer (0..JBE_MAX_PANES-1) this pane shows */
    int  cur_row;        /* cursor file row (0-based) */
    int  cur_col;        /* cursor column within the line */
    int  top_row;        /* viewport top = file row at screen row JBE_VIEW_TOP */
    /* Selection. Anchor at (sel_row, sel_col); the other end is the cursor. */
    bool sel_active;
    bool sel_block;      /* false = stream selection, true = block (rectangle) */
    int  sel_row;
    int  sel_col;
    /* Find. When find_active, a framed box shows the query and the cursor sits
       on the current match (also reverse-video selected via sel_active). */
    bool find_active;
    int  find_query_len;
    int  find_match_row;       /* -1 if no match yet */
    int  find_match_col;
    char find_query[128];
    /* Replace. Fluid model (analogous to Find): the box stays open and both
       fields stay editable. replace_phase is the FOCUSED field: 0 = the search
       field (shares find_query), 1 = the "with" field. */
    bool replace_active;
    int  replace_phase;
    int  replace_with_len;
    char replace_with[128];
} jbe_pane_t;

/* One dropdown menu entry. Used both by the static File/Edit/Search/Window
   menus (declared in jbe.c) and by the dynamic Options menu (stored in
   jbe_state_t.options_items). NULL label terminates the array. */
typedef struct {
    const char *label;
    char        accel;
    const char *hint;
} jbe_menu_item_t;

/* --- Buffer (text + history + file identity) ------------------------------
 * Split-screen step 1b: per-file state lives here so a later step can put
 * an array of buffers behind one editor while panes pick which to view.
 * Single buffer per state for now. */
typedef struct {
    char **lines;        /* dynamic array of line strings (no trailing \n) */
    int   *len;          /* cached length per line */
    int    n_lines;
    int    cap_lines;
    bool   dirty;        /* unsaved edits since load (cleared on load/save) */
    /* Name of the active syntax scheme: empty string means no
       highlighting; otherwise the lookup is case-insensitive against
       both built-ins (JBE_BUILTIN_SCHEMES) and A:syntax/<name>.syn on
       the flash floppy. Set by jbe_load (auto-detect from extension)
       and by the Options→Syntax menu. */
    char   syntax_name[JBE_NAME_MAX + 1];
    /* Pointer to the active scheme: NULL when syntax_name is empty,
       else a built-in (Z80_SCHEME, ...) or a parsed scheme owned by
       the buffer. Owned schemes have _owns_storage == true and are
       freed by buffer_free. Rebuilt each time syntax_name changes. */
    const jbe_syn_scheme_t *active_scheme;
    char   filename[JBE_NAME_MAX + 1];   /* basename, shown in status bar */
    char   path[JBE_PATH_MAX + 1];       /* full path passed to jbe_load, used by jbe_save */
    /* Undo / redo history. */
    jbe_undo_stack_t undo;
    jbe_undo_stack_t redo;
    /* Saved-state tracking. saved_seq is the seq of the undo top at the
       moment of the last successful save (0 means "saved at the load
       state"). dirty becomes false again iff the current undo top has
       that seq AND saved_reachable is still true (the saved record may
       have been evicted by the byte-budget — in which case we can never
       return to a clean state via undo/redo). */
    uint64_t seq_counter;
    uint64_t saved_seq;
    uint64_t kpress_seq;     /* incremented by jbe_handle_key on every call */
    bool     saved_reachable;
} jbe_buffer_t;

/* What the shared "unsaved changes" confirmation should do once the user agrees
   to discard. The same framed box guards Close, un-split, New and Open. */
enum {
    JBE_CONFIRM_NONE = 0,
    JBE_CONFIRM_CLOSE,
    JBE_CONFIRM_UNSPLIT,
    JBE_CONFIRM_NEW,
    JBE_CONFIRM_OPEN
};

typedef struct {
    /* Buffers — one per pane (see the pane comment above). buffers[0] is
       always live; buffers[1] is live only while split_active is true. */
    jbe_buffer_t buffers[JBE_MAX_PANES];
    /* Panes — viewport state, one per visible split. Single pane is in use
       when split_active is false (panes[0]). */
    jbe_pane_t panes[JBE_MAX_PANES];
    int        active_pane;     /* 0 or 1; the one that receives key input */
    bool       split_active;    /* false: only panes[0] is rendered */
    int        split_row;       /* screen row of the horizontal divider line */
    /* Clipboard — editor-wide (shared across buffers and panes). */
    char  *clip;
    int    clip_len;
    bool   clip_block;   /* false = stream paste; true = block paste (5c) */
    /* Menu (step 7). menu_idx is the active title 0..3; item_idx is the
       active dropdown item; menu_active = dropdown open. */
    bool menu_active;
    int  menu_idx;
    int  item_idx;
    /* Options→Syntax menu is rebuilt dynamically every time the menu is
       opened. Item 0 is always "Syntax: None"; later items list each
       built-in plus any user `.syn` file whose name is not already
       covered by a built-in (file with matching name= overrides via the
       look-up path, so it stays one menu item). Cap of 16 items is a
       soft limit; extra .syn files past that are silently dropped. */
    #define JBE_OPTIONS_MAX_ITEMS  16
    #define JBE_OPTIONS_LABEL_MAX  (JBE_NAME_MAX + 16)   /* "Syntax: ..." */
    int  options_n;
    char options_labels[JBE_OPTIONS_MAX_ITEMS][JBE_OPTIONS_LABEL_MAX + 1];
    char options_names [JBE_OPTIONS_MAX_ITEMS][JBE_NAME_MAX + 1];
    /* Mirror of options_labels/names as a menu_item_t array so the
       generic menu helpers (menu_item_count, dropdown render, etc.) can
       iterate over it the same way they iterate over the static menus.
       Element options_n has label=NULL as the sentinel terminator. */
    jbe_menu_item_t options_items[JBE_OPTIONS_MAX_ITEMS + 1];
    bool quit;           /* set by File->Exit; main loop returns when true */
    bool new_request;    /* File->New: main creates a fresh empty buffer */
    bool run_request;    /* host app runs the active buffer (JBB sets this from
                            the Run menu; plain JBE leaves it untouched) */
    bool save_request;   /* File->Save / Save As: the embedding loop performs the
                            actual jbe_save() AFTER jbe_handle_key returns. This
                            keeps the (stack-heavy) write off the deep key-handling
                            call chain -- writing to C: (LittleFS) from there
                            overflowed the ~2 KB Core 0 stack into the VGA buffers
                            and dropped the video signal. Mirrors open_request. */
    bool caret_on;       /* blink phase for the Find/Replace input caret. The
                            embedding loop sets this from its own clock (~2 Hz)
                            before each jbe_render; the renderer draws the caret
                            only while it is true. Keeps jbe.c free of any clock. */

    /* File→Open dialog: when open_active, jbe_handle_key routes keystrokes
       through the ui_filelist widget instead of editing the buffer; when
       the user picks a file, open_path holds its absolute path and
       open_request is raised so jbe_main can route it to a buffer slot. */
    bool           open_active;
    bool           open_request;
    char           open_path[JBE_PATH_MAX + 1];
    ui_filelist_t  open_dlg;

    /* Japi Commander: a modal two-pane file manager (Run -> Japi Commander, or
       Ctrl+J). v0 copies files only (F5). Reuses the ui_filelist widget for each
       pane; Tab switches the active pane, Ctrl+Tab switches that pane's drive. */
    bool           commander_active;
    int            commander_pane;       /* 0 = left, 1 = right */
    ui_filelist_t  commander_list[2];
    char           commander_msg[128];   /* status line: copy result / error */

    /* Commander v1 file ops: a modal name prompt (mkdir / rename) and a delete
       confirmation, both shown on the Commander's status row. */
    bool           commander_input_active;
    int            commander_input_kind;  /* 0 = mkdir, 1 = rename */
    char           commander_input[80];
    int            commander_input_len;
    int            commander_input_cur;   /* caret index within the field, 0..len */
    bool           commander_confirm_delete;

    /* Windows-style clipboard: Ctrl+C / Ctrl+X snapshot the tagged (or current)
       files; Ctrl+V copies/moves them into the active pane. */
    char           clip_src[UI_FILELIST_PATH_MAX];           /* source directory */
    char           clip_names[JBE_CLIP_MAX][UI_FILELIST_NAME_MAX + 1];
    int            clip_n;
    bool           clip_cut;                                 /* false=copy, true=move */

    /* Options -> CPU speed: a small floating chooser (260 / 324 / 390 MHz). */
    int            cpu_item_index;     /* row of "CPU speed..." in the Options menu */
    bool           cpu_dialog_active;
    int            cpu_sel;            /* highlighted tier: 0 = 260, 1 = 324, 2 = 390 */

    /* F1 Help overlay (v2.0, in progress): a centred framed window that floats
       over the editor so the work behind it stays visible. help_top = scroll
       offset (first visible help line). */
    bool           help_active;
    int            help_top;
    /* Transient one-line notice shown on the status row (high-visibility),
       e.g. why a file could not be opened. Set by jbe_load on a failed/refused
       load (the current document is left untouched), cleared on the next key
       and on any successful load. Empty string = no notice. */
    char           open_msg[80];

    /* File→Save As: when save_as_active the status row is a filename prompt.
       Enter saves the active buffer to save_as_name and adopts it as the
       buffer's path; Esc cancels. Save on an untitled buffer (no path yet)
       opens this same prompt, which is why New→Save now works. */
    bool save_as_active;
    int  save_as_len;
    char save_as_name[JBE_PATH_MAX + 1];

    /* File→Close: empty the active pane (it falls back to a fresh untitled
       document; the pane itself never disappears). close_request is consumed
       by the embedding loop, which calls jbe_close_active(). close_confirm is
       raised when that would discard unsaved edits: the status row asks to
       confirm and the next Y/Enter discards (any other key cancels).
       confirm_action says WHICH action is waiting on that yes: close, un-split,
       New or Open (all four share the one box). */
    bool close_request;
    bool close_confirm;
    int  confirm_action;       /* JBE_CONFIRM_* */

    /* Keyboard macro: record JAPI_KEY codes on Ctrl+M, replay on Ctrl+P.
       Recording is linear (not circular) — once the buffer is full,
       further keys are dropped so a long session can't silently overwrite
       the start. Ctrl+M and Ctrl+P themselves are filtered from recording
       to keep macros composable. macro_playing guards against recursive
       replay (a macro that contains Ctrl+P) and against recording our
       own replayed keys. */
    #define JBE_MACRO_MAX 256
    uint16_t macro_buf[JBE_MACRO_MAX];
    uint16_t macro_len;
    bool     macro_recording;
    bool     macro_playing;
} jbe_state_t;

/* Returns a pointer to the active pane and to the buffer that pane shows.
   A pane shows the buffer named by its buf_idx (two panes may share one).
   Works for both `jbe_state_t *` and `const jbe_state_t *`. */
#define JBE_PANE(s) (&(s)->panes[(s)->active_pane])
#define JBE_BUF(s)  (&(s)->buffers[JBE_PANE(s)->buf_idx])

void jbe_init(jbe_state_t *s);
void jbe_new(jbe_state_t *s);
bool jbe_load(jbe_state_t *s, const char *path);
bool jbe_save(jbe_state_t *s);
/* Close the active document: the active pane falls back to a fresh untitled
   buffer. The pane itself is never removed and the other pane (if the split is
   open) is left untouched. To go back to a single pane, un-split via
   View->Split. Called by the embedding loop when close_request is set. */
void jbe_close_active(jbe_state_t *s);
/* Open the second pane (if not already split) and load `path` into it,
   leaving focus on that new pane. Returns false if the load failed (the
   split still opens, with an empty buffer). Used by the sim entry-points to
   honour a second command-line file. */
bool jbe_open_in_split(jbe_state_t *s, const char *path);
void jbe_free(jbe_state_t *s);

void jbe_handle_key(jbe_state_t *s, uint16_t k);
void jbe_render(const jbe_state_t *s);

/* Undo / redo (also bound to Ctrl+Z / Ctrl+Y and to Edit menu). */
void jbe_undo(jbe_state_t *s);
void jbe_redo(jbe_state_t *s);

#endif
