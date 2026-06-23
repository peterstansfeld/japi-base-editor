/* JBE MVP step 1 — read-only viewer.
 * Loads a text file via the Japi Base file API, keeps it as an array of line
 * strings, and lets the user navigate with arrows / Home / End / PgUp / PgDn.
 * The viewport auto-scrolls so the cursor stays visible.
 *
 * Colour scheme is the classic Turbo Pascal / QuickBASIC feel: dark-blue
 * background, white text, cyan title/status bars, the cursor cell as a
 * reverse-video block.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "jbe.h"
#include "help_text.h"   /* generated from JBE_MANUAL.md by tools/mkhelp.py:
                            HELP_LINES[], HELP_NLINES, HELP_ANCHORS[] */

#define JBE_BG          VGA_DARK_BLUE
#define JBE_FG          VGA_WHITE
#define JBE_BAR_FG      VGA_BLACK
#define JBE_BAR_BG      VGA_CYAN
/* The status bar turns black-on-yellow while it is waiting for input (Save As,
   Close confirmation, Find, Replace) so the prompt clearly stands out from the
   normal cyan bar. */
#define JBE_PROMPT_FG   VGA_BLACK
#define JBE_PROMPT_BG   VGA_YELLOW
/* Bracket matching: the bracket the cursor sits on (or just left of) and its
   partner get a calm grey "chip"; a bracket with no partner turns red. Grey is
   distinct from the white-background selection, and recolouring only the cell's
   background leaves the syntax foreground colour readable. */
#define JBE_MATCH_FG    VGA_BLACK
#define JBE_MATCH_BG    0x2A            /* medium grey (6-bit RGB 10/10/10) */
#define JBE_UNMATCH_FG  VGA_WHITE
#define JBE_UNMATCH_BG  VGA_RED

/* --- Line buffer helpers ---------------------------------------------- */

static bool jbe_push_line(jbe_state_t *s, const char *buf, int n) {
    if (JBE_BUF(s)->n_lines == JBE_BUF(s)->cap_lines) {
        int nc = JBE_BUF(s)->cap_lines ? JBE_BUF(s)->cap_lines * 2 : 64;
        char **nl = realloc(JBE_BUF(s)->lines, nc * sizeof *nl);
        int   *nlen = realloc(JBE_BUF(s)->len,  nc * sizeof *nlen);
        if (!nl || !nlen) { free(nl); free(nlen); return false; }
        JBE_BUF(s)->lines = nl; JBE_BUF(s)->len = nlen; JBE_BUF(s)->cap_lines = nc;
    }
    char *line = malloc(n + 1);
    if (!line) return false;
    memcpy(line, buf, n);
    line[n] = 0;
    JBE_BUF(s)->lines[JBE_BUF(s)->n_lines]   = line;
    JBE_BUF(s)->len[JBE_BUF(s)->n_lines]     = n;
    JBE_BUF(s)->n_lines++;
    return true;
}

/* --- Undo / redo helpers ----------------------------------------------
 *
 * Each direction (undo, redo) is a ring buffer of records. We grow the ring
 * geometrically up to a hard count cap; beyond that, and whenever the
 * combined text payload across both rings exceeds JBE_UNDO_BUDGET_BYTES,
 * we evict the oldest record (undo-side first, redo-side only if undo is
 * already empty). See jbe.h for the record-kind semantics.
 *
 * Ownership rule: undo_push() *steals* the `text` pointer (caller hands over
 * a malloc'd buffer or NULL). Eviction free()'s the text. */

#define JBE_UNDO_MAX_RECS  1024  /* hard cap on record count per stack */

/* Release all records and reset to the empty state. Safe on zero-init. */
static void undo_stack_free(jbe_undo_stack_t *st) {
    if (st->recs) {
        for (int i = 0; i < st->count; i++) {
            int idx = (st->head - st->count + i + st->cap) % st->cap;
            free(st->recs[idx].text);
            free(st->recs[idx].pad_orig_len);
        }
        free(st->recs);
    }
    memset(st, 0, sizeof *st);
}

/* Drop the oldest record from the stack (free its text payload). If the
   dropped record was the save-point anchor, mark the saved state as
   unreachable so dirty can never go back to false via undo/redo. */
static void undo_stack_drop_oldest(jbe_state_t *s, jbe_undo_stack_t *st) {
    if (st->count == 0) return;
    int idx = (st->head - st->count + st->cap) % st->cap;
    if (JBE_BUF(s)->saved_reachable && JBE_BUF(s)->saved_seq != 0
        && st->recs[idx].seq == JBE_BUF(s)->saved_seq)
        JBE_BUF(s)->saved_reachable = false;
    st->bytes_used -= st->recs[idx].text_len;
    st->bytes_used -= st->recs[idx].pad_count * (int)sizeof(int16_t);
    free(st->recs[idx].text);
    free(st->recs[idx].pad_orig_len);
    st->recs[idx].text = NULL;
    st->recs[idx].pad_orig_len = NULL;
    st->recs[idx].pad_count = 0;
    st->count--;
}

/* Read-only pointer to the newest record, or NULL if empty. Used by the
   typing-coalescer to grow an existing INSERT in place. */
static jbe_undo_rec_t *undo_stack_peek_newest(jbe_undo_stack_t *st) {
    if (st->count == 0) return NULL;
    int idx = (st->head - 1 + st->cap) % st->cap;
    return &st->recs[idx];
}

/* Word-character test for the typing coalescer: identifier chars stay glued
   together, everything else (space, tab, punctuation, symbols) breaks the
   current undo record so one Ctrl+Z undoes one word — the Word/Notepad++/
   VS Code convention. */
static bool jbe_is_word_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        ||  c == '_';
}

/* Pop the newest record into `*out` (caller owns the text). Returns false
   if the stack was empty. Used by Ctrl+Z / Ctrl+Y in a later step. */
static bool undo_stack_pop(jbe_undo_stack_t *st, jbe_undo_rec_t *out) {
    if (st->count == 0) return false;
    int idx = (st->head - 1 + st->cap) % st->cap;
    *out = st->recs[idx];
    st->recs[idx].text = NULL;
    st->recs[idx].pad_orig_len = NULL;
    st->head = idx;
    st->count--;
    st->bytes_used -= out->text_len;
    st->bytes_used -= out->pad_count * (int)sizeof(int16_t);
    return true;
}

/* Append a record. Grows the ring up to JBE_UNDO_MAX_RECS; beyond that,
   evicts the oldest entry to make room. Steals ownership of rec->text.
   `s` is passed only so eviction can update saved_reachable. */
static void undo_stack_push_rec(jbe_state_t *s, jbe_undo_stack_t *st,
                                const jbe_undo_rec_t *rec) {
    if (st->cap == 0) {
        st->cap  = 16;
        st->recs = calloc(st->cap, sizeof *st->recs);
        if (!st->recs) { free(rec->text); free(rec->pad_orig_len); st->cap = 0; return; }
    } else if (st->count == st->cap && st->cap < JBE_UNDO_MAX_RECS) {
        int nc = st->cap * 2;
        if (nc > JBE_UNDO_MAX_RECS) nc = JBE_UNDO_MAX_RECS;
        jbe_undo_rec_t *nr = calloc(nc, sizeof *nr);
        if (!nr) { free(rec->text); free(rec->pad_orig_len); return; }
        int oldest = (st->head - st->count + st->cap) % st->cap;
        for (int i = 0; i < st->count; i++)
            nr[i] = st->recs[(oldest + i) % st->cap];
        free(st->recs);
        st->recs = nr;
        st->cap  = nc;
        st->head = st->count;
    } else if (st->count == st->cap) {
        undo_stack_drop_oldest(s, st);
    }
    st->recs[st->head] = *rec;
    st->head = (st->head + 1) % st->cap;
    st->count++;
    st->bytes_used += rec->text_len;
    st->bytes_used += rec->pad_count * (int)sizeof(int16_t);
}

/* Keep the combined undo+redo payload under JBE_UNDO_BUDGET_BYTES by
   evicting oldest records (undo first, then redo). */
static void undo_enforce_budget(jbe_state_t *s) {
    while (JBE_BUF(s)->undo.bytes_used + JBE_BUF(s)->redo.bytes_used > JBE_UNDO_BUDGET_BYTES) {
        if      (JBE_BUF(s)->undo.count > 0) undo_stack_drop_oldest(s, &JBE_BUF(s)->undo);
        else if (JBE_BUF(s)->redo.count > 0) undo_stack_drop_oldest(s, &JBE_BUF(s)->redo);
        else break;
    }
}

/* Update JBE_BUF(s)->dirty from the saved-state anchor. The buffer is "clean" iff
   the current undo top has the saved seq (or both are at the load state)
   AND the saved record has not been evicted from the history. */
static void jbe_recompute_dirty(jbe_state_t *s) {
    if (!JBE_BUF(s)->saved_reachable) { JBE_BUF(s)->dirty = true; return; }
    uint64_t top = (JBE_BUF(s)->undo.count > 0)
                 ? undo_stack_peek_newest(&JBE_BUF(s)->undo)->seq
                 : 0;
    JBE_BUF(s)->dirty = (top != JBE_BUF(s)->saved_seq);
}

/* Push a new undo record from a user edit. Steals `text` (NULL or malloc'd).
   Clears the redo stack: any new edit invalidates the alternative future. */
static void undo_push(jbe_state_t *s, jbe_undo_kind_t kind,
                      int row, int col, char *text, int text_len,
                      int cur_row_before, int cur_col_before, bool is_block) {
    jbe_undo_rec_t rec = {0};
    rec.kind            = kind;
    rec.row             = row;
    rec.col             = col;
    rec.text            = text;
    rec.text_len        = text_len;
    rec.cur_row_before  = cur_row_before;
    rec.cur_col_before  = cur_col_before;
    rec.is_block        = is_block;
    rec.seq             = ++JBE_BUF(s)->seq_counter;
    undo_stack_push_rec(s, &JBE_BUF(s)->undo, &rec);
    /* Clear redo on any new user edit. Dropping a saved-anchor record on
       the redo side marks the save state as unreachable too. */
    while (JBE_BUF(s)->redo.count > 0) undo_stack_drop_oldest(s, &JBE_BUF(s)->redo);
    undo_enforce_budget(s);
    jbe_recompute_dirty(s);
}

/* --- Syntax-scheme registry -------------------------------------------
   The actual data tables (keyword lists, default Z80_SCHEME instance,
   z80_colour_line lexer, etc.) live further down in the Syntax-
   highlighting section. We forward-declare just the registry + id-lookup
   here so the top-of-file helpers (buffer_free, jbe_load, dropdown
   menu) can route through them. */
extern const jbe_syn_scheme_t *const JBE_BUILTIN_SCHEMES[];
static const jbe_syn_scheme_t *scheme_by_name(const char *name);
static int syn_streq_ci(const char *a, const char *b);

/* Free one buffer's owned memory and reset it to the pristine "(no file)"
   state. Safe on a zero-initialised buffer. */
static void buffer_free(jbe_buffer_t *b) {
    for (int i = 0; i < b->n_lines; i++) free(b->lines[i]);
    free(b->lines); free(b->len);
    undo_stack_free(&b->undo);
    undo_stack_free(&b->redo);
    /* Free the active scheme only if it was parsed (i.e. owns its
       storage); built-ins are static. */
    if (b->active_scheme && b->active_scheme->_owns_storage)
        jbe_syn_scheme_free((jbe_syn_scheme_t *)b->active_scheme);
    memset(b, 0, sizeof *b);
    strcpy(b->filename, "(no file)");
    b->saved_reachable = true;
}

/* Build the canonical path `C:config/syntax/<lowercase_name>.syn` into out[].
   Returns false if name is empty or won't fit. */
static bool build_syn_path(const char *name, char *out, int out_max) {
    if (!name || !*name) return false;
    int prefix = (int)strlen("C:config/syntax/");
    if ((int)strlen(name) > out_max - prefix - 5) return false;
    memcpy(out, "C:config/syntax/", (size_t)prefix);
    int p = prefix;
    for (const char *n = name; *n; n++) {
        char c = *n;
        if (c >= 'A' && c <= 'Z') c += 32;
        out[p++] = c;
    }
    memcpy(out + p, ".syn", 4); p += 4;
    out[p] = 0;
    return true;
}

/* Resolve the active scheme for a name. Tries C:config/syntax/<name>.syn first;
   if that exists and parses, returns the parsed override (caller owns).
   Otherwise returns the matching built-in (static, never freed). Returns
   NULL for empty name or when nothing matches. */
static const jbe_syn_scheme_t *scheme_resolve(const char *name) {
    if (!name || !*name) return 0;
    char path[64];
    if (build_syn_path(name, path, sizeof path)) {
        jbe_syn_scheme_t *parsed = jbe_syn_scheme_load(path);
        if (parsed) return parsed;
    }
    return scheme_by_name(name);
}

/* Set the active scheme on a buffer, freeing whatever was there if it
   was a parsed override. Pass NULL to clear. */
static void install_scheme(jbe_buffer_t *b, const jbe_syn_scheme_t *scheme) {
    if (b->active_scheme == scheme) return;
    if (b->active_scheme && b->active_scheme->_owns_storage)
        jbe_syn_scheme_free((jbe_syn_scheme_t *)b->active_scheme);
    b->active_scheme = scheme;
}

void jbe_init(jbe_state_t *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < JBE_MAX_PANES; i++) {
        strcpy(s->buffers[i].filename, "(no file)");
        s->buffers[i].saved_reachable = true;
    }
    /* Horizontal divider lands mid-screen by default (top 1..31, bottom
       33..62). Only rendered when split_active goes true (step 3). */
    s->split_row = (JBE_VIEW_TOP + JBE_VIEW_BOTTOM + 1) / 2;

    /* All built-in config lives under C:config/ (the always-present flash);
       user syntax schemes go in C:config/syntax/. Make sure both exist so the
       user can Save a .syn there and the Options menu can scan it. Create the
       parent first: mkdir is idempotent (an existing folder is ignored). */
    japi_mkdir("C:config");
    japi_mkdir("C:config/syntax");
}

void jbe_free(jbe_state_t *s) {
    for (int i = 0; i < JBE_MAX_PANES; i++) buffer_free(&s->buffers[i]);
    free(s->clip);
    jbe_init(s);
}

/* Cheap case-insensitive extension match — returns 1 if path ends with .ext */
static int ext_is(const char *path, const char *ext) {
    int pn = (int)strlen(path), en = (int)strlen(ext);
    if (pn < en) return 0;
    for (int i = 0; i < en; i++) {
        char a = path[pn - en + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Match path extension against the registered built-in schemes, then
   (if no built-in claimed the extension) against user .syn files on
   C:config/syntax/. Writes the scheme name (or empty string) into name_out.
   Built-ins always win — a user scheme can only add extensions for a
   language JBE doesn't already know. */
static void syntax_for_path(const char *path, char *name_out, int name_max) {
    if (name_max <= 0) return;
    name_out[0] = 0;
    /* Pass 1: built-ins. */
    for (int s = 0; JBE_BUILTIN_SCHEMES[s]; s++) {
        const jbe_syn_scheme_t *sch = JBE_BUILTIN_SCHEMES[s];
        if (!sch->extensions || !sch->name) continue;
        for (int e = 0; sch->extensions[e]; e++)
            if (ext_is(path, sch->extensions[e])) {
                strncpy(name_out, sch->name, (size_t)name_max - 1);
                name_out[name_max - 1] = 0;
                return;
            }
    }
    /* Pass 2: scan C:config/syntax/ for a user-only scheme (.syn file) that
       claims this extension. First match wins in directory order. */
    japi_dir_t d;
    if (!japi_opendir(&d, "C:config/syntax")) return;
    char fname[JBE_NAME_MAX + 1];
    while (japi_readdir(&d, fname, sizeof fname)) {
        int n = (int)strlen(fname);
        if (n < 5) continue;
        const char *ext = fname + n - 4;
        if (!(ext[0] == '.' && (ext[1] == 's' || ext[1] == 'S')
                             && (ext[2] == 'y' || ext[2] == 'Y')
                             && (ext[3] == 'n' || ext[3] == 'N'))) continue;
        char syn_path[64];
        if (n + (int)strlen("C:config/syntax/") + 1 > (int)sizeof syn_path) continue;
        snprintf(syn_path, sizeof syn_path, "C:config/syntax/%s", fname);
        jbe_syn_scheme_t *p = jbe_syn_scheme_load(syn_path);
        if (!p) continue;
        bool matched = false;
        if (p->name && p->extensions) {
            for (int e = 0; p->extensions[e]; e++)
                if (ext_is(path, p->extensions[e])) { matched = true; break; }
        }
        if (matched) {
            strncpy(name_out, p->name, (size_t)name_max - 1);
            name_out[name_max - 1] = 0;
            jbe_syn_scheme_free(p);
            japi_closedir(&d);
            return;
        }
        jbe_syn_scheme_free(p);
    }
    japi_closedir(&d);
}

/* If the active pane is sharing the other pane's buffer (two views of one
   file), move it to the free buffer slot so an upcoming New/Open replaces only
   this pane's document and leaves the other pane untouched. No-op when there is
   no split, or when the pane already has its own buffer. */
static void detach_active_pane(jbe_state_t *s) {
    if (!s->split_active) return;
    int other = s->active_pane ^ 1;
    if (s->panes[other].buf_idx == JBE_PANE(s)->buf_idx)
        JBE_PANE(s)->buf_idx ^= 1;          /* claim the other (free) slot */
}

bool jbe_load(jbe_state_t *s, const char *path) {
    /* Read the whole file into memory FIRST, before touching any buffer. A file
       the editor can't display -- too large for RAM, or binary (a BMP, a UF2,
       ...) -- then leaves the editor exactly as it was: the current document is
       untouched and we only report why. This read-first / commit-later order is
       what stops a bad Open from wedging the editor on a half-built buffer. */
    const char *bn = path;               /* basename for messages + the title */
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') bn = p + 1;

    /* static: a japi_file_t embeds a FatFs FIL (~550 bytes -- it carries a
       512-byte sector buffer) and the platform's Core 0 stack is only ~2 KB.
       The editor runs single-threaded and never opens a file re-entrantly, so
       static storage is safe and keeps that big object off the stack. */
    static japi_file_t f;
    if (!japi_fopen(&f, path, JAPI_READ)) {
        snprintf(s->open_msg, sizeof s->open_msg, "Cannot open %s", bn);
        return false;
    }
    int sz = japi_fsize(&f);
    if (sz < 0) {
        japi_fclose(&f);
        snprintf(s->open_msg, sizeof s->open_msg, "Cannot read %s", bn);
        return false;
    }
    if (sz > JBE_MAX_FILE_BYTES) {       /* refuse BEFORE any huge allocation */
        japi_fclose(&f);
        snprintf(s->open_msg, sizeof s->open_msg, "%s is too large to edit", bn);
        return false;
    }
    char *all = sz ? malloc(sz) : 0;
    if (sz && !all) {                    /* won't fit in RAM */
        japi_fclose(&f);
        snprintf(s->open_msg, sizeof s->open_msg, "%s is too large to edit", bn);
        return false;
    }
    int got = sz ? japi_fread(&f, all, sz) : 0;
    japi_fclose(&f);
    if (got < 0) {
        free(all);
        snprintf(s->open_msg, sizeof s->open_msg, "Cannot read %s", bn);
        return false;
    }
    /* A NUL byte means this is not a text file -- refuse it rather than fill the
       editor with unreadable bytes. */
    for (int i = 0; i < got; i++) {
        if (all[i] == '\0') {
            free(all);
            snprintf(s->open_msg, sizeof s->open_msg, "%s is not a text file", bn);
            return false;
        }
    }

    /* The file is good -- now COMMIT: detach a shared pane, drop the old
       document, and adopt the new one. From here nothing fails. */
    detach_active_pane(s);
    buffer_free(JBE_BUF(s));
    int keep = JBE_PANE(s)->buf_idx;
    memset(JBE_PANE(s), 0, sizeof(jbe_pane_t));
    JBE_PANE(s)->buf_idx = keep;
    strncpy(JBE_BUF(s)->filename, bn, JBE_NAME_MAX);
    JBE_BUF(s)->filename[JBE_NAME_MAX] = 0;
    strncpy(JBE_BUF(s)->path, path, JBE_PATH_MAX);
    JBE_BUF(s)->path[JBE_PATH_MAX] = 0;
    syntax_for_path(path, JBE_BUF(s)->syntax_name, sizeof JBE_BUF(s)->syntax_name);
    install_scheme(JBE_BUF(s), scheme_resolve(JBE_BUF(s)->syntax_name));

    /* Split on '\n'; strip a trailing '\r' so CRLF files render cleanly. */
    int start = 0;
    for (int i = 0; i < got; i++) {
        if (all[i] == '\n') {
            int end = i;
            if (end > start && all[end - 1] == '\r') end--;
            jbe_push_line(s, all + start, end - start);
            start = i + 1;
        }
    }
    if (start < got) jbe_push_line(s, all + start, got - start);  /* trailing line */
    if (JBE_BUF(s)->n_lines == 0) jbe_push_line(s, "", 0);        /* always >= 1 line */
    free(all);
    s->open_msg[0] = 0;                  /* success: clear any stale notice */
    return true;
}

/* Start a brand-new, unnamed buffer (File->New) in the active pane, replacing
   whatever it held. If the pane was sharing the other pane's file, detach it
   first so that file is left intact. No path, so Save falls through to Save As;
   the buffer holds one empty line and the pane's viewport resets. */
void jbe_new(jbe_state_t *s) {
    detach_active_pane(s);
    buffer_free(JBE_BUF(s));
    strcpy(JBE_BUF(s)->filename, "(untitled)");
    jbe_push_line(s, "", 0);
    int keep = JBE_PANE(s)->buf_idx;
    memset(JBE_PANE(s), 0, sizeof(jbe_pane_t));
    JBE_PANE(s)->buf_idx = keep;
    /* A fresh document carries no open-error notice. This also clears the boot
       case where the firmware tries A:scratch.txt (often absent -- normal) and
       falls back to jbe_new: the "Cannot open" message must not linger. */
    s->open_msg[0] = 0;
}

bool jbe_save(jbe_state_t *s) {
    if (!JBE_BUF(s)->path[0]) return false;             /* no path set (state not loaded) */
    /* static: keep the ~550-byte FatFs FIL inside japi_file_t off the ~2 KB
       Core 0 stack. Single-threaded, non-reentrant -> safe (see jbe_load). */
    static japi_file_t f;
    if (!japi_fopen(&f, JBE_BUF(s)->path, JAPI_WRITE)) return false;
    bool ok = true;
    const char nl = '\n';
    for (int i = 0; i < JBE_BUF(s)->n_lines && ok; i++) {
        int n = JBE_BUF(s)->len[i];
        if (n > 0 && japi_fwrite(&f, JBE_BUF(s)->lines[i], n) != n) ok = false;
        if (ok    && japi_fwrite(&f, &nl, 1) != 1)         ok = false;
    }
    japi_fclose(&f);
    if (ok) {
        /* Anchor the saved state to the current undo top, so undoing back
           to here flips dirty off again. saved_reachable resets to true
           even if a previous save's anchor had been evicted. */
        JBE_BUF(s)->saved_seq = (JBE_BUF(s)->undo.count > 0)
                     ? undo_stack_peek_newest(&JBE_BUF(s)->undo)->seq
                     : 0;
        JBE_BUF(s)->saved_reachable = true;
        JBE_BUF(s)->dirty = false;
    }
    return ok;
}

/* Close the active document. See jbe.h. Closing simply empties the active pane
   back to a fresh untitled buffer; jbe_new does exactly that, and (when the
   split is open and this pane was sharing the other pane's file) detaches first
   so the other pane keeps its file. The pane count is never changed here —
   removing a pane is View->Split's job. */
void jbe_close_active(jbe_state_t *s) {
    jbe_new(s);
}

/* --- File menu: Save As + Close -------------------------------------- */

/* Open the Save As filename prompt. Prefills the buffer's current path, or a
   bare "A:" drive prefix (the removable SD, the usual save target) so the user
   only has to type the name. */
static void save_as_open(jbe_state_t *s) {
    const char *cur = JBE_BUF(s)->path;
    if (cur[0]) {
        strncpy(s->save_as_name, cur, JBE_PATH_MAX);
        s->save_as_name[JBE_PATH_MAX] = 0;
    } else {
        strcpy(s->save_as_name, "A:");
    }
    s->save_as_len    = (int)strlen(s->save_as_name);
    s->save_as_active = true;
}

/* File→Save / Ctrl+S. A titled buffer saves straight to its path; an untitled
   one falls through to Save As instead of failing silently (the New→Save bug). */
static void save_or_save_as(jbe_state_t *s) {
    if (!JBE_BUF(s)->path[0]) { save_as_open(s); return; }
    /* Defer the write to the embedding loop (see save_request in jbe.h): the
       actual jbe_save() must not run on this deep key-handling stack. */
    s->save_request = true;
}

/* Enter in the Save As prompt: adopt the typed path as the buffer's path, set
   the basename as its display name, then save. */
static void save_as_commit(jbe_state_t *s) {
    s->save_as_active = false;
    if (s->save_as_len == 0) return;
    snprintf(JBE_BUF(s)->path, sizeof JBE_BUF(s)->path, "%s", s->save_as_name);
    const char *bn = s->save_as_name;          /* basename = after last '/' or ':' */
    for (const char *p = s->save_as_name; *p; p++)
        if (*p == '/' || *p == ':') bn = p + 1;
    int bl = (int)strlen(bn);
    if (bl > JBE_NAME_MAX) bl = JBE_NAME_MAX;   /* display name is just the tail */
    memcpy(JBE_BUF(s)->filename, bn, (size_t)bl);
    JBE_BUF(s)->filename[bl] = 0;
    /* Path + display name are adopted now (so the status bar updates at once),
       but the write itself is deferred to the embedding loop -- see save_request
       in jbe.h: jbe_save() off this deep stack would overflow the Core 0 stack. */
    s->save_request = true;
}

/* Routes a keystroke while the Save As prompt is open. */
static void save_as_route_key(jbe_state_t *s, uint16_t k) {
    switch (k) {
        case JAPI_KEY_ESCAPE: s->save_as_active = false; return;
        case JAPI_KEY_ENTER:  save_as_commit(s);         return;
        case JAPI_KEY_BACKSPACE:
            if (s->save_as_len > 0) s->save_as_name[--s->save_as_len] = 0;
            return;
        default:
            if (k >= 32 && k < 127 && s->save_as_len < JBE_PATH_MAX) {
                s->save_as_name[s->save_as_len++] = (char)k;
                s->save_as_name[s->save_as_len]   = 0;
            }
            return;
    }
}

/* Would closing the active pane actually destroy its document? Only if the
   buffer isn't also shown in the other pane (closing one of two shared views
   frees nothing). Drives whether the dirty-confirm prompt is needed. */
static bool active_doc_would_be_lost(const jbe_state_t *s) {
    if (!s->split_active) return true;          /* single pane: it IS the doc */
    int other = s->active_pane ^ 1;
    return s->panes[other].buf_idx != JBE_PANE(s)->buf_idx;
}

/* File→Close / Ctrl+W. Closes at once unless that would discard unsaved edits
   (the buffer is dirty AND not still shown in the other pane), in which case it
   first asks for confirmation (close_confirm). */
static void close_current(jbe_state_t *s) {
    if (active_doc_would_be_lost(s) && JBE_BUF(s)->dirty && !s->close_confirm) {
        s->close_confirm   = true;
        s->confirm_action  = JBE_CONFIRM_CLOSE;
        return;
    }
    s->close_confirm = false;
    s->close_request = true;   /* the embedding loop calls jbe_close_active() */
}

/* File→New / Ctrl+N. Starts a fresh document, but first guards the active one:
   if it has unsaved changes that would be lost, raise the shared confirm and
   only start New once the user agrees (see the close_confirm handler). */
static void request_new(jbe_state_t *s) {
    if (active_doc_would_be_lost(s) && JBE_BUF(s)->dirty && !s->close_confirm) {
        s->close_confirm  = true;
        s->confirm_action = JBE_CONFIRM_NEW;
        return;
    }
    s->new_request = true;     /* the embedding loop calls jbe_new() */
}

/* --- Edit primitives -------------------------------------------------- */

/* Resize line `row` to hold exactly `n` bytes (plus NUL). */
static bool line_set_len(jbe_state_t *s, int row, int n) {
    char *p = realloc(JBE_BUF(s)->lines[row], n + 1);
    if (!p) return false;
    p[n] = 0;
    JBE_BUF(s)->lines[row] = p;
    JBE_BUF(s)->len[row]   = n;
    return true;
}

/* Insert a fresh empty line slot at index `row`; shifts the rest down. */
static bool lines_insert_at(jbe_state_t *s, int row) {
    if (JBE_BUF(s)->n_lines == JBE_BUF(s)->cap_lines) {
        int nc = JBE_BUF(s)->cap_lines ? JBE_BUF(s)->cap_lines * 2 : 64;
        char **nl   = realloc(JBE_BUF(s)->lines, nc * sizeof *nl);
        int   *nlen = realloc(JBE_BUF(s)->len,   nc * sizeof *nlen);
        if (!nl || !nlen) { free(nl); free(nlen); return false; }
        JBE_BUF(s)->lines = nl; JBE_BUF(s)->len = nlen; JBE_BUF(s)->cap_lines = nc;
    }
    memmove(&JBE_BUF(s)->lines[row + 1], &JBE_BUF(s)->lines[row],
            (JBE_BUF(s)->n_lines - row) * sizeof *JBE_BUF(s)->lines);
    memmove(&JBE_BUF(s)->len[row + 1],   &JBE_BUF(s)->len[row],
            (JBE_BUF(s)->n_lines - row) * sizeof *JBE_BUF(s)->len);
    JBE_BUF(s)->lines[row] = 0; JBE_BUF(s)->len[row] = 0;
    JBE_BUF(s)->n_lines++;
    return true;
}

/* Remove line at index `row`; shifts the rest up. Frees the string. */
static void lines_remove_at(jbe_state_t *s, int row) {
    free(JBE_BUF(s)->lines[row]);
    memmove(&JBE_BUF(s)->lines[row], &JBE_BUF(s)->lines[row + 1],
            (JBE_BUF(s)->n_lines - row - 1) * sizeof *JBE_BUF(s)->lines);
    memmove(&JBE_BUF(s)->len[row],   &JBE_BUF(s)->len[row + 1],
            (JBE_BUF(s)->n_lines - row - 1) * sizeof *JBE_BUF(s)->len);
    JBE_BUF(s)->n_lines--;
}

/* Insert one printable byte at the cursor; cursor moves right.
 *
 * Undo coalescing (word-boundary mode — Word / Notepad++ / VS Code style):
 * consecutive word-character inserts on the same line, glued to the end of
 * the previous one, grow the same INSERT record in place. The first
 * non-word character (space, tab, punctuation) breaks the run, so one
 * Ctrl+Z undoes one word, the next Ctrl+Z undoes the separator, and so on.
 *
 * Any other mutation (Enter, Delete, Backspace, paste, selection-delete,
 * replace) pushes a non-coalescable record, naturally ending the run. */
static void edit_insert_char(jbe_state_t *s, char ch) {
    int row = JBE_PANE(s)->cur_row, col = JBE_PANE(s)->cur_col, n = JBE_BUF(s)->len[row];
    if (!line_set_len(s, row, n + 1)) return;
    char *L = JBE_BUF(s)->lines[row];
    memmove(&L[col + 1], &L[col], n - col);
    L[col] = ch;

    bool is_word = jbe_is_word_char((unsigned char)ch);
    jbe_undo_rec_t *top = undo_stack_peek_newest(&JBE_BUF(s)->undo);
    bool can_coalesce =
        is_word && top
        && top->kind == JBE_UNDO_INSERT
        && top->coalesce_typing
        && !top->is_block
        && top->row == row
        && top->col + top->text_len == col
        /* Must be the *immediately* next keypress — no cursor moves or other
           ops in between (even if they leave the cursor at the same col). */
        && top->last_kpress + 1 == JBE_BUF(s)->kpress_seq
        /* Don't coalesce into the saved-state record: that would move the
           save anchor and we could no longer return to a clean state. */
        && !(JBE_BUF(s)->saved_reachable && JBE_BUF(s)->saved_seq != 0 && top->seq == JBE_BUF(s)->saved_seq);

    if (can_coalesce) {
        char *nt = realloc(top->text, top->text_len + 1);
        if (nt) {
            nt[top->text_len] = ch;
            top->text     = nt;
            top->text_len++;
            JBE_BUF(s)->undo.bytes_used++;
            top->seq         = ++JBE_BUF(s)->seq_counter;  /* state changed → fresh seq */
            top->last_kpress = JBE_BUF(s)->kpress_seq;
            /* Still a user edit: invalidate any pending redo future. */
            while (JBE_BUF(s)->redo.count > 0) undo_stack_drop_oldest(s, &JBE_BUF(s)->redo);
            undo_enforce_budget(s);
            jbe_recompute_dirty(s);
        } else {
            /* realloc failed: fall back to a fresh record (no coalescing). */
            char *t = malloc(1);
            if (t) {
                t[0] = ch;
                undo_push(s, JBE_UNDO_INSERT, row, col, t, 1, row, col, false);
                jbe_undo_rec_t *new_top = undo_stack_peek_newest(&JBE_BUF(s)->undo);
                if (new_top) {
                    new_top->coalesce_typing = is_word;
                    new_top->last_kpress     = JBE_BUF(s)->kpress_seq;
                }
            }
        }
    } else {
        char *t = malloc(1);
        if (t) {
            t[0] = ch;
            undo_push(s, JBE_UNDO_INSERT, row, col, t, 1, row, col, false);
            jbe_undo_rec_t *new_top = undo_stack_peek_newest(&JBE_BUF(s)->undo);
            if (new_top) {
                new_top->coalesce_typing = is_word;
                new_top->last_kpress     = JBE_BUF(s)->kpress_seq;
            }
        }
    }
    JBE_PANE(s)->cur_col++;
    JBE_BUF(s)->dirty = true;
}

static void line_insert_bytes(jbe_state_t *s, int row, int col,
                              const char *src, int n);   /* defined below */

/* Split current line at cursor; rest becomes a new line below.
 * Pushes SPLIT onto the undo stack. */
static void edit_newline(jbe_state_t *s) {
    int row = JBE_PANE(s)->cur_row, col = JBE_PANE(s)->cur_col, n = JBE_BUF(s)->len[row];
    if (!lines_insert_at(s, row + 1)) return;
    int tail = n - col;
    if (!line_set_len(s, row + 1, tail)) { lines_remove_at(s, row + 1); return; }
    memcpy(JBE_BUF(s)->lines[row + 1], &JBE_BUF(s)->lines[row][col], tail);
    /* Truncate the current line; realloc-shrink is fine if it fails. */
    JBE_BUF(s)->lines[row][col] = 0;
    JBE_BUF(s)->len[row] = col;
    (void)line_set_len(s, row, col);   /* best-effort shrink */
    undo_push(s, JBE_UNDO_SPLIT, row, col, NULL, 0, row, col, false);
    JBE_PANE(s)->cur_row++; JBE_PANE(s)->cur_col = 0;

    /* Auto-indent: start the new line with the same leading whitespace as the
       line we just left, so code keeps its indentation. The source is the now
       truncated line[row], so splitting *inside* the indentation never copies
       more than what sat before the cursor. Pushed as its own INSERT record:
       one Ctrl+Z drops the indent, a second rejoins the line. */
    {
        const char *prev = JBE_BUF(s)->lines[row];
        int ind = 0;
        while (ind < JBE_BUF(s)->len[row] && (prev[ind] == ' ' || prev[ind] == '\t')) ind++;
        if (ind > 0) {
            line_insert_bytes(s, row + 1, 0, prev, ind);     /* copies the bytes */
            char *t = malloc((size_t)ind);
            if (t) {
                memcpy(t, JBE_BUF(s)->lines[row], (size_t)ind);
                undo_push(s, JBE_UNDO_INSERT, row + 1, 0, t, ind, row + 1, 0, false);
            }
            JBE_PANE(s)->cur_col = ind;
        }
    }
    JBE_BUF(s)->dirty = true;
}

/* Delete one char at the cursor (forward delete). Joins lines at end-of-line.
 * Pushes DELETE(1) for the in-line case or JOIN at (row, n) for the join. */
static void edit_delete(jbe_state_t *s) {
    int row = JBE_PANE(s)->cur_row, col = JBE_PANE(s)->cur_col, n = JBE_BUF(s)->len[row];
    if (col < n) {
        char *L = JBE_BUF(s)->lines[row];
        char  ch = L[col];
        memmove(&L[col], &L[col + 1], n - col - 1);
        (void)line_set_len(s, row, n - 1);
        char *t = malloc(1);
        if (t) { t[0] = ch; undo_push(s, JBE_UNDO_DELETE, row, col, t, 1, row, col, false); }
        JBE_BUF(s)->dirty = true;
    } else if (row + 1 < JBE_BUF(s)->n_lines) {
        int m = JBE_BUF(s)->len[row + 1];
        if (!line_set_len(s, row, n + m)) return;
        memcpy(&JBE_BUF(s)->lines[row][n], JBE_BUF(s)->lines[row + 1], m);
        lines_remove_at(s, row + 1);
        undo_push(s, JBE_UNDO_JOIN, row, n, NULL, 0, row, col, false);
        JBE_BUF(s)->dirty = true;
    }
}

/* Delete one char before the cursor (backspace). Joins at start-of-line.
 * Inlined (no longer delegates to edit_delete) so the undo record can
 * carry the cursor position from *before* the user pressed backspace. */
static void edit_backspace(jbe_state_t *s) {
    if (JBE_PANE(s)->cur_col > 0) {
        int row = JBE_PANE(s)->cur_row, col = JBE_PANE(s)->cur_col, n = JBE_BUF(s)->len[row];
        char *L = JBE_BUF(s)->lines[row];
        char  ch = L[col - 1];
        memmove(&L[col - 1], &L[col], n - col);
        (void)line_set_len(s, row, n - 1);
        char *t = malloc(1);
        if (t) { t[0] = ch; undo_push(s, JBE_UNDO_DELETE, row, col - 1, t, 1, row, col, false); }
        JBE_PANE(s)->cur_col--;
        JBE_BUF(s)->dirty = true;
    } else if (JBE_PANE(s)->cur_row > 0) {
        int prev   = JBE_PANE(s)->cur_row - 1;
        int prev_n = JBE_BUF(s)->len[prev];
        int n      = JBE_BUF(s)->len[JBE_PANE(s)->cur_row];
        if (!line_set_len(s, prev, prev_n + n)) return;
        memcpy(&JBE_BUF(s)->lines[prev][prev_n], JBE_BUF(s)->lines[JBE_PANE(s)->cur_row], n);
        int old_row = JBE_PANE(s)->cur_row;
        lines_remove_at(s, old_row);
        undo_push(s, JBE_UNDO_JOIN, prev, prev_n, NULL, 0, old_row, 0, false);
        JBE_PANE(s)->cur_row = prev;
        JBE_PANE(s)->cur_col = prev_n;
        JBE_BUF(s)->dirty = true;
    }
}

/* --- Selection & clipboard (stream mode, step 5a) -------------------- */

/* Normalise the selection range so (r1,c1) <= (r2,c2) in document order. */
static void sel_range(const jbe_state_t *s, int *r1, int *c1, int *r2, int *c2) {
    int a = JBE_PANE(s)->sel_row, b = JBE_PANE(s)->sel_col;
    int p = JBE_PANE(s)->cur_row, q = JBE_PANE(s)->cur_col;
    if (a < p || (a == p && b <= q)) { *r1 = a; *c1 = b; *r2 = p; *c2 = q; }
    else                              { *r1 = p; *c1 = q; *r2 = a; *c2 = b; }
}

/* Block-mode range: rectangle with sorted row and column bounds. Unlike
   stream-mode sel_range() this does NOT preserve document order — block
   selection is a Cartesian product of row range x column range. */
static void sel_range_block(const jbe_state_t *s, int *r1, int *c1, int *r2, int *c2) {
    int ra = JBE_PANE(s)->sel_row, ca = JBE_PANE(s)->sel_col;
    int rb = JBE_PANE(s)->cur_row, cb = JBE_PANE(s)->cur_col;
    *r1 = ra < rb ? ra : rb;  *r2 = ra > rb ? ra : rb;
    *c1 = ca < cb ? ca : cb;  *c2 = ca > cb ? ca : cb;
}

static bool in_selection(const jbe_state_t *s, int row, int col) {
    if (!JBE_PANE(s)->sel_active) return false;
    int r1, c1, r2, c2;
    if (JBE_PANE(s)->sel_block) {
        sel_range_block(s, &r1, &c1, &r2, &c2);
        return row >= r1 && row <= r2 && col >= c1 && col < c2;
    }
    sel_range(s, &r1, &c1, &r2, &c2);
    if (row < r1 || row > r2) return false;
    if (r1 == r2)              return col >= c1 && col < c2;
    if (row == r1)             return col >= c1;
    if (row == r2)             return col < c2;
    return true;
}

/* Begin a selection at the current cursor if none is active yet, and set the
   mode. Switching mode keeps the existing anchor — the user can flip between
   Shift+nav (stream) and Ctrl+Shift+nav (block) without losing it. */
static void sel_begin(jbe_state_t *s, bool block) {
    if (!JBE_PANE(s)->sel_active) {
        JBE_PANE(s)->sel_active = true;
        JBE_PANE(s)->sel_row    = JBE_PANE(s)->cur_row;
        JBE_PANE(s)->sel_col    = JBE_PANE(s)->cur_col;
    }
    JBE_PANE(s)->sel_block = block;
}

/* Select the whole buffer as a stream selection: anchor at the very start,
   cursor at the end of the last line. */
static void select_all(jbe_state_t *s) {
    jbe_pane_t *p = JBE_PANE(s);
    int last = JBE_BUF(s)->n_lines - 1;
    p->sel_active = true;
    p->sel_block  = false;
    p->sel_row    = 0;
    p->sel_col    = 0;
    p->cur_row    = last;
    p->cur_col    = JBE_BUF(s)->len[last];
}

/* Delete every byte inside the current selection. Cursor lands at the start
   of what used to be the selection. Leaves sel_active = false.
 * Captures the deleted bytes (multi-line, '\n'-separated) into one DELETE
 * undo record, so a single Ctrl+Z restores the entire selection. */
static void sel_delete(jbe_state_t *s) {
    if (!JBE_PANE(s)->sel_active) return;
    int cur_row_before = JBE_PANE(s)->cur_row, cur_col_before = JBE_PANE(s)->cur_col;
    if (JBE_PANE(s)->sel_block) {
        int r1, c1, r2, c2; sel_range_block(s, &r1, &c1, &r2, &c2);
        /* Capture deleted text first: per-row slice [c1, min(c2, len)) joined with \n. */
        int w = c2 - c1, n = 0;
        for (int r = r1; r <= r2; r++) {
            int rl = JBE_BUF(s)->len[r] > c1 ? JBE_BUF(s)->len[r] - c1 : 0;
            if (rl > w) rl = w;
            n += rl;
            if (r < r2) n++;
        }
        char *txt = n > 0 ? malloc(n) : NULL;
        if (n > 0 && !txt) return;       /* OOM: abort the whole op */
        int o = 0;
        for (int r = r1; r <= r2; r++) {
            int rl = JBE_BUF(s)->len[r] > c1 ? JBE_BUF(s)->len[r] - c1 : 0;
            if (rl > w) rl = w;
            if (rl > 0) memcpy(txt + o, JBE_BUF(s)->lines[r] + c1, rl);
            o += rl;
            if (r < r2) txt[o++] = '\n';
        }
        /* Now perform the actual cut. */
        for (int r = r1; r <= r2; r++) {
            int rn = JBE_BUF(s)->len[r];
            if (c1 >= rn) continue;
            int end = c2 > rn ? rn : c2;
            int rest = rn - end;
            memmove(JBE_BUF(s)->lines[r] + c1, JBE_BUF(s)->lines[r] + end, rest);
            (void)line_set_len(s, r, c1 + rest);
        }
        undo_push(s, JBE_UNDO_DELETE, r1, c1, txt, n,
                  cur_row_before, cur_col_before, true);
        JBE_PANE(s)->cur_row    = r1;
        JBE_PANE(s)->cur_col    = c1;
        JBE_PANE(s)->sel_active = false;
        JBE_BUF(s)->dirty      = true;
        return;
    }
    int r1, c1, r2, c2; sel_range(s, &r1, &c1, &r2, &c2);
    /* Capture deleted text: same layout as clip_copy_selection stream mode. */
    int n = 0;
    if (r1 == r2) n = c2 - c1;
    else {
        n = JBE_BUF(s)->len[r1] - c1;
        for (int i = r1 + 1; i < r2; i++) n += 1 + JBE_BUF(s)->len[i];
        n += 1 + c2;
    }
    char *txt = n > 0 ? malloc(n) : NULL;
    if (n > 0 && !txt) return;
    {
        int o = 0;
        if (r1 == r2) {
            memcpy(txt, JBE_BUF(s)->lines[r1] + c1, c2 - c1);
            o = c2 - c1;
        } else {
            int first_n = JBE_BUF(s)->len[r1] - c1;
            memcpy(txt + o, JBE_BUF(s)->lines[r1] + c1, first_n); o += first_n;
            for (int i = r1 + 1; i < r2; i++) {
                txt[o++] = '\n';
                memcpy(txt + o, JBE_BUF(s)->lines[i], JBE_BUF(s)->len[i]); o += JBE_BUF(s)->len[i];
            }
            txt[o++] = '\n';
            memcpy(txt + o, JBE_BUF(s)->lines[r2], c2); o += c2;
        }
        (void)o;
    }
    /* Trim line r1 to its prefix [0..c1) and append the tail of r2 from c2. */
    int tail_len = JBE_BUF(s)->len[r2] - c2;
    char *new_first = malloc(c1 + tail_len + 1);
    if (!new_first) { free(txt); return; }
    memcpy(new_first,        JBE_BUF(s)->lines[r1],         c1);
    memcpy(new_first + c1,   JBE_BUF(s)->lines[r2] + c2,    tail_len);
    new_first[c1 + tail_len] = 0;
    free(JBE_BUF(s)->lines[r1]);
    JBE_BUF(s)->lines[r1] = new_first;
    JBE_BUF(s)->len[r1]   = c1 + tail_len;
    /* Drop r1+1..r2 (inclusive). */
    for (int i = r1 + 1; i <= r2; i++) free(JBE_BUF(s)->lines[i]);
    int removed = r2 - r1;
    if (removed > 0) {
        memmove(&JBE_BUF(s)->lines[r1 + 1], &JBE_BUF(s)->lines[r2 + 1],
                (JBE_BUF(s)->n_lines - r2 - 1) * sizeof *JBE_BUF(s)->lines);
        memmove(&JBE_BUF(s)->len[r1 + 1],   &JBE_BUF(s)->len[r2 + 1],
                (JBE_BUF(s)->n_lines - r2 - 1) * sizeof *JBE_BUF(s)->len);
        JBE_BUF(s)->n_lines -= removed;
    }
    undo_push(s, JBE_UNDO_DELETE, r1, c1, txt, n,
              cur_row_before, cur_col_before, false);
    JBE_PANE(s)->cur_row   = r1;
    JBE_PANE(s)->cur_col   = c1;
    JBE_PANE(s)->sel_active = false;
    JBE_BUF(s)->dirty      = true;
}

/* Copy the selection into the clipboard. Stream mode: lines joined with \n.
   Block mode: per-row slice [c1, min(c2, len)) joined with \n; clip_block
   tells clip_paste it should be inserted column-wise. */
static void clip_copy_selection(jbe_state_t *s) {
    if (!JBE_PANE(s)->sel_active) return;
    if (JBE_PANE(s)->sel_block) {
        int r1, c1, r2, c2; sel_range_block(s, &r1, &c1, &r2, &c2);
        int w = c2 - c1;
        int n = 0;
        for (int r = r1; r <= r2; r++) {
            int row_n = JBE_BUF(s)->len[r] > c1 ? JBE_BUF(s)->len[r] - c1 : 0;
            if (row_n > w) row_n = w;
            n += row_n;
            if (r < r2) n++;                       /* \n separator */
        }
        char *buf = malloc(n + 1);
        if (!buf) return;
        int o = 0;
        for (int r = r1; r <= r2; r++) {
            int row_n = JBE_BUF(s)->len[r] > c1 ? JBE_BUF(s)->len[r] - c1 : 0;
            if (row_n > w) row_n = w;
            memcpy(buf + o, JBE_BUF(s)->lines[r] + c1, row_n); o += row_n;
            if (r < r2) buf[o++] = '\n';
        }
        buf[o] = 0;
        free(s->clip);
        s->clip       = buf;
        s->clip_len   = o;
        s->clip_block = true;
        return;
    }
    int r1, c1, r2, c2; sel_range(s, &r1, &c1, &r2, &c2);
    /* Size up. */
    int n = 0;
    if (r1 == r2) n = c2 - c1;
    else {
        n = JBE_BUF(s)->len[r1] - c1;
        for (int i = r1 + 1; i < r2; i++) n += 1 + JBE_BUF(s)->len[i];
        n += 1 + c2;
    }
    char *buf = malloc(n + 1);
    if (!buf) return;
    int o = 0;
    if (r1 == r2) {
        memcpy(buf, JBE_BUF(s)->lines[r1] + c1, c2 - c1);
        o = c2 - c1;
    } else {
        int first_n = JBE_BUF(s)->len[r1] - c1;
        memcpy(buf + o, JBE_BUF(s)->lines[r1] + c1, first_n); o += first_n;
        for (int i = r1 + 1; i < r2; i++) {
            buf[o++] = '\n';
            memcpy(buf + o, JBE_BUF(s)->lines[i], JBE_BUF(s)->len[i]); o += JBE_BUF(s)->len[i];
        }
        buf[o++] = '\n';
        memcpy(buf + o, JBE_BUF(s)->lines[r2], c2); o += c2;
    }
    buf[o] = 0;
    free(s->clip);
    s->clip       = buf;
    s->clip_len   = o;
    s->clip_block = false;
}

/* Set the text clipboard to a copy of a NUL-terminated string (stream mode), so
   another mode (e.g. the calculator) can hand a value to the editor's paste. */
void jbe_clip_set(jbe_state_t *s, const char *text) {
    int n = (int)strlen(text);
    char *buf = malloc((size_t)n + 1);
    if (!buf) return;
    memcpy(buf, text, (size_t)n + 1);
    free(s->clip);
    s->clip       = buf;
    s->clip_len   = n;
    s->clip_block = false;
}

/* Insert a single segment (no newlines) at (row, col). */
static void line_insert_bytes(jbe_state_t *s, int row, int col,
                              const char *src, int n) {
    int old = JBE_BUF(s)->len[row];
    if (!line_set_len(s, row, old + n)) return;
    char *L = JBE_BUF(s)->lines[row];
    memmove(&L[col + n], &L[col], old - col);
    memcpy (&L[col],     src,     n);
}

/* Edit->Toggle Comment: put or remove a BASIC comment marker (') at the very
 * start (column 0, no indentation) of either the current line or, when a
 * selection is active, every row the selection touches. A one-keystroke way to
 * disable/enable code while debugging.
 *
 * Block rule (matches modern editors): if every non-empty row in the range
 * already starts with ' the whole block is uncommented, otherwise the whole
 * block is commented. Empty rows are left untouched. The cursor stays on the
 * same character; the selection is cleared afterwards (its columns would shift).
 * Each row is its own undo step, so undoing a block toggle takes one Ctrl+Z per
 * changed row. */
static void toggle_comment(jbe_state_t *s) {
    int r1, r2;
    if (JBE_PANE(s)->sel_active) {               /* any row the selection touches */
        int a = JBE_PANE(s)->sel_row, b = JBE_PANE(s)->cur_row;
        r1 = a < b ? a : b;
        r2 = a < b ? b : a;
    } else {
        r1 = r2 = JBE_PANE(s)->cur_row;
    }

    /* Decide direction: uncomment only when every non-empty row is commented. */
    bool any = false, all_commented = true;
    for (int r = r1; r <= r2; r++) {
        if (JBE_BUF(s)->len[r] > 0) {
            any = true;
            if (JBE_BUF(s)->lines[r][0] != '\'') { all_commented = false; break; }
        }
    }
    bool uncomment = any && all_commented;

    int cur_row = JBE_PANE(s)->cur_row, cur_col = JBE_PANE(s)->cur_col;
    for (int r = r1; r <= r2; r++) {
        int n = JBE_BUF(s)->len[r];
        char *L = JBE_BUF(s)->lines[r];
        if (uncomment) {
            if (n > 0 && L[0] == '\'') {         /* remove the leading ' */
                char ch = L[0];
                memmove(&L[0], &L[1], (size_t)(n - 1));
                (void)line_set_len(s, r, n - 1);
                char *t = malloc(1);
                if (t) { t[0] = ch; undo_push(s, JBE_UNDO_DELETE, r, 0, t, 1, r, 0, false); }
                if (r == cur_row && cur_col > 0) cur_col--;
            }
        } else if (n > 0) {                      /* comment non-empty rows only */
            line_insert_bytes(s, r, 0, "'", 1);
            char *t = malloc(1);
            if (t) { t[0] = '\''; undo_push(s, JBE_UNDO_INSERT, r, 0, t, 1, r, 0, false); }
            if (r == cur_row) cur_col++;
        }
    }
    JBE_PANE(s)->cur_col   = cur_col;
    JBE_PANE(s)->sel_active = false;             /* selection columns would be stale */
    JBE_BUF(s)->dirty = true;
}

#define JBE_TAB_WIDTH 2     /* soft-tab width: spaces, never a real tab char */

/* Tab with no selection: insert spaces up to the next tab stop at the cursor. */
static void edit_tab(jbe_state_t *s) {
    int col = JBE_PANE(s)->cur_col;
    int n = JBE_TAB_WIDTH - (col % JBE_TAB_WIDTH);   /* 1..JBE_TAB_WIDTH */
    for (int i = 0; i < n; i++) edit_insert_char(s, ' ');
}

/* Indent (dedent=false) or dedent (true) every row the cursor or selection
   covers, by one tab width at column 0. One undo step per changed row. The
   selection is kept (columns shifted) so repeated Tab/Shift+Tab keeps moving the
   same block; empty rows are not indented and have nothing to dedent. */
static void indent_block(jbe_state_t *s, bool dedent) {
    int r1, r2;
    if (JBE_PANE(s)->sel_active) {
        int a = JBE_PANE(s)->sel_row, b = JBE_PANE(s)->cur_row;
        r1 = a < b ? a : b; r2 = a < b ? b : a;
    } else {
        r1 = r2 = JBE_PANE(s)->cur_row;
    }
    int cur_row = JBE_PANE(s)->cur_row, cur_col = JBE_PANE(s)->cur_col;
    int sel_row = JBE_PANE(s)->sel_row, sel_col = JBE_PANE(s)->sel_col;
    for (int r = r1; r <= r2; r++) {
        int n = JBE_BUF(s)->len[r];
        char *L = JBE_BUF(s)->lines[r];
        if (dedent) {
            int rm = 0;
            while (rm < JBE_TAB_WIDTH && rm < n && L[rm] == ' ') rm++;
            if (rm > 0) {
                char *t = malloc((size_t)rm);
                if (t) memcpy(t, L, (size_t)rm);
                memmove(&L[0], &L[rm], (size_t)(n - rm));
                (void)line_set_len(s, r, n - rm);
                if (t) undo_push(s, JBE_UNDO_DELETE, r, 0, t, rm, r, 0, false);
                if (r == cur_row) cur_col = cur_col > rm ? cur_col - rm : 0;
                if (r == sel_row) sel_col = sel_col > rm ? sel_col - rm : 0;
            }
        } else if (n > 0) {                          /* don't indent empty rows */
            char sp[JBE_TAB_WIDTH];
            for (int i = 0; i < JBE_TAB_WIDTH; i++) sp[i] = ' ';
            line_insert_bytes(s, r, 0, sp, JBE_TAB_WIDTH);
            char *t = malloc(JBE_TAB_WIDTH);
            if (t) { for (int i = 0; i < JBE_TAB_WIDTH; i++) t[i] = ' ';
                     undo_push(s, JBE_UNDO_INSERT, r, 0, t, JBE_TAB_WIDTH, r, 0, false); }
            if (r == cur_row) cur_col += JBE_TAB_WIDTH;
            if (r == sel_row) sel_col += JBE_TAB_WIDTH;
        }
    }
    JBE_PANE(s)->cur_col = cur_col;
    JBE_PANE(s)->sel_col = sel_col;
    JBE_BUF(s)->dirty = true;
}

/* Paste the clipboard at the cursor. Format depends on clip_block:
   - stream: split on \n, splice each segment into the current line, tail of
     the line re-attaches after the last segment.
   - block: each \n-separated segment is inserted on row cur_row+i at column
     cur_col; rows beyond n_lines are appended as new empty lines; rows
     shorter than cur_col are padded with spaces. */
static void clip_paste(jbe_state_t *s) {
    if (!s->clip || s->clip_len == 0) return;
    if (JBE_PANE(s)->sel_active) sel_delete(s);   /* pushes its own DELETE record */

    /* Snapshot the clipboard for the undo record: clip itself can be
       overwritten by a subsequent copy before the user hits Ctrl+Z. */
    char *snapshot   = malloc(s->clip_len);
    int   snap_len   = s->clip_len;
    bool  snap_block = s->clip_block;
    int   snap_row   = JBE_PANE(s)->cur_row;
    int   snap_col   = JBE_PANE(s)->cur_col;
    if (snapshot) memcpy(snapshot, s->clip, s->clip_len);

    if (s->clip_block) {
        int start_row = JBE_PANE(s)->cur_row, start_col = JBE_PANE(s)->cur_col;
        int n_before  = JBE_BUF(s)->n_lines;
        /* Pre-count segments so we can size pad_orig_len[] once. */
        int seg_count = 1;
        for (int i = 0; i < s->clip_len; i++) if (s->clip[i] == '\n') seg_count++;
        int16_t *pad_orig = malloc((size_t)seg_count * sizeof(int16_t));
        int      grew     = 0;
        bool     any_meta = false;

        int seg_start = 0, seg_idx = 0;
        int last_len  = 0;
        for (int i = 0; i <= s->clip_len; i++) {
            if (i == s->clip_len || s->clip[i] == '\n') {
                int seg_len = i - seg_start;
                int target  = start_row + seg_idx;
                int16_t meta = -1;
                if (target >= n_before) {
                    /* Row created beyond original EOF — undo must drop it. */
                    grew++;
                    any_meta = true;
                } else if (JBE_BUF(s)->len[target] < start_col) {
                    /* Row was shorter than start_col — record its original
                       length so undo can strip the padding spaces. */
                    meta = (int16_t)JBE_BUF(s)->len[target];
                    any_meta = true;
                }
                if (pad_orig) pad_orig[seg_idx] = meta;

                while (target >= JBE_BUF(s)->n_lines) lines_insert_at(s, JBE_BUF(s)->n_lines);
                if (JBE_BUF(s)->len[target] < start_col) {
                    int old = JBE_BUF(s)->len[target];
                    if (line_set_len(s, target, start_col)) {
                        memset(JBE_BUF(s)->lines[target] + old, ' ', start_col - old);
                    }
                }
                line_insert_bytes(s, target, start_col, s->clip + seg_start, seg_len);
                last_len = seg_len;
                seg_idx++;
                seg_start = i + 1;
            }
        }
        JBE_PANE(s)->cur_row = start_row + (seg_idx > 0 ? seg_idx - 1 : 0);
        JBE_PANE(s)->cur_col = start_col + last_len;
        if (snapshot) {
            undo_push(s, JBE_UNDO_INSERT, snap_row, snap_col,
                      snapshot, snap_len, snap_row, snap_col, snap_block);
            if (any_meta && pad_orig) {
                jbe_undo_rec_t *top = undo_stack_peek_newest(&JBE_BUF(s)->undo);
                if (top) {
                    top->grew_lines   = (int16_t)grew;
                    top->pad_count    = (int16_t)seg_count;
                    top->pad_orig_len = pad_orig;
                    JBE_BUF(s)->undo.bytes_used += seg_count * (int)sizeof(int16_t);
                    undo_enforce_budget(s);
                    pad_orig = NULL;   /* ownership transferred to the record */
                }
            }
        }
        free(pad_orig);                /* no-op if NULL (already attached) */
        JBE_BUF(s)->dirty = true;
        return;
    }

    /* Walk segments separated by '\n'. */
    int start = 0;
    int first = 1;
    int tail_row = JBE_PANE(s)->cur_row;
    int tail_col_in_old = JBE_PANE(s)->cur_col;
    int old_tail_len    = JBE_BUF(s)->len[tail_row] - tail_col_in_old;
    /* Save and remove the tail so we can append after the last paste segment. */
    char *tail = 0;
    if (old_tail_len > 0) {
        tail = malloc(old_tail_len);
        memcpy(tail, JBE_BUF(s)->lines[tail_row] + tail_col_in_old, old_tail_len);
        (void)line_set_len(s, tail_row, tail_col_in_old);
    } else {
        JBE_BUF(s)->len[tail_row] = tail_col_in_old;
    }
    for (int i = 0; i <= s->clip_len; i++) {
        if (i == s->clip_len || s->clip[i] == '\n') {
            int seg_len = i - start;
            if (first) {
                /* Append to current line at cursor. */
                line_insert_bytes(s, JBE_PANE(s)->cur_row, JBE_PANE(s)->cur_col,
                                  s->clip + start, seg_len);
                JBE_PANE(s)->cur_col += seg_len;
                first = 0;
            } else {
                /* New line below cursor with this segment. */
                lines_insert_at(s, JBE_PANE(s)->cur_row + 1);
                if (line_set_len(s, JBE_PANE(s)->cur_row + 1, seg_len))
                    memcpy(JBE_BUF(s)->lines[JBE_PANE(s)->cur_row + 1], s->clip + start, seg_len);
                JBE_PANE(s)->cur_row++;
                JBE_PANE(s)->cur_col = seg_len;
            }
            start = i + 1;
        }
    }
    /* Re-attach the original tail of the line we cut, after the last paste. */
    if (old_tail_len > 0) {
        line_insert_bytes(s, JBE_PANE(s)->cur_row, JBE_PANE(s)->cur_col, tail, old_tail_len);
        free(tail);
    }
    if (snapshot)
        undo_push(s, JBE_UNDO_INSERT, snap_row, snap_col,
                  snapshot, snap_len, snap_row, snap_col, snap_block);
    JBE_BUF(s)->dirty = true;
}

/* --- Navigation ------------------------------------------------------- */

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* How many screen rows one logical line occupies under char-wrap.
   Always at least 1 (an empty line still takes one row). */
static int visual_rows_of(int len) {
    if (len <= 0) return 1;
    return (len + JBE_WRAP_WIDTH - 1) / JBE_WRAP_WIDTH;
}

/* Visual sub-row and visual column for a cursor at byte `col` in its line.
   The boundary case `col > 0 && col % W == 0` is mapped to the END of the
   previous sub-row (visual col = W) instead of the start of a non-existent
   next sub-row — keeps the cursor on the wrap-marker cell, which always
   exists on wrapped sub-rows. */
static void cursor_visual_pos(int col, int *sub, int *vcol) {
    int W = JBE_WRAP_WIDTH;
    if (col > 0 && col % W == 0) { *sub = col / W - 1; *vcol = W; }
    else                         { *sub = col / W;     *vcol = col - *sub * W; }
}

/* Total visual rows in lines [from..to). O(to-from); the file ranges we
   walk during cursor-follow are small. */
static int visual_rows_range(const jbe_state_t *s, int from, int to) {
    int v = 0;
    for (int i = from; i < to; i++) v += visual_rows_of(JBE_BUF(s)->len[i]);
    return v;
}

/* Keep the cursor inside the viewport. Vertical only — horizontal scrolling
   is gone: long lines wrap onto the next visual row instead. */
static void jbe_follow_cursor(jbe_state_t *s) {
    if (JBE_PANE(s)->cur_row < JBE_PANE(s)->top_row) JBE_PANE(s)->top_row = JBE_PANE(s)->cur_row;
    int cur_sub, cur_vcol; cursor_visual_pos(JBE_PANE(s)->cur_col, &cur_sub, &cur_vcol);
    /* Advance top_row until cur_row's visual position fits on screen. */
    while (JBE_PANE(s)->top_row < JBE_PANE(s)->cur_row) {
        int v = visual_rows_range(s, JBE_PANE(s)->top_row, JBE_PANE(s)->cur_row);
        if (v + cur_sub + 1 <= JBE_VIEW_HEIGHT) break;
        JBE_PANE(s)->top_row++;
    }
    if (JBE_PANE(s)->top_row < 0) JBE_PANE(s)->top_row = 0;
}

/* --- Menu bar definitions (step 7) ----------------------------------- */

/* Menu titles shown on the title row. The accelerator letter is the first
   upper-case letter of the title; drawn in a contrasting colour because
   plain VGA text has no underline attribute. */
/* Menu order is deliberate: File/Edit/View/Search/Macro/Options. View (the
   split/layout menu) sits right after Edit, the classic QBasic place for a
   View menu; Macro sits between Search and Options so the static-content menus
   stay grouped left of the (dynamic) Options menu. The accelerator letters are
   all unique on their first letter, no collisions. */
static const char *JBE_MENU_TITLES[] = {
    "File", "Edit", "View", "Search", "Macro", "Options", "Run", "Help"
};
/* The Run menu is always present: it carries the Japi Commander, which ships
   with the editor. Only the Basic Interpreter entry *inside* Run is JBB-only,
   because the interpreter is a separate product the host app supplies. */
#define JBE_MENU_COUNT 8
#define JBE_MENU_ACCEL_FG  VGA_RED       /* accelerator letter */
#define JBE_DROP_BG        VGA_CYAN      /* dropdown background */
#define JBE_DROP_FG        VGA_BLACK     /* dropdown text       */
#define JBE_DROP_HI_BG     VGA_BLACK    /* highlighted item bg */
#define JBE_DROP_HI_FG     VGA_YELLOW   /* highlighted item fg */

/* Menu-item type is now declared in jbe.h as jbe_menu_item_t so it can
   also back the dynamic Options table on jbe_state_t. Alias kept for
   readability in this file. NULL label terminates a menu's array. */
typedef jbe_menu_item_t menu_item_t;

/* A dropdown separator: a horizontal line, not a selectable item. Marked by a
   sentinel label (\x01) so it needs no new struct field. Navigation skips it,
   it has no accelerator, and the dispatch indices count it like any row. */
#define JBE_MENU_SEP { "\x01", 0, 0 }
static bool menu_item_is_sep(const menu_item_t *it) {
    return it->label && it->label[0] == '\x01';
}

/* No "Exit": the editor is the whole machine -- there is nowhere to exit to.
   (On the host simulator, Ctrl+Q still quits the process so the terminal is
   restored; that is a host convenience, not a feature of the machine.) */
static const menu_item_t MENU_FILE[]    = {
    {"New",        'N', "Ctrl+N"},
    {"Open...",    'O', "Ctrl+O"},
    {"Save",       'S', "Ctrl+S"},
    {"Save As...", 'A', 0},
    {"Close",      'C', "Ctrl+W"},
    {0,0,0}
};
static const menu_item_t MENU_EDIT[]    = {
    {"Cut",   'T', "Ctrl+X"},
    {"Copy",  'C', "Ctrl+C"},
    {"Paste", 'P', "Ctrl+V"},
    {"Select All", 'A', "Ctrl+A"},
    JBE_MENU_SEP,
    {"Toggle Comment", 'G', "Ctrl+G"},
    {"Format", 'F', 0},
    JBE_MENU_SEP,
    {"Undo",  'U', "Ctrl+Z"},
    {"Redo",  'R', "Ctrl+Y"},
    {0,0,0}
};
static const menu_item_t MENU_SEARCH[]  = {
    {"Find...",      'F', "Ctrl+F"},
    {"Replace...",   'R', "Ctrl+R"},
    {"Go to Line...",'G', "Ctrl+L"},
    {0,0,0}
};
/* Macro menu: explicit entries so a menu = capability inventory. Record
   start/stop toggles recording (one item), Play replays the recorded macro.
   Ctrl+M is *not* used because the keyboard layer maps it to Enter (50-year-old
   ASCII convention), so the shortcuts are Ctrl+T (recoRd) and Ctrl+P (Play). */
static const menu_item_t MENU_MACRO[]  = {
    {"Record start/stop", 'R', "Ctrl+T"},
    {"Play",              'P', "Ctrl+P"},
    {0,0,0}
};
/* MENU_OPTIONS is no longer static — it lives on jbe_state_t.options_items
   and is rebuilt every time the Options menu is opened (see
   rebuild_options_menu in this file). */
static const menu_item_t MENU_VIEW[]  = {
    {"Toggle windowed view", 'W', 0      },   /* no shortcut (Jan, 2026-06-13) */
    {"Other pane",       'O', "Ctrl+Tab"},
    /* Directional accelerators U/D (not the menu's own open-letter V).
       \x18 / \x19 are the CP437 up/down arrow glyphs; the bindings are the
       real Ctrl+Up / Ctrl+Down keys (see jbe_handle_key). */
    {"Move divider up",  'U', "Ctrl+\x18"},
    {"Move divider down",'D', "Ctrl+\x19"},
    {0,0,0}
};
/* Run menu. The Japi Commander ships with the editor, so it is always here.
   The Basic Interpreter entry is added only when the editor is built into the
   full Japi Base Computer (JBB_RUN), which is what supplies the interpreter --
   the editor itself contains none. */
static const menu_item_t MENU_RUN[] = {
#ifdef JBB_RUN
    {"Basic Interpreter", 'B', "Ctrl+B"},
#endif
    {"Japi Commander",    'J', "Ctrl+J"},
    {0,0,0}
};

/* Help menu: makes the built-in F1 help discoverable from the menu bar. */
static const menu_item_t MENU_HELP[] = {
    {"Editor Help", 'E', "F1"},
    {0,0,0}
};
/* Returns the menu items for title `m` on this editor state. Slot 5
   (Options) is dynamic — its array lives in s->options_items, rebuilt
   on every menu_open. The other slots are static. */
static const menu_item_t *menu_items_for(const jbe_state_t *s, int m) {
    static const menu_item_t *const STATIC_MENUS[JBE_MENU_COUNT] = {
        MENU_FILE, MENU_EDIT, MENU_VIEW, MENU_SEARCH, MENU_MACRO, 0, MENU_RUN,
        MENU_HELP
    };
    if (m == 5) return s->options_items;   /* Options is dynamic */
    return STATIC_MENUS[m];
}

static int menu_item_count(const jbe_state_t *s, int m) {
    const menu_item_t *items = menu_items_for(s, m);
    int n = 0; while (items[n].label) n++; return n;
}
static int menu_item_maxlen(const jbe_state_t *s, int m) {
    const menu_item_t *items = menu_items_for(s, m);
    int n = 0;
    for (int i = 0; items[i].label; i++) {
        int L = (int)strlen(items[i].label);
        if (items[i].hint) L += 3 + (int)strlen(items[i].hint);
        if (L > n) n = L;
    }
    return n;
}
static int menu_title_col(int m) {
    int col = 2;
    for (int i = 0; i < m; i++) col += (int)strlen(JBE_MENU_TITLES[i]) + 3;
    return col;
}

/* Find a menu by accelerator letter (case-insensitive). Returns 0..N-1 or -1. */
static int menu_idx_for_letter(char up) {
    for (int m = 0; m < JBE_MENU_COUNT; m++)
        if (JBE_MENU_TITLES[m][0] == up) return m;
    return -1;
}

/* Find an item in the active menu by its declared accelerator letter. */
static int item_idx_for_letter(const jbe_state_t *s, int m, char up) {
    const menu_item_t *items = menu_items_for(s, m);
    for (int i = 0; items[i].label; i++) {
        char c = items[i].accel;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == up) return i;
    }
    return -1;
}

/* Forward decl — rebuild_options_menu walks JBE_BUILTIN_SCHEMES + the
   floppy. Defined further down (it needs scheme_by_name + the parser). */
static void rebuild_options_menu(jbe_state_t *s);

static void menu_open(jbe_state_t *s, int m) {
    if (m == 5) rebuild_options_menu(s);  /* Options: fresh list every open */
    s->menu_active = true;
    s->menu_idx    = m;
    s->item_idx    = 0;
}
static void menu_close(jbe_state_t *s) { s->menu_active = false; }

/* ---- F1 Help helpers (the viewer render_help lives near jbe_render) -------
   Content + the id->line anchor table come from help_text.h. */
#define HELP_VIS_ROWS 48          /* visible content rows in the 52-tall window */

/* Map a help id ("menu:File", "item:Edit/Select All") to a line, or -1. */
static int help_anchor_line(const char *id) {
    if (!id) return -1;
    for (int i = 0; HELP_ANCHORS[i].id; i++)
        if (strcmp(HELP_ANCHORS[i].id, id) == 0) return HELP_ANCHORS[i].line;
    return -1;
}

/* Largest valid scroll offset so the final page is fully shown. */
static int help_max_top(void) {
    int m = HELP_NLINES - HELP_VIS_ROWS;
    return m < 0 ? 0 : m;
}

/* Open the help window scrolled to `id` (NULL or unknown -> the top). */
static void help_open(jbe_state_t *s, const char *id) {
    int line = help_anchor_line(id);
    if (line < 0) line = 0;
    if (line > help_max_top()) line = help_max_top();
    s->menu_active = false;        /* help takes over; close any open menu */
    s->help_active = true;
    s->help_top    = line;
}

/* F1 from the editor or an open menu: jump to the matching help section. The
   label normalisation (drop a trailing "...") mirrors tools/mkhelp.py so the
   ids match; an item with no anchor falls back to its menu's section. */
static void help_open_context(jbe_state_t *s) {
    if (!s->menu_active) { help_open(s, NULL); return; }
    const char *title = JBE_MENU_TITLES[s->menu_idx];
    const menu_item_t *it = &menu_items_for(s, s->menu_idx)[s->item_idx];
    char id[96];
    if (it->label && !menu_item_is_sep(it)) {
        char lab[48];
        snprintf(lab, sizeof lab, "%s", it->label);
        int n = (int)strlen(lab);
        while (n >= 3 && lab[n-1]=='.' && lab[n-2]=='.' && lab[n-3]=='.') { n -= 3; lab[n] = 0; }
        while (n > 0 && lab[n-1] == ' ') lab[--n] = 0;
        snprintf(id, sizeof id, "item:%s/%s", title, lab);
        if (help_anchor_line(id) >= 0) { help_open(s, id); return; }
    }
    snprintf(id, sizeof id, "menu:%s", title);
    help_open(s, id);
}

/* Keys while the help window is open (fully modal). */
static void help_handle_key(jbe_state_t *s, uint16_t k) {
    int maxt = help_max_top();
    switch (k) {
        case JAPI_KEY_UP:    s->help_top -= 1;             break;
        case JAPI_KEY_DOWN:  s->help_top += 1;             break;
        case JAPI_KEY_PGUP:  s->help_top -= HELP_VIS_ROWS; break;
        case JAPI_KEY_PGDN:  s->help_top += HELP_VIS_ROWS; break;
        case JAPI_KEY_HOME:  s->help_top  = 0;             break;
        case JAPI_KEY_END:   s->help_top  = maxt;          break;
        case JAPI_KEY_ESCAPE:
        case JAPI_KEY_F1:    s->help_active = false;       return;
        default:                                           return;
    }
    if (s->help_top < 0)    s->help_top = 0;
    if (s->help_top > maxt) s->help_top = maxt;
}

/* ---- Options -> CPU speed chooser (render_cpu_dialog lives near render_help) */
static const int CPU_TIERS[3] = { 260, 324, 390 };

static int cpu_tier_index(int mhz) {
    for (int i = 0; i < 3; i++) if (CPU_TIERS[i] == mhz) return i;
    return 1;   /* default to the 324 MHz tier */
}

static void cpu_open(jbe_state_t *s) {
    s->cpu_dialog_active = true;
    s->cpu_sel = cpu_tier_index(japi_get_cpu_clock_mhz());
}

static void cpu_handle_key(jbe_state_t *s, uint16_t k) {
    switch (k) {
        case JAPI_KEY_UP:    if (s->cpu_sel > 0) s->cpu_sel--; break;
        case JAPI_KEY_DOWN:  if (s->cpu_sel < 2) s->cpu_sel++; break;
        case JAPI_KEY_ENTER:
            /* Persist the choice and reboot to apply. On hardware this does not
               return; in the host simulator it is a no-op, so just close. */
            japi_set_cpu_clock(CPU_TIERS[s->cpu_sel]);
            s->cpu_dialog_active = false;
            break;
        case JAPI_KEY_ESCAPE: s->cpu_dialog_active = false; break;
        default: break;
    }
}

/* --- Find (step 8a) -------------------------------------------------- */

/* Case-insensitive char compare. */
static int ci_eq(char a, char b) {
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    return a == b;
}

/* Is the query found at (line[col..]) ? Returns 1 if it fits and matches. */
static int find_match_at(const char *line, int len, int col,
                         const char *q, int qn) {
    if (col + qn > len) return 0;
    for (int i = 0; i < qn; i++) if (!ci_eq(line[col + i], q[i])) return 0;
    return 1;
}

/* Find the next match of the query starting at (start_row, start_col), with
   wrap-around back to (0,0). Sets *r, *c on success, returns true. */
static bool find_search(const jbe_state_t *s, int start_row, int start_col,
                        int *r, int *c) {
    int qn = JBE_PANE(s)->find_query_len;
    if (qn <= 0 || JBE_BUF(s)->n_lines == 0) return false;
    int rows = JBE_BUF(s)->n_lines;
    for (int pass = 0; pass < 2; pass++) {
        int rfrom = (pass == 0) ? start_row : 0;
        int rto   = (pass == 0) ? rows     : (start_row + 1);
        for (int row = rfrom; row < rto; row++) {
            int col0 = (pass == 0 && row == start_row) ? start_col : 0;
            int line_len = JBE_BUF(s)->len[row];
            int col_end  = line_len - qn;
            if (pass == 1 && row == start_row) {
                /* Only search the prefix we missed on the first pass. */
                int limit = start_col - 1;
                if (limit < col_end) col_end = limit;
            }
            for (int col = col0; col <= col_end; col++) {
                if (find_match_at(JBE_BUF(s)->lines[row], line_len, col,
                                  JBE_PANE(s)->find_query, qn)) {
                    *r = row; *c = col; return true;
                }
            }
        }
    }
    return false;
}

/* Apply a found match to the editor state: highlight + cursor. */
static void find_apply(jbe_state_t *s, int row, int col) {
    JBE_PANE(s)->find_match_row = row;
    JBE_PANE(s)->find_match_col = col;
    JBE_PANE(s)->sel_active     = true;
    JBE_PANE(s)->sel_block      = false;
    JBE_PANE(s)->sel_row        = row;
    JBE_PANE(s)->sel_col        = col;
    JBE_PANE(s)->cur_row        = row;
    JBE_PANE(s)->cur_col        = col + JBE_PANE(s)->find_query_len;
    jbe_follow_cursor(s);
}

/* Drop find mode without touching the cursor. */
static void find_close(jbe_state_t *s) {
    JBE_PANE(s)->find_active     = false;
    JBE_PANE(s)->find_query_len  = 0;
    JBE_PANE(s)->find_query[0]   = 0;
    JBE_PANE(s)->find_match_row  = -1;
}

/* Re-run the search from the anchor (last match start, or cursor if none).
   Used after each printable / Backspace edit on the query. */
static void find_redo_from_anchor(jbe_state_t *s) {
    int from_r = (JBE_PANE(s)->find_match_row >= 0) ? JBE_PANE(s)->find_match_row : JBE_PANE(s)->cur_row;
    int from_c = (JBE_PANE(s)->find_match_row >= 0) ? JBE_PANE(s)->find_match_col : JBE_PANE(s)->cur_col;
    int r, c;
    if (JBE_PANE(s)->find_query_len > 0 && find_search(s, from_r, from_c, &r, &c)) {
        find_apply(s, r, c);
    } else {
        JBE_PANE(s)->find_match_row = -1;
        JBE_PANE(s)->sel_active     = false;
    }
}

/* F3 / Ctrl+F-again: jump to the next match after the current one. */
static void find_next(jbe_state_t *s) {
    if (JBE_PANE(s)->find_query_len == 0) return;
    int from_r = (JBE_PANE(s)->find_match_row >= 0) ? JBE_PANE(s)->find_match_row : JBE_PANE(s)->cur_row;
    int from_c = (JBE_PANE(s)->find_match_row >= 0) ? JBE_PANE(s)->find_match_col + 1 : JBE_PANE(s)->cur_col;
    int r, c;
    if (find_search(s, from_r, from_c, &r, &c)) find_apply(s, r, c);
    else { JBE_PANE(s)->find_match_row = -1; JBE_PANE(s)->sel_active = false; }
}

/* Find the nearest match strictly BEFORE (anchor_row, anchor_col); if there is
   none, wrap to the very last match in the document. One forward scan keeps it
   simple and obviously correct -- documents here are small. */
static bool find_search_back(const jbe_state_t *s, int anchor_row, int anchor_col,
                             int *r, int *c) {
    int qn = JBE_PANE(s)->find_query_len;
    if (qn <= 0 || JBE_BUF(s)->n_lines == 0) return false;
    int best_r = -1, best_c = -1;   /* latest match strictly before the anchor */
    int last_r = -1, last_c = -1;   /* very last match anywhere (for the wrap) */
    for (int row = 0; row < JBE_BUF(s)->n_lines; row++) {
        int line_len = JBE_BUF(s)->len[row];
        for (int col = 0; col <= line_len - qn; col++) {
            if (!find_match_at(JBE_BUF(s)->lines[row], line_len, col,
                               JBE_PANE(s)->find_query, qn)) continue;
            last_r = row; last_c = col;
            if (row < anchor_row || (row == anchor_row && col < anchor_col)) {
                best_r = row; best_c = col;
            }
        }
    }
    if (best_r >= 0) { *r = best_r; *c = best_c; return true; }
    if (last_r >= 0) { *r = last_r; *c = last_c; return true; }
    return false;
}

/* Ctrl+P: jump to the previous match (searches upward, wraps to the bottom). */
static void find_prev(jbe_state_t *s) {
    if (JBE_PANE(s)->find_query_len == 0) return;
    int ar = (JBE_PANE(s)->find_match_row >= 0) ? JBE_PANE(s)->find_match_row : JBE_PANE(s)->cur_row;
    int ac = (JBE_PANE(s)->find_match_row >= 0) ? JBE_PANE(s)->find_match_col : JBE_PANE(s)->cur_col;
    int r, c;
    if (find_search_back(s, ar, ac, &r, &c)) find_apply(s, r, c);
    else { JBE_PANE(s)->find_match_row = -1; JBE_PANE(s)->sel_active = false; }
}

/* Open Find mode; called from Ctrl+F and Search->Find menu. */
static void find_open(jbe_state_t *s) {
    JBE_PANE(s)->find_active    = true;
    JBE_PANE(s)->find_query_len = 0;
    JBE_PANE(s)->find_query[0]  = 0;
    JBE_PANE(s)->find_match_row = -1;
    JBE_PANE(s)->sel_active     = false;
}

/* --- Replace (step 8b) ---------------------------------------------- */

static void replace_close(jbe_state_t *s) {
    JBE_PANE(s)->replace_active    = false;
    JBE_PANE(s)->replace_phase     = 0;
    JBE_PANE(s)->find_active       = false;
    JBE_PANE(s)->find_query_len    = 0;
    JBE_PANE(s)->replace_with_len  = 0;
    JBE_PANE(s)->find_match_row    = -1;
}

static void replace_open(jbe_state_t *s) {
    find_open(s);
    JBE_PANE(s)->replace_active   = true;
    JBE_PANE(s)->replace_phase    = 0;
    JBE_PANE(s)->replace_with_len = 0;
    JBE_PANE(s)->replace_with[0]  = 0;
}

/* --- Go to Line (Ctrl+L) -------------------------------------------- */

static void goto_open(jbe_state_t *s) {
    s->goto_active = true;
    s->goto_len    = 0;
    s->goto_buf[0] = 0;
}

/* Move the active pane's cursor to a 1-based line (clamped to the document) and
   scroll it into view. Exported (see jbe.h): the BASIC interpreter calls this to
   jump to the line of a parse / run-time error after the program returns. */
void jbe_goto_line(jbe_state_t *s, int line_1based) {
    int n = line_1based;
    if (n < 1) n = 1;
    if (n > JBE_BUF(s)->n_lines) n = JBE_BUF(s)->n_lines;
    JBE_PANE(s)->cur_row    = n - 1;
    JBE_PANE(s)->cur_col    = 0;
    JBE_PANE(s)->sel_active = false;
    jbe_follow_cursor(s);
}

/* Parse the typed line number (1-based) and jump there. Empty input just closes
   the prompt. */
static void goto_commit(jbe_state_t *s) {
    s->goto_active = false;
    if (s->goto_len == 0) return;
    jbe_goto_line(s, (int)strtol(s->goto_buf, NULL, 10));
}

/* Routes a keystroke while the Go to Line prompt is open: digits only. */
static void goto_route_key(jbe_state_t *s, uint16_t k) {
    switch (k) {
        case JAPI_KEY_ESCAPE: s->goto_active = false; return;
        case JAPI_KEY_ENTER:  goto_commit(s);         return;
        case JAPI_KEY_BACKSPACE:
            if (s->goto_len > 0) s->goto_buf[--s->goto_len] = 0;
            return;
        default:
            if (k >= '0' && k <= '9' && s->goto_len < (int)sizeof s->goto_buf - 1) {
                s->goto_buf[s->goto_len++] = (char)k;
                s->goto_buf[s->goto_len]   = 0;
            }
            return;
    }
}

/* Replace the current match in place, pushing two undo records (a DELETE of the
 * old text + an INSERT of the replacement, so two Ctrl+Z restore the line). Does
 * NOT advance -- find_match_* still point at the edited spot. Returns the column
 * just past the inserted replacement, or -1 if there is no current match. */
static int replace_edit_only(jbe_state_t *s) {
    if (JBE_PANE(s)->find_match_row < 0) return -1;
    int row = JBE_PANE(s)->find_match_row, col = JBE_PANE(s)->find_match_col;
    int qn  = JBE_PANE(s)->find_query_len, wn = JBE_PANE(s)->replace_with_len;
    int line_len = JBE_BUF(s)->len[row];
    int new_len  = line_len - qn + wn;
    /* Snapshot the old match text before mutation. */
    char *old_match = qn > 0 ? malloc(qn) : NULL;
    if (qn > 0 && old_match) memcpy(old_match, JBE_BUF(s)->lines[row] + col, qn);
    char *new_match = wn > 0 ? malloc(wn) : NULL;
    if (wn > 0 && new_match) memcpy(new_match, JBE_PANE(s)->replace_with, wn);
    if (wn > qn) { if (!line_set_len(s, row, new_len)) { free(old_match); free(new_match); return -1; } }
    char *L = JBE_BUF(s)->lines[row];
    memmove(&L[col + wn], &L[col + qn], line_len - col - qn);
    memcpy (&L[col],      JBE_PANE(s)->replace_with, wn);
    if (wn < qn) (void)line_set_len(s, row, new_len);
    else         JBE_BUF(s)->len[row] = new_len;
    if (old_match) undo_push(s, JBE_UNDO_DELETE, row, col,
                             old_match, qn, row, col, false);
    if (new_match) undo_push(s, JBE_UNDO_INSERT, row, col,
                             new_match, wn, row, col, false);
    JBE_BUF(s)->dirty = true;
    return col + wn;
}

/* Forward search to the end of the document WITHOUT the wrap-around. Replace All
 * uses this so a replacement that itself contains the search text (e.g. "a" ->
 * "aa") can never loop forever. */
static bool find_search_fwd_nowrap(const jbe_state_t *s, int start_row, int start_col,
                                   int *r, int *c) {
    int qn = JBE_PANE(s)->find_query_len;
    if (qn <= 0) return false;
    for (int row = start_row; row < JBE_BUF(s)->n_lines; row++) {
        int line_len = JBE_BUF(s)->len[row];
        int col0 = (row == start_row) ? start_col : 0;
        for (int col = col0; col <= line_len - qn; col++)
            if (find_match_at(JBE_BUF(s)->lines[row], line_len, col,
                              JBE_PANE(s)->find_query, qn)) { *r = row; *c = col; return true; }
    }
    return false;
}

/* Enter in Replace: replace the current match, then highlight the next one
 * (wrapping, like find-next). */
static void replace_apply_current(jbe_state_t *s) {
    int row   = JBE_PANE(s)->find_match_row;
    int after = replace_edit_only(s);
    if (after < 0) return;
    int r, c;
    if (find_search(s, row, after, &r, &c)) find_apply(s, r, c);
    else { JBE_PANE(s)->find_match_row = -1; JBE_PANE(s)->sel_active = false; }
}

/* Ctrl+A in Replace: replace from the current match forward to the end of the
 * document (no wrap). */
static void replace_all(jbe_state_t *s) {
    while (JBE_PANE(s)->find_match_row >= 0) {
        int row   = JBE_PANE(s)->find_match_row;
        int after = replace_edit_only(s);
        if (after < 0) break;
        int r, c;
        if (find_search_fwd_nowrap(s, row, after, &r, &c)) find_apply(s, r, c);
        else { JBE_PANE(s)->find_match_row = -1; JBE_PANE(s)->sel_active = false; break; }
    }
}

/* --- Undo / redo apply --------------------------------------------------
 *
 * `undo_apply` executes one record in either direction:
 *   reverse=true  → do the inverse of the record (Ctrl+Z).
 *   reverse=false → do the action recorded         (Ctrl+Y).
 *
 * The four record kinds form two inverse pairs (INSERT↔DELETE, SPLIT↔JOIN),
 * so the reverse direction maps the kind to its partner before dispatching.
 *
 * Cursor placement:
 *   reverse=true  → cursor restored to (cur_row_before, cur_col_before).
 *   reverse=false → cursor lands at the natural end of the action.
 *
 * Caller is responsible for moving the record between undo and redo stacks.
 */

/* Count newlines and length of the last segment in text[0..n). */
static void undo_scan(const char *text, int n, int *nl_out, int *last_seg_out) {
    int nl = 0, last_seg = 0;
    for (int i = 0; i < n; i++) {
        if (text[i] == '\n') { nl++; last_seg = 0; }
        else last_seg++;
    }
    *nl_out = nl; *last_seg_out = last_seg;
}

/* Plain in-line byte insert at (row, col). */
static void apply_inline_insert(jbe_state_t *s, int row, int col,
                                const char *src, int n) {
    if (n <= 0) return;
    int old = JBE_BUF(s)->len[row];
    if (!line_set_len(s, row, old + n)) return;
    char *L = JBE_BUF(s)->lines[row];
    memmove(&L[col + n], &L[col], old - col);
    memcpy (&L[col],     src,    n);
}

/* Plain in-line byte delete at (row, col). */
static void apply_inline_delete(jbe_state_t *s, int row, int col, int n) {
    if (n <= 0) return;
    int oldn = JBE_BUF(s)->len[row];
    char *L = JBE_BUF(s)->lines[row];
    memmove(&L[col], &L[col + n], oldn - col - n);
    (void)line_set_len(s, row, oldn - n);
}

/* Split row at col into row, row+1. */
static void apply_split(jbe_state_t *s, int row, int col) {
    int n = JBE_BUF(s)->len[row];
    if (!lines_insert_at(s, row + 1)) return;
    int tail = n - col;
    if (!line_set_len(s, row + 1, tail)) { lines_remove_at(s, row + 1); return; }
    memcpy(JBE_BUF(s)->lines[row + 1], &JBE_BUF(s)->lines[row][col], tail);
    JBE_BUF(s)->lines[row][col] = 0;
    JBE_BUF(s)->len[row] = col;
    (void)line_set_len(s, row, col);
}

/* Join row+1 onto row, expecting col == len[row] at the join point. */
static void apply_join(jbe_state_t *s, int row, int col) {
    if (row + 1 >= JBE_BUF(s)->n_lines) return;
    int m = JBE_BUF(s)->len[row + 1];
    if (!line_set_len(s, row, col + m)) return;
    memcpy(&JBE_BUF(s)->lines[row][col], JBE_BUF(s)->lines[row + 1], m);
    lines_remove_at(s, row + 1);
}

/* Stream insert (text may contain '\n'). Reports end position. */
static void apply_stream_insert(jbe_state_t *s, int row, int col,
                                const char *text, int n,
                                int *end_row, int *end_col) {
    int orig_tail_len = JBE_BUF(s)->len[row] - col;
    char *tail = NULL;
    if (orig_tail_len > 0) {
        tail = malloc(orig_tail_len);
        if (!tail) { *end_row = row; *end_col = col; return; }
        memcpy(tail, JBE_BUF(s)->lines[row] + col, orig_tail_len);
        (void)line_set_len(s, row, col);
    }
    int seg_start = 0;
    bool first = true;
    int cr = row, cc = col;
    for (int i = 0; i <= n; i++) {
        if (i == n || text[i] == '\n') {
            int seg_len = i - seg_start;
            if (first) {
                apply_inline_insert(s, cr, cc, text + seg_start, seg_len);
                cc += seg_len;
                first = false;
            } else {
                lines_insert_at(s, cr + 1);
                if (line_set_len(s, cr + 1, seg_len))
                    memcpy(JBE_BUF(s)->lines[cr + 1], text + seg_start, seg_len);
                cr++;
                cc = seg_len;
            }
            seg_start = i + 1;
        }
    }
    if (orig_tail_len > 0) {
        apply_inline_insert(s, cr, cc, tail, orig_tail_len);
        free(tail);
    }
    *end_row = cr; *end_col = cc;
}

/* Stream delete: remove `n` bytes (incl. '\n's) starting at (row, col). */
static void apply_stream_delete(jbe_state_t *s, int row, int col,
                                const char *text, int n) {
    int nl, last_seg;
    undo_scan(text, n, &nl, &last_seg);
    int er = row + nl;
    int ec = (nl == 0) ? (col + n) : last_seg;
    int tail_len = JBE_BUF(s)->len[er] - ec;
    char *new_first = malloc(col + tail_len + 1);
    if (!new_first) return;
    memcpy(new_first,        JBE_BUF(s)->lines[row],         col);
    memcpy(new_first + col,  JBE_BUF(s)->lines[er] + ec,     tail_len);
    new_first[col + tail_len] = 0;
    free(JBE_BUF(s)->lines[row]);
    JBE_BUF(s)->lines[row] = new_first;
    JBE_BUF(s)->len[row]   = col + tail_len;
    for (int i = row + 1; i <= er; i++) free(JBE_BUF(s)->lines[i]);
    int removed = er - row;
    if (removed > 0) {
        memmove(&JBE_BUF(s)->lines[row + 1], &JBE_BUF(s)->lines[er + 1],
                (JBE_BUF(s)->n_lines - er - 1) * sizeof *JBE_BUF(s)->lines);
        memmove(&JBE_BUF(s)->len[row + 1],   &JBE_BUF(s)->len[er + 1],
                (JBE_BUF(s)->n_lines - er - 1) * sizeof *JBE_BUF(s)->len);
        JBE_BUF(s)->n_lines -= removed;
    }
}

/* Block insert: each '\n'-segment placed on row+i at col, padding short rows
   with spaces and appending fresh rows when running past EOF (mirrors the
   block branch of clip_paste). The accompanying undo record stores
   pad_orig_len[] + grew_lines so undo can strip the padding and drop the
   appended rows — see the JBE_UNDO_DELETE branch of undo_apply. */
static void apply_block_insert(jbe_state_t *s, int row, int col,
                               const char *text, int n) {
    int seg_start = 0, seg_idx = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || text[i] == '\n') {
            int seg_len = i - seg_start;
            int target  = row + seg_idx;
            while (target >= JBE_BUF(s)->n_lines) lines_insert_at(s, JBE_BUF(s)->n_lines);
            if (JBE_BUF(s)->len[target] < col) {
                int old = JBE_BUF(s)->len[target];
                if (line_set_len(s, target, col))
                    memset(JBE_BUF(s)->lines[target] + old, ' ', col - old);
            }
            apply_inline_insert(s, target, col, text + seg_start, seg_len);
            seg_idx++;
            seg_start = i + 1;
        }
    }
}

/* Block delete: for each row+i, remove chunk_i.len bytes at col. */
static void apply_block_delete(jbe_state_t *s, int row, int col,
                               const char *text, int n) {
    int seg_start = 0, seg_idx = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || text[i] == '\n') {
            int seg_len = i - seg_start;
            int target  = row + seg_idx;
            if (target < JBE_BUF(s)->n_lines && col + seg_len <= JBE_BUF(s)->len[target])
                apply_inline_delete(s, target, col, seg_len);
            seg_idx++;
            seg_start = i + 1;
        }
    }
}

static void undo_apply(jbe_state_t *s, const jbe_undo_rec_t *rec, bool reverse) {
    jbe_undo_kind_t k = rec->kind;
    if (reverse) {
        switch (k) {
            case JBE_UNDO_INSERT: k = JBE_UNDO_DELETE; break;
            case JBE_UNDO_DELETE: k = JBE_UNDO_INSERT; break;
            case JBE_UNDO_SPLIT:  k = JBE_UNDO_JOIN;   break;
            case JBE_UNDO_JOIN:   k = JBE_UNDO_SPLIT;  break;
        }
    }
    int end_row = rec->row, end_col = rec->col;
    switch (k) {
        case JBE_UNDO_INSERT:
            if (rec->is_block) {
                apply_block_insert(s, rec->row, rec->col, rec->text, rec->text_len);
                int nl, last_seg; undo_scan(rec->text, rec->text_len, &nl, &last_seg);
                end_row = rec->row + nl;
                end_col = rec->col + last_seg;
            } else {
                apply_stream_insert(s, rec->row, rec->col, rec->text, rec->text_len,
                                    &end_row, &end_col);
            }
            break;
        case JBE_UNDO_DELETE:
            if (rec->is_block) {
                apply_block_delete(s, rec->row, rec->col, rec->text, rec->text_len);
                /* Reversing a block INSERT that originally grew the buffer
                   past EOF or padded short rows: strip padding spaces and
                   drop the grown rows so the buffer is byte-for-byte equal
                   to its pre-paste state. pad_count == 0 on plain DELETEs
                   (and on inserts that needed no growth/padding), so this
                   branch is a no-op there. */
                if (rec->pad_count > 0 && rec->pad_orig_len) {
                    /* 1. Restore original lengths of rows we padded. */
                    for (int i = 0; i < rec->pad_count; i++) {
                        int16_t orig = rec->pad_orig_len[i];
                        if (orig < 0) continue;             /* sentinel: nothing to restore */
                        int target = rec->row + i;
                        if (target < JBE_BUF(s)->n_lines
                            && JBE_BUF(s)->len[target] > orig)
                            (void)line_set_len(s, target, orig);
                    }
                    /* 2. Drop the trailing rows that were appended past EOF. */
                    for (int i = 0; i < rec->grew_lines; i++) {
                        int target = JBE_BUF(s)->n_lines - 1;
                        if (target <= 0) break;             /* never drop the last row */
                        lines_remove_at(s, target);
                    }
                }
            } else {
                apply_stream_delete(s, rec->row, rec->col, rec->text, rec->text_len);
            }
            end_row = rec->row; end_col = rec->col;
            break;
        case JBE_UNDO_SPLIT:
            apply_split(s, rec->row, rec->col);
            end_row = rec->row + 1; end_col = 0;
            break;
        case JBE_UNDO_JOIN:
            apply_join(s, rec->row, rec->col);
            end_row = rec->row; end_col = rec->col;
            break;
    }
    if (reverse) { JBE_PANE(s)->cur_row = rec->cur_row_before; JBE_PANE(s)->cur_col = rec->cur_col_before; }
    else         { JBE_PANE(s)->cur_row = end_row;             JBE_PANE(s)->cur_col = end_col;             }
    JBE_PANE(s)->sel_active = false;
    JBE_BUF(s)->dirty      = true;
}

/* Public: Ctrl+Z. Pop the newest undo record, apply its inverse, push it
   onto the redo stack. */
void jbe_undo(jbe_state_t *s) {
    jbe_undo_rec_t rec;
    if (!undo_stack_pop(&JBE_BUF(s)->undo, &rec)) return;
    undo_apply(s, &rec, true);
    rec.coalesce_typing = false;             /* never coalesce across an undo */
    undo_stack_push_rec(s, &JBE_BUF(s)->redo, &rec);  /* steals text ownership */
    undo_enforce_budget(s);
    jbe_recompute_dirty(s);
    jbe_follow_cursor(s);
}

/* Public: Ctrl+Y. Pop the newest redo record, re-apply the action, push it
   back onto the undo stack. */
void jbe_redo(jbe_state_t *s) {
    jbe_undo_rec_t rec;
    if (!undo_stack_pop(&JBE_BUF(s)->redo, &rec)) return;
    undo_apply(s, &rec, false);
    rec.coalesce_typing = false;
    undo_stack_push_rec(s, &JBE_BUF(s)->undo, &rec);
    undo_enforce_budget(s);
    jbe_recompute_dirty(s);
    jbe_follow_cursor(s);
}

/* Execute the currently-highlighted item, then close the menu. Unknown /
   not-yet-implemented items fall through silently. */
/* Split-screen helpers. View->Split starts the two panes on the SAME file
   (two views of one buffer), which is handy for copying between distant parts;
   a File->New / File->Open in either pane then gives that pane its own file.
   So Open seeds pane 1 from pane 0 (same buffer, same scroll). View->Split
   again un-splits: it drops the OTHER pane and keeps the one you're in (see
   split_toggle). Swap moves the focus between the two panes; it's a no-op when
   the split isn't open. */
static void split_open(jbe_state_t *s) {
    if (s->split_active) return;
    s->panes[1] = s->panes[0];        /* same buffer, same viewport to begin */
    s->split_active = true;
    s->active_pane  = 0;
}

static void split_swap_focus(jbe_state_t *s) {
    if (!s->split_active) return;
    s->active_pane = (s->active_pane == 0) ? 1 : 0;
}

/* True if dropping pane `p` (on un-split) would discard unsaved edits: it owns
   a dirty buffer that isn't also shown in the other pane. */
static bool pane_drop_would_lose(const jbe_state_t *s, int p) {
    int other = p ^ 1;
    if (s->panes[other].buf_idx == s->panes[p].buf_idx) return false;  /* shared */
    return s->buffers[s->panes[p].buf_idx].dirty;
}

/* Un-split: return to a single full-screen pane showing the ACTIVE pane's
   document, dropping the other pane. The dropped pane's buffer is freed unless
   it is shared with the kept pane. Single-pane mode always lives in pane 0 /
   buffer 0, so the survivor is moved down into slot 0 if needed. */
static void split_collapse_keep_active(jbe_state_t *s) {
    int keep     = s->active_pane;
    int drop     = keep ^ 1;
    int keep_idx = s->panes[keep].buf_idx;
    int drop_idx = s->panes[drop].buf_idx;
    if (drop_idx != keep_idx)
        buffer_free(&s->buffers[drop_idx]);
    jbe_pane_t kept = s->panes[keep];
    if (keep_idx != 0) {
        s->buffers[0] = s->buffers[keep_idx];
        memset(&s->buffers[keep_idx], 0, sizeof s->buffers[keep_idx]);
    }
    memset(&s->buffers[1], 0, sizeof s->buffers[1]);   /* slot 1 is free now */
    kept.buf_idx = 0;
    s->panes[0] = kept;
    memset(&s->panes[1], 0, sizeof s->panes[1]);
    s->split_active = false;
    s->active_pane  = 0;
}

/* Move the divider one screen row up (delta=-1) or down (delta=+1).
   No-op when there is no split. The clamp keeps at least one text row
   on each side of the divider so both panes always have something to
   render. */
static void split_resize(jbe_state_t *s, int delta) {
    if (!s->split_active) return;
    int r = s->split_row + delta;
    if (r < JBE_VIEW_TOP + 1)    r = JBE_VIEW_TOP + 1;
    if (r > JBE_VIEW_BOTTOM - 1) r = JBE_VIEW_BOTTOM - 1;
    s->split_row = r;
}

static void open_dialog_show(jbe_state_t *s);     /* defined below */
static void macro_toggle_record(jbe_state_t *s);  /* defined below */
static void macro_replay(jbe_state_t *s);         /* defined below */
static void commander_open(jbe_state_t *s);              /* defined below */
static void commander_handle_key(jbe_state_t *s, uint16_t k);  /* defined below */

/* Toggle the split (View->Split). Off->on starts two views of the current
   file; on->off un-splits, keeping the pane you're in and dropping the other.
   If dropping it would lose unsaved edits, ask first (the shared confirm box,
   targeted at this un-split via confirm_action). */
static void split_toggle(jbe_state_t *s) {
    if (!s->split_active) { split_open(s); return; }
    if (pane_drop_would_lose(s, s->active_pane ^ 1) && !s->close_confirm) {
        s->close_confirm  = true;
        s->confirm_action = JBE_CONFIRM_UNSPLIT;
        return;
    }
    s->close_confirm = false;
    split_collapse_keep_active(s);
}

/* Open the split and load `path` into the second pane (see jbe.h). */
bool jbe_open_in_split(jbe_state_t *s, const char *path) {
    split_open(s);                /* both panes start on pane 0's buffer */
    s->active_pane = 1;
    return jbe_load(s, path);     /* detaches pane 1 to its own slot, then loads */
}

static void menu_activate(jbe_state_t *s) {
    int m = s->menu_idx, i = s->item_idx;
    menu_close(s);   /* close first so the dropdown is gone before we render */
    switch (m) {
        case 0: /* File */
            if      (i == 0) request_new(s);
            else if (i == 1) open_dialog_show(s);
            else if (i == 2) save_or_save_as(s);   /* Save: untitled -> Save As */
            else if (i == 3) save_as_open(s);       /* Save As */
            else if (i == 4) close_current(s);      /* Close */
            break;
        case 1: /* Edit (indexes 4 and 7 are separator lines) */
            if (i == 0) {                          /* Cut */
                if (JBE_PANE(s)->sel_active) { clip_copy_selection(s); sel_delete(s); }
            } else if (i == 1) {                   /* Copy */
                clip_copy_selection(s);
            } else if (i == 2) {                   /* Paste */
                clip_paste(s);
                if (JBE_PANE(s)->cur_col > JBE_BUF(s)->len[JBE_PANE(s)->cur_row])
                    JBE_PANE(s)->cur_col = JBE_BUF(s)->len[JBE_PANE(s)->cur_row];
            } else if (i == 3) {                   /* Select All */
                select_all(s);
            } else if (i == 5) {                   /* Toggle Comment */
                toggle_comment(s);
            } else if (i == 6) {                   /* Format (Embellish) */
                jbe_format(s);
            } else if (i == 8) {                   /* Undo */
                jbe_undo(s);
            } else if (i == 9) {                   /* Redo */
                jbe_redo(s);
            }
            jbe_follow_cursor(s);
            break;
        case 2: /* View */
            if      (i == 0) split_toggle(s);
            else if (i == 1) split_swap_focus(s);
            else if (i == 2) split_resize(s, -1);
            else if (i == 3) split_resize(s, +1);
            break;
        case 3: /* Search */
            if      (i == 0) find_open(s);
            else if (i == 1) replace_open(s);
            else if (i == 2) goto_open(s);
            break;
        case 4: /* Macro */
            if      (i == 0) macro_toggle_record(s);
            else if (i == 1) macro_replay(s);
            break;
        case 5: /* Options — CPU speed, or a dynamic syntax-scheme item */
            if (i == s->cpu_item_index) { cpu_open(s); break; }
            if (i >= 0 && i < s->options_n) {
                strncpy(JBE_BUF(s)->syntax_name, s->options_names[i],
                        sizeof JBE_BUF(s)->syntax_name - 1);
                JBE_BUF(s)->syntax_name[sizeof JBE_BUF(s)->syntax_name - 1] = 0;
                install_scheme(JBE_BUF(s), scheme_resolve(JBE_BUF(s)->syntax_name));
            }
            break;
        case 6: { /* Run -- dispatch by accelerator so the index stays correct
                     whether or not the Basic Interpreter entry is present. */
            const menu_item_t *items = menu_items_for(s, 6);
            char acc = items[i].accel;
#ifdef JBB_RUN
            if (acc == 'B') { s->run_request = true; break; }
#endif
            if (acc == 'J') commander_open(s);
            break;
        }
        case 7: /* Help -> Editor Help: open the manual at the top. */
            help_open(s, NULL);
            break;
    }
}

/* Keyboard macro helpers. Recording captures the post-japi_base.h JAPI_KEY
   stream (already debounced, modifier-resolved), so a recorded macro is
   portable between sim and Pico. Ctrl+T (Record) and Ctrl+P (Play) are
   filtered from the recorded stream so they don't accidentally re-trigger
   record/play during replay. Recording stops silently on buffer overflow
   — partial macros are better than silently-overwritten ones. */
static void macro_toggle_record(jbe_state_t *s) {
    if (s->macro_playing) return;                 /* should never happen */
    if (s->macro_recording) {
        s->macro_recording = false;
    } else {
        s->macro_len = 0;
        s->macro_recording = true;
    }
}

static void macro_replay(jbe_state_t *s) {
    if (s->macro_playing) return;                 /* nested replay forbidden */
    if (s->macro_len == 0) return;                /* nothing to play */
    bool was_recording = s->macro_recording;
    s->macro_recording = false;                   /* don't capture replay */
    s->macro_playing   = true;
    uint16_t n = s->macro_len;
    for (uint16_t i = 0; i < n; i++) {
        jbe_handle_key(s, s->macro_buf[i]);
    }
    s->macro_playing   = false;
    s->macro_recording = was_recording;
}

/* File→Open dialog. Centered panel; ui_filelist owns the list area. The
   dialog is purely interactive — the result (picked path) is handed off via
   s->open_request so jbe_main can decide which buffer slot receives it. */
#define JBE_OPEN_DLG_ROW   19
#define JBE_OPEN_DLG_COL   38
#define JBE_OPEN_DLG_W     50
#define JBE_OPEN_DLG_H     26
#define JBE_OPEN_LIST_ROW  (JBE_OPEN_DLG_ROW + 2)
#define JBE_OPEN_LIST_COL  (JBE_OPEN_DLG_COL + 1)
#define JBE_OPEN_LIST_H    (JBE_OPEN_DLG_H - 4)        /* title + sep + list + help */
#define JBE_OPEN_LIST_W    (JBE_OPEN_DLG_W - 2)

static void open_dialog_show(jbe_state_t *s) {
    s->open_active = true;
    ui_filelist_default_colors(&s->open_dlg);
    /* Match JBE's cyan dialog palette (same as the menu dropdowns). */
    s->open_dlg.fg     = JBE_DROP_FG;
    s->open_dlg.bg     = JBE_DROP_BG;
    s->open_dlg.sel_fg = JBE_DROP_HI_FG;
    s->open_dlg.sel_bg = JBE_DROP_HI_BG;
    /* Same fg as files: low contrast on cyan otherwise. Directories are
       still visually distinct thanks to the "/" suffix the widget appends. */
    s->open_dlg.dir_fg = JBE_DROP_FG;
    /* Start on A: (the SD card) when one is present; if there is no SD,
       ui_filelist_open returns false, so fall back to C: (built-in flash) --
       no point opening to an empty A: when the files are on C:. The user can
       still Tab between the two drives either way. */
    if (!ui_filelist_open(&s->open_dlg, "A:",
                          JBE_OPEN_LIST_ROW, JBE_OPEN_LIST_COL,
                          JBE_OPEN_LIST_H,   JBE_OPEN_LIST_W)) {
        ui_filelist_open(&s->open_dlg, "C:",
                         JBE_OPEN_LIST_ROW, JBE_OPEN_LIST_COL,
                         JBE_OPEN_LIST_H,   JBE_OPEN_LIST_W);
    }
}

static void open_dialog_route_key(jbe_state_t *s, uint16_t k) {
    ui_filelist_event_t ev = ui_filelist_key(&s->open_dlg, k);
    if (ev == UI_FILELIST_PICK) {
        ui_filelist_picked_path(&s->open_dlg, s->open_path, sizeof s->open_path);
        s->open_active  = false;
        /* Opening over a dirty document would lose it -- guard it with the same
           confirm box (open_path is already captured; Open runs on yes). */
        if (active_doc_would_be_lost(s) && JBE_BUF(s)->dirty && !s->close_confirm) {
            s->close_confirm  = true;
            s->confirm_action = JBE_CONFIRM_OPEN;
        } else {
            s->open_request = true;
        }
    } else if (ev == UI_FILELIST_CANCEL) {
        s->open_active = false;
    }
    /* CD: widget already reloaded, dialog stays open. NONE: nothing to do. */
}

void jbe_handle_key(jbe_state_t *s, uint16_t k) {
    /* The Japi Commander is fully modal and independent of the text buffer. */
    if (s->commander_active) { commander_handle_key(s, k); return; }
    /* The F1 help window is modal too, and works with an empty buffer. */
    if (s->help_active) { help_handle_key(s, k); return; }
    /* The CPU-speed chooser is modal. */
    if (s->cpu_dialog_active) { cpu_handle_key(s, k); return; }
    if (JBE_BUF(s)->n_lines == 0) return;
    s->open_msg[0] = 0;   /* any key dismisses the transient "could not open" notice */

    /* File→Open dialog swallows every key until the user picks or cancels.
       Sits at the top so it isn't bypassed by find/menu/etc. */
    if (s->open_active) {
        open_dialog_route_key(s, k);
        return;
    }

    /* Save As filename prompt: swallows keys until Enter saves or Esc cancels. */
    if (s->save_as_active) {
        save_as_route_key(s, k);
        return;
    }

    /* Go to Line prompt: swallows keys until Enter jumps or Esc cancels. */
    if (s->goto_active) {
        goto_route_key(s, k);
        return;
    }

    /* Discard-confirmation (dirty buffer): Y/Enter goes ahead, anything else
       cancels. The same prompt serves two actions: File->Close (empty the
       active pane) and View->Split's un-split (drop the other pane). */
    if (s->close_confirm) {
        bool go     = (k == 'y' || k == 'Y' || k == JAPI_KEY_ENTER);
        int  action = s->confirm_action;
        s->close_confirm  = false;
        s->confirm_action = JBE_CONFIRM_NONE;
        if (go) {
            switch (action) {
                case JBE_CONFIRM_UNSPLIT: split_collapse_keep_active(s); break;
                case JBE_CONFIRM_NEW:     s->new_request   = true;       break;
                case JBE_CONFIRM_OPEN:    s->open_request  = true;       break;
                case JBE_CONFIRM_CLOSE:
                default:                  s->close_request = true;       break;
            }
        }
        return;
    }

    /* Record this key BEFORE dispatch, unless it's the Record/Play shortcut
       itself, and unless we're replaying (otherwise replay would re-record
       its own output and grow the macro on each play). */
    if (s->macro_recording && !s->macro_playing
        && k != JAPI_KEY_CTRL('T') && k != JAPI_KEY_CTRL('P')) {
        if (s->macro_len < JBE_MACRO_MAX) {
            s->macro_buf[s->macro_len++] = k;
        }
        /* overflow: silently stop appending; existing prefix stays valid */
    }

    /* Bump the keypress counter so coalescing can require the next typed
       char to be the *immediately* next keypress. Otherwise a Left/Right
       wiggle that returns the cursor to the same column would still pass
       the geometric coalesce check. */
    JBE_BUF(s)->kpress_seq++;

    /* Replace mode runs in three phases: typing the find string (phase 0),
       typing the replacement (phase 1), then per-match confirm Y/N/A/Esc
       (phase 2). Phase 0 shares the find_query buffer + match tracking so
       the current match is already highlighted while you type. */
    if (JBE_PANE(s)->replace_active) {
        /* Fluid model, analogous to Find: the box stays open, both fields stay
           editable, and you navigate + replace with non-letter keys (so the
           letters remain free for typing). replace_phase is the focused field:
           0 = the search field, 1 = the "with" field. */
        switch (k) {
            case JAPI_KEY_ESCAPE:    replace_close(s);                return;
            case JAPI_KEY_TAB:       JBE_PANE(s)->replace_phase ^= 1; return;
            case JAPI_KEY_CTRL('N'): find_next(s);                    return;
            case JAPI_KEY_CTRL('P'): find_prev(s);                    return;
            case JAPI_KEY_CTRL('A'): replace_all(s);                  return;
            case JAPI_KEY_ENTER:     replace_apply_current(s);        return;
            case JAPI_KEY_BACKSPACE:
                if (JBE_PANE(s)->replace_phase == 0) {            /* search field */
                    if (JBE_PANE(s)->find_query_len > 0) {
                        JBE_PANE(s)->find_query[--JBE_PANE(s)->find_query_len] = 0;
                        if (JBE_PANE(s)->find_query_len == 0) {
                            JBE_PANE(s)->find_match_row = -1; JBE_PANE(s)->sel_active = false;
                        } else find_redo_from_anchor(s);
                    }
                } else {                                          /* "with" field */
                    if (JBE_PANE(s)->replace_with_len > 0)
                        JBE_PANE(s)->replace_with[--JBE_PANE(s)->replace_with_len] = 0;
                }
                return;
            default:
                if (k >= 32 && k < 127) {
                    if (JBE_PANE(s)->replace_phase == 0) {        /* type into search */
                        if (JBE_PANE(s)->find_query_len < (int)sizeof JBE_PANE(s)->find_query - 1) {
                            JBE_PANE(s)->find_query[JBE_PANE(s)->find_query_len++] = (char)k;
                            JBE_PANE(s)->find_query[JBE_PANE(s)->find_query_len]   = 0;
                            find_redo_from_anchor(s);
                        }
                    } else {                                      /* type into "with" */
                        if (JBE_PANE(s)->replace_with_len < (int)sizeof JBE_PANE(s)->replace_with - 1) {
                            JBE_PANE(s)->replace_with[JBE_PANE(s)->replace_with_len++] = (char)k;
                            JBE_PANE(s)->replace_with[JBE_PANE(s)->replace_with_len]   = 0;
                        }
                    }
                }
                return;
        }
    }

    /* Find mode swallows almost every key — typing builds the query,
       Backspace shrinks it. Enter / Ctrl+N / Ctrl+F jump to the next match,
       Ctrl+P to the previous one (both wrap); the box stays open until Esc. */
    if (JBE_PANE(s)->find_active) {
        switch (k) {
            case JAPI_KEY_ESCAPE:
                find_close(s);
                return;
            case JAPI_KEY_ENTER:
            case JAPI_KEY_CTRL('F'):
            case JAPI_KEY_CTRL('N'):
                find_next(s);
                return;
            case JAPI_KEY_CTRL('P'):
                find_prev(s);
                return;
            case JAPI_KEY_BACKSPACE:
                if (JBE_PANE(s)->find_query_len > 0) {
                    JBE_PANE(s)->find_query[--JBE_PANE(s)->find_query_len] = 0;
                    if (JBE_PANE(s)->find_query_len == 0) {
                        JBE_PANE(s)->find_match_row = -1;
                        JBE_PANE(s)->sel_active = false;
                    } else {
                        find_redo_from_anchor(s);
                    }
                }
                return;
            default:
                if (k >= 32 && k < 127 &&
                    JBE_PANE(s)->find_query_len < (int)sizeof JBE_PANE(s)->find_query - 1) {
                    JBE_PANE(s)->find_query[JBE_PANE(s)->find_query_len++] = (char)k;
                    JBE_PANE(s)->find_query[JBE_PANE(s)->find_query_len]   = 0;
                    find_redo_from_anchor(s);
                }
                return;
        }
    }

    /* File shortcuts. */
    if (k == JAPI_KEY_CTRL('N')) { request_new(s); return; }
    if (k == JAPI_KEY_CTRL('O')) { open_dialog_show(s);   return; }
#ifndef JAPI_PICO
    /* Ctrl+Q -- SIMULATOR ONLY, and on purpose NOT documented in the manual
       (it is not a feature of the machine). The real Japi Base has no Exit:
       the editor is the whole environment, there is nowhere to quit to. But
       the Linux simulator is an ordinary program that needs a clean way out,
       and Ctrl+Q is exactly that: it sets `quit`, the host main() returns, and
       the atexit handlers restore the terminal (raw mode + alt-screen) so you
       land back at a sane shell prompt. Compiled out on the Pico via
       JAPI_PICO, so on hardware it costs nothing and can never reset the
       machine. */
    if (k == JAPI_KEY_CTRL('Q')) { s->quit = true;        return; }
#endif

    /* Ctrl+F / Ctrl+R outside any search mode open them. */
    if (k == JAPI_KEY_CTRL('F')) { find_open(s);    return; }
    if (k == JAPI_KEY_CTRL('R')) { replace_open(s); return; }
    if (k == JAPI_KEY_CTRL('L')) { goto_open(s);    return; }

    /* Macro shortcuts. Ctrl+T = Record start/stop; Ctrl+P = Play. (Ctrl+M cannot
       be used: the keyboard layer maps it to Enter.) */
    if (k == JAPI_KEY_CTRL('T')) { macro_toggle_record(s); return; }
    if (k == JAPI_KEY_CTRL('P')) { macro_replay(s);        return; }

    /* Ctrl+J opens the Japi Commander (Run -> Japi Commander). */
    if (k == JAPI_KEY_CTRL('J')) { commander_open(s); return; }

    /* Ctrl+A toggles a leading comment (') on the current line or selection
       (Edit->Toggle Comment). */
    if (k == JAPI_KEY_CTRL('A')) { select_all(s);    jbe_follow_cursor(s); return; }
    if (k == JAPI_KEY_CTRL('G')) { toggle_comment(s); jbe_follow_cursor(s); return; }

#ifdef JBB_RUN
    /* Ctrl+B runs the program through the BASIC interpreter (Run menu). */
    if (k == JAPI_KEY_CTRL('B')) { s->run_request = true; return; }
#endif

    /* Tab indents: no selection inserts spaces to the next tab stop; a selection
       shifts the whole block right by one tab width. Shift+Tab dedents the
       current line or the selected block. */
    if (k == JAPI_KEY_TAB) {
        if (JBE_PANE(s)->sel_active) indent_block(s, false);
        else                        edit_tab(s);
        jbe_follow_cursor(s);
        return;
    }
    if (k == JAPI_KEY_STAB) {
        indent_block(s, true);
        jbe_follow_cursor(s);
        return;
    }

    /* View->Split (toggle) is menu-only now; no Ctrl shortcut. */

    /* Ctrl+Tab swaps focus between the two split panes (no-op when there is
       no split open — use View->Split first). Needs the platform's distinct
       Ctrl+Tab code (JAPI_KEY_CTAB); plain Tab still indents. */
    if (k == JAPI_KEY_CTAB) { split_swap_focus(s); return; }

    /* Ctrl+Up / Ctrl+Down shift the split divider up or down. No-op when
       the split isn't open; the helper clamps to keep both panes alive. */
    if (k == JAPI_KEY_CUP)   { split_resize(s, -1); return; }
    if (k == JAPI_KEY_CDOWN) { split_resize(s, +1); return; }

    /* F1 opens the built-in help. Context-sensitive: in an open menu it jumps to
       that menu (or the highlighted item); while editing it opens at the top. */
    if (k == JAPI_KEY_F1) { help_open_context(s); return; }

    /* Open menu from normal mode on Alt+<accelerator>. */
    if (!s->menu_active && (k & 0xFF00) == JAPI_KEY_ALT_BASE) {
        char up = (char)(k & 0xFF);
        if (up >= 'a' && up <= 'z') up -= 32;
        int m = menu_idx_for_letter(up);
        if (m >= 0) { menu_open(s, m); return; }
    }

    /* While a menu is open, Alt+<letter> only runs an item OF THAT MENU. A
       letter that no item in the current menu uses does NOTHING -- it never
       jumps to another menu, even if it matches a different menu's title. To
       switch menus, use the arrow keys (or Esc, then reopen). */
    if (s->menu_active && (k & 0xFF00) == JAPI_KEY_ALT_BASE) {
        char up = (char)(k & 0xFF);
        if (up >= 'a' && up <= 'z') up -= 32;
        int i = item_idx_for_letter(s, s->menu_idx, up);
        if (i >= 0) { s->item_idx = i; menu_activate(s); }
        return;   /* matched -> activated; no match -> nothing happens */
    }

    /* Modal menu key handling. */
    if (s->menu_active) {
        switch (k) {
            case JAPI_KEY_LEFT:
                /* Use menu_open so the Options menu (idx 4) gets its
                   dynamic items rebuilt on every arrival — otherwise
                   navigating into Options via arrows leaves options_n
                   at zero and the next Down crashes with modulo-by-zero. */
                menu_open(s, (s->menu_idx + JBE_MENU_COUNT - 1) % JBE_MENU_COUNT);
                return;
            case JAPI_KEY_RIGHT:
                menu_open(s, (s->menu_idx + 1) % JBE_MENU_COUNT);
                return;
            case JAPI_KEY_UP: {
                int n = menu_item_count(s, s->menu_idx);
                const menu_item_t *items = menu_items_for(s, s->menu_idx);
                for (int step = 0; n > 0 && step < n; step++) {   /* skip separators */
                    s->item_idx = (s->item_idx + n - 1) % n;
                    if (!menu_item_is_sep(&items[s->item_idx])) break;
                }
                return;
            }
            case JAPI_KEY_DOWN: {
                int n = menu_item_count(s, s->menu_idx);
                const menu_item_t *items = menu_items_for(s, s->menu_idx);
                for (int step = 0; n > 0 && step < n; step++) {   /* skip separators */
                    s->item_idx = (s->item_idx + 1) % n;
                    if (!menu_item_is_sep(&items[s->item_idx])) break;
                }
                return;
            }
            case JAPI_KEY_ENTER:  menu_activate(s); return;
            case JAPI_KEY_ESCAPE: menu_close(s);    return;
            default:
                /* A bare letter does NOT run an item: accelerators only fire
                   with Alt held (handled in the Alt+letter block above), so a
                   plain keypress here is simply swallowed while the menu is
                   modal. Use the arrow keys + Enter, or Alt+<red letter>. */
                return;
        }
    }

    /* Map Shift+nav (0x0161..) and Ctrl+Shift+nav (0x0181..) to their plain
       nav code + an "extending" flag. The block flag selects stream-vs-block
       selection mode. */
    bool extending = false;
    bool block_ext = false;
    uint16_t base  = k;
    switch (k) {
        case JAPI_KEY_SUP:    base = JAPI_KEY_UP;    extending = true; break;
        case JAPI_KEY_SDOWN:  base = JAPI_KEY_DOWN;  extending = true; break;
        case JAPI_KEY_SLEFT:  base = JAPI_KEY_LEFT;  extending = true; break;
        case JAPI_KEY_SRIGHT: base = JAPI_KEY_RIGHT; extending = true; break;
        case JAPI_KEY_SHOME:  base = JAPI_KEY_HOME;  extending = true; break;
        case JAPI_KEY_SEND:   base = JAPI_KEY_END;   extending = true; break;
        case JAPI_KEY_SPGUP:  base = JAPI_KEY_PGUP;  extending = true; break;
        case JAPI_KEY_SPGDN:  base = JAPI_KEY_PGDN;  extending = true; break;
        case JAPI_KEY_CSUP:    base = JAPI_KEY_UP;    extending = true; block_ext = true; break;
        case JAPI_KEY_CSDOWN:  base = JAPI_KEY_DOWN;  extending = true; block_ext = true; break;
        case JAPI_KEY_CSLEFT:  base = JAPI_KEY_LEFT;  extending = true; block_ext = true; break;
        case JAPI_KEY_CSRIGHT: base = JAPI_KEY_RIGHT; extending = true; block_ext = true; break;
        case JAPI_KEY_CSHOME:  base = JAPI_KEY_HOME;  extending = true; block_ext = true; break;
        case JAPI_KEY_CSEND:   base = JAPI_KEY_END;   extending = true; block_ext = true; break;
        case JAPI_KEY_CSPGUP:  base = JAPI_KEY_PGUP;  extending = true; block_ext = true; break;
        case JAPI_KEY_CSPGDN:  base = JAPI_KEY_PGDN;  extending = true; block_ext = true; break;
        default: break;
    }
    if (extending) sel_begin(s, block_ext);

    /* Clipboard shortcuts. Copy keeps the selection; cut + paste consume it. */
    if (base == JAPI_KEY_CTRL('C')) { clip_copy_selection(s); return; }
    if (base == JAPI_KEY_CTRL('X')) {
        if (JBE_PANE(s)->sel_active) { clip_copy_selection(s); sel_delete(s); }
        jbe_follow_cursor(s);
        return;
    }
    if (base == JAPI_KEY_CTRL('V')) {
        clip_paste(s);
        if (JBE_PANE(s)->cur_col > JBE_BUF(s)->len[JBE_PANE(s)->cur_row]) JBE_PANE(s)->cur_col = JBE_BUF(s)->len[JBE_PANE(s)->cur_row];
        jbe_follow_cursor(s);
        return;
    }
    /* Ctrl+S must not clear the selection (save is a passive op). */
    if (base == JAPI_KEY_CTRL('S')) { save_or_save_as(s); return; }
    if (base == JAPI_KEY_CTRL('W')) { close_current(s);   return; }

    /* Undo / redo. Like cut/copy/paste they consume any pending extending
       selection state implicitly (undo_apply clears sel_active). */
    if (base == JAPI_KEY_CTRL('Z')) { jbe_undo(s); return; }
    if (base == JAPI_KEY_CTRL('Y')) { jbe_redo(s); return; }

    int line_len = JBE_BUF(s)->len[JBE_PANE(s)->cur_row];
    switch (base) {
        case JAPI_KEY_UP: {
            /* One visual row up: stay in same line if a sub-row exists above,
               otherwise jump to the last sub-row of the previous line. */
            int sub, vcol; cursor_visual_pos(JBE_PANE(s)->cur_col, &sub, &vcol);
            if (sub > 0)               JBE_PANE(s)->cur_col = (sub - 1) * JBE_WRAP_WIDTH + vcol;
            else if (JBE_PANE(s)->cur_row > 0) {
                JBE_PANE(s)->cur_row--;
                int prev_last_sub = visual_rows_of(JBE_BUF(s)->len[JBE_PANE(s)->cur_row]) - 1;
                JBE_PANE(s)->cur_col = prev_last_sub * JBE_WRAP_WIDTH + vcol;
            }
            break;
        }
        case JAPI_KEY_DOWN: {
            int sub, vcol; cursor_visual_pos(JBE_PANE(s)->cur_col, &sub, &vcol);
            int vrs = visual_rows_of(JBE_BUF(s)->len[JBE_PANE(s)->cur_row]);
            if (sub + 1 < vrs)             JBE_PANE(s)->cur_col = (sub + 1) * JBE_WRAP_WIDTH + vcol;
            else if (JBE_PANE(s)->cur_row + 1 < JBE_BUF(s)->n_lines) {
                JBE_PANE(s)->cur_row++;
                JBE_PANE(s)->cur_col = vcol;
            }
            break;
        }
        case JAPI_KEY_LEFT:  JBE_PANE(s)->cur_col = clampi(JBE_PANE(s)->cur_col - 1, 0, line_len);       break;
        case JAPI_KEY_RIGHT: JBE_PANE(s)->cur_col = clampi(JBE_PANE(s)->cur_col + 1, 0, line_len);       break;
        case JAPI_KEY_HOME:  JBE_PANE(s)->cur_col = 0;                                          break;
        case JAPI_KEY_END:   JBE_PANE(s)->cur_col = line_len;                                   break;
        case JAPI_KEY_PGUP: {
            /* Move BOTH viewport and cursor by a screen-ful so the cursor
               keeps its relative position. No "snap to other edge" on a
               direction flip. */
            int d = JBE_VIEW_HEIGHT;
            JBE_PANE(s)->top_row -= d;  if (JBE_PANE(s)->top_row < 0) JBE_PANE(s)->top_row = 0;
            JBE_PANE(s)->cur_row = clampi(JBE_PANE(s)->cur_row - d, 0, JBE_BUF(s)->n_lines - 1);
            break;
        }
        case JAPI_KEY_PGDN: {
            int d = JBE_VIEW_HEIGHT;
            int max_top = JBE_BUF(s)->n_lines - JBE_VIEW_HEIGHT;
            if (max_top < 0) max_top = 0;
            JBE_PANE(s)->top_row += d;  if (JBE_PANE(s)->top_row > max_top) JBE_PANE(s)->top_row = max_top;
            JBE_PANE(s)->cur_row = clampi(JBE_PANE(s)->cur_row + d, 0, JBE_BUF(s)->n_lines - 1);
            break;
        }
        case JAPI_KEY_ENTER:
            if (JBE_PANE(s)->sel_active) sel_delete(s);
            edit_newline(s);
            break;
        case JAPI_KEY_BACKSPACE:
            if (JBE_PANE(s)->sel_active) sel_delete(s);     /* BS over selection = delete it */
            else               edit_backspace(s);
            break;
        case JAPI_KEY_DELETE:
            if (JBE_PANE(s)->sel_active) sel_delete(s);     /* same for forward Delete */
            else               edit_delete(s);
            break;
        default:
            /* Insert any printable byte: ASCII 0x20..0x7E and the CP437 high
               glyphs 0x80..0xFE (accented letters, EUR, currency...). The
               special keys all live at 0x0100+, so anything < 0x100 except DEL
               is text. The renderer already draws high bytes via the CP437 font. */
            if (k >= 32 && k != 127 && k < 0x100) {
                if (JBE_PANE(s)->sel_active) sel_delete(s); /* typing replaces selection */
                edit_insert_char(s, (char)k);
                break;
            }
            return;
    }
    /* Clip the column to the new line's length after a vertical move. */
    if (JBE_PANE(s)->cur_col > JBE_BUF(s)->len[JBE_PANE(s)->cur_row]) JBE_PANE(s)->cur_col = JBE_BUF(s)->len[JBE_PANE(s)->cur_row];
    if (!extending) JBE_PANE(s)->sel_active = false;
    jbe_follow_cursor(s);
}

/* --- Syntax highlighting --------------------------------------------- */

/* Schemes live as pure data (jbe_syn_scheme_t in jbe.h). Z80 below is one
   built-in scheme; the Basic scheme is the other. The same struct
   shape will be filled by the .syn-file parser in stap 2b, so the renderer
   below is the single colour pipeline for both built-in and user schemes. */

static int is_ident_start(unsigned char c);
static int is_ident_cont(unsigned char c);

/* Lightweight label symbol — a slice into the source buffer; valid for one
   render tick. We rebuild the set each render so edits show up immediately. */
typedef struct { const char *ptr; int len; } z80_sym_t;

static int z80_sym_eq_ci(const char *line, int from, int to,
                         const char *p, int n) {
    if (to - from != n) return 0;
    for (int i = 0; i < n; i++) {
        char a = line[from + i], b = p[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

static int z80_in_symset(const char *line, int from, int to,
                         const z80_sym_t *arr, int n) {
    for (int k = 0; k < n; k++)
        if (z80_sym_eq_ci(line, from, to, arr[k].ptr, arr[k].len)) return 1;
    return 0;
}

/* True if `c` starts an EOL comment under this scheme (any character listed
   in scheme->comment_chars). */
static int is_comment_start(const jbe_syn_scheme_t *scheme, unsigned char c) {
    if (!scheme || !scheme->comment_chars) return 0;
    for (const char *p = scheme->comment_chars; *p; p++)
        if ((unsigned char)*p == c) return 1;
    return 0;
}

/* Walk every line, collect every identifier directly followed by ':'.
   Caller owns the returned array (free it; the slice pointers are valid
   until the next edit). Z80-specific lexer (labels-with-colon), driven
   only by the scheme's comment_chars for skip-to-EOL. */
static int z80_collect_labels(const jbe_state_t *s,
                              const jbe_syn_scheme_t *scheme,
                              z80_sym_t **out) {
    int cap = 16, n = 0;
    z80_sym_t *arr = malloc(cap * sizeof *arr);
    if (!arr) { *out = 0; return 0; }
    for (int r = 0; r < JBE_BUF(s)->n_lines; r++) {
        const char *line = JBE_BUF(s)->lines[r];
        int len = JBE_BUF(s)->len[r];
        int i = 0;
        while (i < len) {
            unsigned char c = (unsigned char)line[i];
            if (is_comment_start(scheme, c)) break;    /* comment: skip rest */
            if (is_ident_start(c)) {
                int start = i;
                while (i < len && is_ident_cont((unsigned char)line[i])) i++;
                if (i < len && line[i] == ':') {
                    if (n >= cap) {
                        cap *= 2;
                        z80_sym_t *nw = realloc(arr, cap * sizeof *arr);
                        if (!nw) break;
                        arr = nw;
                    }
                    arr[n].ptr = line + start;
                    arr[n].len = i - start;
                    n++;
                }
            } else {
                i++;
            }
        }
    }
    *out = arr;
    return n;
}

static int word_match_ci(const char *line, int from, int to, const char *word) {
    int n = to - from;
    if ((int)strlen(word) != n) return 0;
    for (int i = 0; i < n; i++) {
        char a = line[from + i], b = word[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

static int word_in_list_ci(const char *line, int from, int to,
                           const char *const *list) {
    if (!list) return 0;
    for (int i = 0; list[i]; i++) if (word_match_ci(line, from, to, list[i])) return 1;
    return 0;
}

static int is_ident_start(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.';
}
static int is_ident_cont(unsigned char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9') || c == '\'';
}

/* --- Built-in Z80 scheme data ----------------------------------------- */

static const char *const Z80_MNEMONICS[] = {
    "ADC","ADD","AND","BIT","CALL","CCF","CP","CPD","CPDR","CPI","CPIR","CPL",
    "DAA","DEC","DI","DJNZ","EI","EX","EXX","HALT","IM","IN","INC","IND",
    "INDR","INI","INIR","JP","JR","LD","LDD","LDDR","LDI","LDIR","NEG","NOP",
    "OR","OTDR","OTIR","OUT","OUTD","OUTI","POP","PUSH","RES","RET","RETI",
    "RETN","RL","RLA","RLC","RLCA","RLD","RR","RRA","RRC","RRCA","RRD","RST",
    "SBC","SCF","SET","SLA","SLL","SRA","SRL","SUB","XOR", 0
};
static const char *const Z80_DIRECTIVES[] = {
    "ORG","DB","DW","DEFB","DEFW","DEFS","DS","DM","DEFM","EQU","INCLUDE",
    "INCBIN","END","IF","ELSE","ENDIF","MACRO","ENDM","ALIGN", 0
};
static const char *const Z80_REGISTERS[] = {
    "A","B","C","D","E","H","L","I","R","AF","BC","DE","HL","IX","IY","SP",
    "AF'","IXH","IXL","IYH","IYL","NZ","NC","PO","PE","P","M","Z", 0
};
static const char *const Z80_EXTENSIONS[] = { ".z80", ".asm", ".s", 0 };

static const jbe_syn_scheme_t Z80_SCHEME = {
    .name            = "Z80",
    .flavor          = JBE_SYN_FLAVOR_Z80,
    .extensions      = Z80_EXTENSIONS,
    .keywords        = Z80_MNEMONICS,
    .directives      = Z80_DIRECTIVES,
    .registers       = Z80_REGISTERS,
    .comment_chars   = ";",
    .color_default   = JBE_FG,        /* white */
    .color_keyword   = VGA_WHITE,     /* mnemonics */
    .color_register  = VGA_MAGENTA,
    .color_directive = 0x0F,          /* VGA_CYAN */
    .color_number    = VGA_YELLOW,
    .color_string    = 0x0F,          /* VGA_CYAN */
    .color_comment   = VGA_GREEN,
    .color_label     = 0x3A,          /* orange */
};

/* --- Built-in Basic scheme data --------------------------------------- */

/* ~80 keywords: core BASIC control flow + statements + functions, plus the
   most-used hardware/peripheral keywords. (Japi Base implements a subset of
   MMBasic, so the scheme is just called "Basic".) The user can override the lot
   via C:config/syntax/basic.syn for a longer or shorter list. Case-insensitive. */
static const char *const MMBASIC_KEYWORDS[] = {
    /* Core control flow / structure */
    "IF","THEN","ELSE","ELSEIF","ENDIF","END",
    "FOR","TO","STEP","NEXT","WHILE","WEND","DO","LOOP","UNTIL","EXIT",
    "GOTO","GOSUB","RETURN","ON","SELECT","CASE",
    "SUB","ENDSUB","FUNCTION","ENDFUNCTION","CALL","TYPE",
    /* Data / variables */
    "LET","DIM","CONST","STATIC","LOCAL","GLOBAL","REDIM","ERASE","INC","ARRAY","SORT","MATH","LONGSTRING",
    "DATA","READ","RESTORE","REM",
    /* I/O — terminal */
    "PRINT","INPUT","LINE","WRITE","CLS","COLOUR","COLOR","FONT","LOCATE",
    /* I/O — files */
    "OPEN","CLOSE","FILE","FILES","KILL","MKDIR","RMDIR","NAME","SEEK","CHDIR","RENAME",
    /* Operators (textual) */
    "AND","OR","XOR","NOT","MOD","DIV",
    /* Bitmap graphics (Japi Base) */
    "GRAPHICS","PIXEL","BOX","CIRCLE","LINE","TRIANGLE","RBOX","ARC","MAP",
    /* peripheral keywords */
    "PIN","SETPIN","PULSIN","PWM","SERVO","TIMER","PAUSE",
    "SPI","I2C","UART","COM","PORT","SOUND","PLAY","TONE",
    /* Misc commands */
    "RUN","STOP","CONT","NEW","LIST","SAVE","LOAD","CHAIN","ERROR","OPTION",
    /* JBB statements + declare-first type words */
    "CURSOR","CONTINUE","RANDOMIZE","QUIT","EDIT","SETTITLE","MEMORY","CLEAR",
    "TRACE","TRON","TROFF","BIT","BYTE","AS","INTEGER","FLOAT","STRING",
    "OUTPUT","APPEND","PRESERVE",
    0
};

/* JBB built-in functions -- highlighted in their own colour (color_directive),
   so they read distinctly from commands/keywords. */
static const char *const MMBASIC_FUNCTIONS[] = {
    /* math */
    "ABS","ACOS","ASIN","ATN","ATAN2","ATAN3","COS","COSH","SIN","SINH","TAN","TANH",
    "EXP","LOG","LOG10","SQR","PI","DEG","RAD","FIX","INT","CINT","SGN","MAX","MIN","RND","CHOICE",
    /* strings */
    "LEN","ASC","CHR$","LEFT$","RIGHT$","MID$","INSTR","UCASE$","LCASE$","TRIM$",
    "SPACE$","STRING$","FORMAT$","FIELD$",
    /* conversion / bits / colour */
    "VAL","STR$","HEX$","OCT$","BIN$","BIN2STR$","STR2BIN","RGB",
    /* date / time */
    "DATE$","TIME$","NOW","DATETIME$","EPOCH","DAY$",
    /* arrays / files / console */
    "BOUND","EOF","LOF","LOC","INPUT$","DIR$","CWD$","INKEY$","POS",
    /* longstring / advanced */
    "LLEN","LGETBYTE","LGETSTR$","LINSTR","EVAL",
    0
};
static const char *const MMBASIC_EXTENSIONS[] = { ".bas", ".mmb", 0 };

static const jbe_syn_scheme_t MMBASIC_SCHEME = {
    .name            = "Basic",
    .flavor          = JBE_SYN_FLAVOR_BASIC,
    .extensions      = MMBASIC_EXTENSIONS,
    .keywords        = MMBASIC_KEYWORDS,
    .directives      = MMBASIC_FUNCTIONS,  /* built-in functions, own colour */
    .registers       = 0,           /* unused for BASIC */
    .comment_chars   = "'",         /* REM is matched as a keyword */
    .color_default   = JBE_FG,      /* white */
    .color_keyword   = 0x0F,        /* VGA_CYAN — commands / keywords */
    .color_register  = VGA_WHITE,   /* unused */
    .color_directive = 0x38,        /* soft orange — built-in functions */
    .color_number    = VGA_YELLOW,
    .color_string    = VGA_MAGENTA,  /* magenta for quoted strings */
    .color_comment   = VGA_GREEN,
    .color_label     = VGA_WHITE,   /* unused */
};

/* Registry of all built-in schemes. Look-up paths (id → scheme,
   extension → scheme, name → scheme) iterate over this list.
   Forward-declared at the top of the file. */
const jbe_syn_scheme_t *const JBE_BUILTIN_SCHEMES[] = {
    &Z80_SCHEME,
    &MMBASIC_SCHEME,
    0
};

/* Look up a built-in scheme by name (case-insensitive). Returns NULL
   when name is empty or no built-in matches. Forward-declared at the
   top of the file. */
static const jbe_syn_scheme_t *scheme_by_name(const char *name) {
    if (!name || !*name) return 0;
    for (int i = 0; JBE_BUILTIN_SCHEMES[i]; i++) {
        const jbe_syn_scheme_t *sch = JBE_BUILTIN_SCHEMES[i];
        if (sch->name && syn_streq_ci(sch->name, name)) return sch;
    }
    return 0;
}

/* --- .syn file parser ------------------------------------------------- */

/* Recognised colour names mapped to platform VGA byte values. Order
   doesn't matter; lookup is linear (≤ 9 entries). Unknown names fall
   back to white at parse time. */
static const struct { const char *name; uint8_t code; } JBE_SYN_COLORS[] = {
    { "black",   VGA_BLACK   },
    { "blue",    VGA_BLUE    },
    { "green",   VGA_GREEN   },
    { "cyan",    VGA_CYAN    },
    { "red",     VGA_RED     },
    { "magenta", VGA_MAGENTA },
    { "yellow",  VGA_YELLOW  },
    { "white",   VGA_WHITE   },
    { "orange",  0x3A        },
    { 0, 0 }
};

static int syn_streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static uint8_t syn_parse_color(const char *value) {
    for (int i = 0; JBE_SYN_COLORS[i].name; i++)
        if (syn_streq_ci(value, JBE_SYN_COLORS[i].name))
            return JBE_SYN_COLORS[i].code;
    return VGA_WHITE;
}

static jbe_syn_flavor_t syn_parse_flavor(const char *value) {
    if (syn_streq_ci(value, "z80"))   return JBE_SYN_FLAVOR_Z80;
    if (syn_streq_ci(value, "basic")) return JBE_SYN_FLAVOR_BASIC;
    if (syn_streq_ci(value, "none")) return JBE_SYN_FLAVOR_NONE;
    return JBE_SYN_FLAVOR_NONE;
}

/* Allocate a NUL-terminated copy of [s, s+n). */
static char *syn_dup_n(const char *s, int n) {
    char *r = malloc((size_t)n + 1);
    if (!r) return 0;
    if (n > 0) memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* Strip leading/trailing whitespace in place. Returns the new start. */
static char *syn_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) n--;
    s[n] = 0;
    return s;
}

/* Split `value` by any of the separator chars in `seps` into a freshly
   malloc'd NULL-terminated char* array. Empty tokens are skipped. Each
   stored string is itself malloc'd. Returns NULL on allocation failure. */
static char **syn_split(const char *value, const char *seps) {
    int cap = 8, n = 0;
    char **arr = malloc((size_t)cap * sizeof *arr);
    if (!arr) return 0;
    int i = 0, len = (int)strlen(value);
    while (i < len) {
        while (i < len && strchr(seps, value[i])) i++;
        if (i >= len) break;
        int start = i;
        while (i < len && !strchr(seps, value[i])) i++;
        if (n + 1 >= cap) {
            int nc = cap * 2;
            char **nw = realloc(arr, (size_t)nc * sizeof *arr);
            if (!nw) { for (int k = 0; k < n; k++) free(arr[k]); free(arr); return 0; }
            arr = nw; cap = nc;
        }
        char *tok = syn_dup_n(value + start, i - start);
        if (!tok) { for (int k = 0; k < n; k++) free(arr[k]); free(arr); return 0; }
        arr[n++] = tok;
    }
    arr[n] = 0;
    return arr;
}

/* Free a NULL-terminated char** built by syn_split. */
static void syn_free_list(char **list) {
    if (!list) return;
    for (int i = 0; list[i]; i++) free(list[i]);
    free(list);
}

void jbe_syn_scheme_free(jbe_syn_scheme_t *scheme) {
    if (!scheme) return;
    if (!scheme->_owns_storage) return;     /* never free a built-in */
    free((void *)scheme->name);
    free((void *)scheme->comment_chars);
    syn_free_list((char **)scheme->extensions);
    syn_free_list((char **)scheme->keywords);
    syn_free_list((char **)scheme->directives);
    syn_free_list((char **)scheme->registers);
    free(scheme);
}

jbe_syn_scheme_t *jbe_syn_scheme_load(const char *path) {
    /* static: keep the ~550-byte FatFs FIL inside japi_file_t off the ~2 KB
       Core 0 stack. Called single-threaded (during a load or from the Options
       menu) and never re-entrantly, so static storage is safe (see jbe_load). */
    static japi_file_t f;
    if (!japi_fopen(&f, path, JAPI_READ)) return 0;
    int sz = japi_fsize(&f);
    if (sz <= 0) { japi_fclose(&f); return 0; }
    char *buf = malloc((size_t)sz);
    if (!buf) { japi_fclose(&f); return 0; }
    int got = japi_fread(&f, buf, sz);
    japi_fclose(&f);
    if (got != sz) { free(buf); return 0; }
    jbe_syn_scheme_t *p = jbe_syn_scheme_parse(buf, sz);
    free(buf);
    return p;
}

jbe_syn_scheme_t *jbe_syn_scheme_parse(const char *text, int len) {
    if (!text || len <= 0) return 0;
    jbe_syn_scheme_t *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->_owns_storage = true;
    s->color_default   = VGA_WHITE;
    s->color_keyword   = VGA_WHITE;
    s->color_register  = VGA_WHITE;
    s->color_directive = VGA_WHITE;
    s->color_number    = VGA_WHITE;
    s->color_string    = VGA_WHITE;
    s->color_comment   = VGA_WHITE;
    s->color_label     = VGA_WHITE;

    int i = 0;
    while (i < len) {
        int line_start = i;
        while (i < len && text[i] != '\n') i++;
        int line_end = i;
        if (i < len) i++;                       /* step past \n */
        char *line = syn_dup_n(text + line_start, line_end - line_start);
        if (!line) { jbe_syn_scheme_free(s); return 0; }
        char *trimmed = syn_trim(line);
        if (*trimmed == 0 || *trimmed == '#') { free(line); continue; }
        char *eq = strchr(trimmed, '=');
        if (!eq) { free(line); continue; }       /* malformed → skip silently */
        *eq = 0;
        char *key   = syn_trim(trimmed);
        char *value = syn_trim(eq + 1);

        if      (syn_streq_ci(key, "name"))          { free((void *)s->name);          s->name          = syn_dup_n(value, (int)strlen(value)); }
        else if (syn_streq_ci(key, "flavor"))        {                                  s->flavor        = syn_parse_flavor(value); }
        else if (syn_streq_ci(key, "comment"))       { free((void *)s->comment_chars); s->comment_chars = syn_dup_n(value, (int)strlen(value)); }
        else if (syn_streq_ci(key, "extensions"))    { syn_free_list((char **)s->extensions); s->extensions = (const char *const *)syn_split(value, ", \t"); }
        else if (syn_streq_ci(key, "keywords"))      { syn_free_list((char **)s->keywords);   s->keywords   = (const char *const *)syn_split(value, " \t"); }
        else if (syn_streq_ci(key, "directives"))    { syn_free_list((char **)s->directives); s->directives = (const char *const *)syn_split(value, " \t"); }
        else if (syn_streq_ci(key, "registers"))     { syn_free_list((char **)s->registers);  s->registers  = (const char *const *)syn_split(value, " \t"); }
        else if (syn_streq_ci(key, "color_default"))   s->color_default   = syn_parse_color(value);
        else if (syn_streq_ci(key, "color_keyword"))   s->color_keyword   = syn_parse_color(value);
        else if (syn_streq_ci(key, "color_register"))  s->color_register  = syn_parse_color(value);
        else if (syn_streq_ci(key, "color_directive")) s->color_directive = syn_parse_color(value);
        else if (syn_streq_ci(key, "color_number"))    s->color_number    = syn_parse_color(value);
        else if (syn_streq_ci(key, "color_string"))    s->color_string    = syn_parse_color(value);
        else if (syn_streq_ci(key, "color_comment"))   s->color_comment   = syn_parse_color(value);
        else if (syn_streq_ci(key, "color_label"))     s->color_label     = syn_parse_color(value);
        /* unknown key → silently ignored so future fields stay forward-compatible */

        free(line);
    }

    /* Minimum viable scheme has a name and a flavor; anything else is
       optional. Without those two we refuse the parse so the caller can
       fall back to the built-in. */
    if (!s->name || s->flavor == JBE_SYN_FLAVOR_NONE) {
        jbe_syn_scheme_free(s);
        return 0;
    }
    return s;
}

/* --- Dynamic Options→Syntax menu builder ------------------------------ */

/* Append one entry to the editor's options table. Pick an accelerator
   letter that doesn't already exist; if none free, use the first letter
   anyway (collisions are tolerated — pijltjes + Enter still works). */
static void options_add(jbe_state_t *s, const char *display_name) {
    if (s->options_n >= JBE_OPTIONS_MAX_ITEMS) return;
    int i = s->options_n;
    strncpy(s->options_names[i], display_name, sizeof s->options_names[i] - 1);
    s->options_names[i][sizeof s->options_names[i] - 1] = 0;
    /* Label is "Syntax: <name>" — or just "Syntax: None" when name is "". */
    if (display_name[0])
        snprintf(s->options_labels[i], sizeof s->options_labels[i],
                 "Syntax: %s", display_name);
    else
        snprintf(s->options_labels[i], sizeof s->options_labels[i],
                 "Syntax: None");
    /* Pick accelerator: 'N' for None, else first letter of the name
       (uppercased). Collisions are not actively avoided — pressing the
       letter activates the first match. */
    char accel = 'N';
    if (display_name[0]) {
        accel = display_name[0];
        if (accel >= 'a' && accel <= 'z') accel -= 32;
    }
    s->options_items[i].label = s->options_labels[i];
    s->options_items[i].accel = accel;
    s->options_items[i].hint  = 0;
    s->options_n++;
}

/* True if a scheme name is already in s->options_names (case-insensitive). */
static bool options_has_name(const jbe_state_t *s, const char *name) {
    for (int i = 0; i < s->options_n; i++)
        if (syn_streq_ci(s->options_names[i], name)) return true;
    return false;
}

/* Peek a .syn file just far enough to read the `name=` field. Avoids a
   full parse + alloc; useful when scanning many files for the menu. */
static bool peek_syn_name(const char *path, char *name_out, int name_max) {
    if (name_max <= 0) return false;
    jbe_syn_scheme_t *p = jbe_syn_scheme_load(path);
    if (!p) return false;
    bool ok = p->name && p->name[0];
    if (ok) {
        strncpy(name_out, p->name, (size_t)name_max - 1);
        name_out[name_max - 1] = 0;
    }
    jbe_syn_scheme_free(p);
    return ok;
}

static void rebuild_options_menu(jbe_state_t *s) {
    s->options_n = 0;
    /* 1. "None" always first. */
    options_add(s, "");
    /* 2. Every built-in. */
    for (int b = 0; JBE_BUILTIN_SCHEMES[b]; b++) {
        const jbe_syn_scheme_t *sch = JBE_BUILTIN_SCHEMES[b];
        if (sch->name && sch->name[0]) options_add(s, sch->name);
    }
    /* 3. Every user .syn whose name is not yet listed. A file whose name=
       matches a built-in is *not* a separate item — the look-up path already
       routes that built-in's resolution through the file.

       User schemes live on C: (the built-in flash), which is always mounted,
       rather than on the removable SD card A:. That matters because this menu
       is rebuilt every time the Options menu is reached, including by merely
       arrowing onto it: scanning A: would risk a slow mount probe whenever no
       card is present and stall the menu, whereas C: is always there and fast. */
    japi_dir_t d;
    if (japi_opendir(&d, "C:config/syntax")) {
        char fname[JBE_NAME_MAX + 1];
        while (japi_readdir(&d, fname, sizeof fname)) {
            int n = (int)strlen(fname);
            if (n < 5) continue;
            const char *ext = fname + n - 4;
            if (!(ext[0] == '.' && (ext[1] == 's' || ext[1] == 'S')
                                 && (ext[2] == 'y' || ext[2] == 'Y')
                                 && (ext[3] == 'n' || ext[3] == 'N'))) continue;
            /* Build "C:config/syntax/<fname>" then peek the name= field. */
            char path[64];
            if ((int)strlen(fname) + (int)strlen("C:config/syntax/") + 1 > (int)sizeof path)
                continue;
            snprintf(path, sizeof path, "C:config/syntax/%s", fname);
            char sname[JBE_NAME_MAX + 1];
            if (!peek_syn_name(path, sname, sizeof sname)) continue;
            if (options_has_name(s, sname)) continue;
            options_add(s, sname);
        }
        japi_closedir(&d);
    }
    /* 4. The CPU-speed tool (not a syntax scheme). Remember its row so the
       activation can tell it apart from the schemes. */
    s->cpu_item_index = -1;
    if (s->options_n < JBE_OPTIONS_MAX_ITEMS) {
        int i = s->options_n;
        s->options_names[i][0] = 0;
        snprintf(s->options_labels[i], sizeof s->options_labels[i], "CPU speed...");
        s->options_items[i].label = s->options_labels[i];
        s->options_items[i].accel = 'C';
        s->options_items[i].hint  = 0;
        s->cpu_item_index = i;
        s->options_n++;
    }
    /* Sentinel for the generic menu helpers. */
    s->options_items[s->options_n].label = 0;
    s->options_items[s->options_n].accel = 0;
    s->options_items[s->options_n].hint  = 0;
}

/* Fill `fg_out` (one byte per source byte) with Z80-flavor token colours,
   reading keyword lists and colours from `scheme`. The `labels` set
   (built once per render) colours bare references to label names too —
   JP DONE / CALL PRINT_CHAR / etc. */
static void z80_colour_line(const jbe_syn_scheme_t *scheme,
                            const char *line, int len, uint8_t *fg_out,
                            const z80_sym_t *labels, int n_labels) {
    for (int i = 0; i < len; i++) fg_out[i] = scheme->color_default;
    int i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)line[i];
        if (is_comment_start(scheme, c)) {             /* comment to EOL */
            for (int k = i; k < len; k++) fg_out[k] = scheme->color_comment;
            return;
        }
        if (c == '\'' || c == '"') {                   /* string literal */
            unsigned char q = c;
            int start = i; i++;
            while (i < len && (unsigned char)line[i] != q) i++;
            if (i < len) i++;                          /* closing quote */
            for (int k = start; k < i; k++) fg_out[k] = scheme->color_string;
            continue;
        }
        if ((c >= '0' && c <= '9') || c == '$' || c == '%') {
            int start = i;
            if (c == '$' || c == '%') i++;
            while (i < len) {
                unsigned char d = (unsigned char)line[i];
                if ((d >= '0' && d <= '9') || (d >= 'A' && d <= 'F') ||
                    (d >= 'a' && d <= 'f') || d == 'x' || d == 'X' || d == 'h' || d == 'H')
                    i++;
                else break;
            }
            if (i > start + ((c == '$' || c == '%') ? 1 : 0)) {
                for (int k = start; k < i; k++) fg_out[k] = scheme->color_number;
                continue;
            }
            i = start + 1; continue;
        }
        if (is_ident_start(c)) {
            int start = i;
            while (i < len && is_ident_cont((unsigned char)line[i])) i++;
            uint8_t colour = scheme->color_default;
            /* Label = identifier immediately followed by ':'. Checked first so
               keyword-looking labels (rare in user code) still get the
               label colour. */
            if (i < len && line[i] == ':')                           colour = scheme->color_label;
            else if (word_in_list_ci(line, start, i, scheme->keywords))   colour = scheme->color_keyword;
            else if (word_in_list_ci(line, start, i, scheme->directives)) colour = scheme->color_directive;
            else if (word_in_list_ci(line, start, i, scheme->registers))  colour = scheme->color_register;
            else if (z80_in_symset(line, start, i, labels, n_labels))     colour = scheme->color_label;
            if (colour != scheme->color_default)
                for (int k = start; k < i; k++) fg_out[k] = colour;
            continue;
        }
        i++;
    }
}

/* Fill `fg_out` with BASIC-flavor token colours. Differences with Z80:
   - Comments start with any char in scheme->comment_chars (typically '),
     OR with the keyword REM at the start of an identifier token.
   - Strings are double-quoted only — apostrophe is the comment char.
   - Numbers can be plain decimal, &H... (hex), &O... (octal),
     &B... (binary). Leading digit-only tokens are line numbers.
   - No labels-with-colon: in BASIC, ':' is a statement separator. */
static void basic_colour_line(const jbe_syn_scheme_t *scheme,
                              const char *line, int len, uint8_t *fg_out) {
    for (int i = 0; i < len; i++) fg_out[i] = scheme->color_default;
    int i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)line[i];
        if (is_comment_start(scheme, c)) {              /* ' to EOL */
            for (int k = i; k < len; k++) fg_out[k] = scheme->color_comment;
            return;
        }
        if (c == '"') {                                  /* string literal */
            int start = i; i++;
            while (i < len && (unsigned char)line[i] != '"') i++;
            if (i < len) i++;
            for (int k = start; k < i; k++) fg_out[k] = scheme->color_string;
            continue;
        }
        if (c == '&' && i + 1 < len &&
            (line[i+1] == 'H' || line[i+1] == 'h' ||
             line[i+1] == 'O' || line[i+1] == 'o' ||
             line[i+1] == 'B' || line[i+1] == 'b')) {    /* &H.. &O.. &B.. */
            int start = i; i += 2;
            while (i < len) {
                unsigned char d = (unsigned char)line[i];
                if ((d >= '0' && d <= '9') || (d >= 'A' && d <= 'F') ||
                    (d >= 'a' && d <= 'f')) i++;
                else break;
            }
            for (int k = start; k < i; k++) fg_out[k] = scheme->color_number;
            continue;
        }
        if (c >= '0' && c <= '9') {                      /* decimal / float */
            int start = i;
            while (i < len) {
                unsigned char d = (unsigned char)line[i];
                if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E') i++;
                else break;
            }
            for (int k = start; k < i; k++) fg_out[k] = scheme->color_number;
            continue;
        }
        if (is_ident_start(c)) {
            int start = i;
            while (i < len && is_ident_cont((unsigned char)line[i])) i++;
            /* REM acts like ' — comment-to-EOL from this token onwards. */
            if (word_match_ci(line, start, i, "REM")) {
                for (int k = start; k < len; k++) fg_out[k] = scheme->color_comment;
                return;
            }
            if (word_in_list_ci(line, start, i, scheme->keywords)) {
                for (int k = start; k < i; k++) fg_out[k] = scheme->color_keyword;
            } else if (word_in_list_ci(line, start, i, scheme->directives)) {
                /* BASIC uses the directives slot for built-in functions, so they
                   get their own colour (color_directive) distinct from commands. */
                for (int k = start; k < i; k++) fg_out[k] = scheme->color_directive;
            }
            continue;
        }
        i++;
    }
}

/* ===================================================================== *
 *  Embellish / Format -- BASIC-aware source tidy-up (Edit -> Format).    *
 *                                                                        *
 *  Re-indents block structure (2 spaces / level), trims trailing
 *  whitespace, and uppercases keyword / built-in-function tokens to their
 *  canonical spelling. Variables, numbers, string bodies and comment
 *  bodies are left untouched. The tokenising rules below mirror
 *  basic_colour_line() above (comment '/REM, "..." strings, &H/&O/&B and
 *  decimal numbers, identifiers); kept separate because the colouriser
 *  emits colours, while the formatter needs token spans + a class. It
 *  always uses the BASIC tables (MMBASIC_KEYWORDS / MMBASIC_FUNCTIONS),
 *  independent of the buffer's active highlight scheme.
 * ===================================================================== */

enum { FMT_NONE = 0, FMT_OPEN, FMT_CLOSE, FMT_MID };   /* block effect of a keyword */

/* The canonical (uppercase) spelling of a keyword/function span, or NULL. */
static const char *fmt_lookup_canon(const char *line, int from, int to,
                                    const char *const *list) {
    if (!list) return NULL;
    for (int i = 0; list[i]; i++)
        if (word_match_ci(line, from, to, list[i])) return list[i];
    return NULL;
}

/* Index just past a number token starting at i (i is '&' or a digit). */
static int fmt_skip_number(const char *src, int len, int i) {
    if (src[i] == '&' && i + 1 < len) {
        unsigned char b = (unsigned char)src[i + 1];
        if (b=='H'||b=='h'||b=='O'||b=='o'||b=='B'||b=='b') {
            i += 2;
            while (i < len) { unsigned char d=(unsigned char)src[i];
                if ((d>='0'&&d<='9')||(d>='A'&&d<='F')||(d>='a'&&d<='f')) i++; else break; }
            return i;
        }
        return i + 1;   /* a lone '&' */
    }
    while (i < len) { unsigned char d=(unsigned char)src[i];
        if ((d>='0'&&d<='9')||d=='.'||d=='e'||d=='E') i++; else break; }
    return i;
}

/* Skip whitespace from `from`; if an identifier follows, fill [*ps,*pe) and
   return true. Used for two-word keyword look-ahead (END IF, SELECT CASE...). */
static bool fmt_peek_ident(const char *src, int len, int from, int *ps, int *pe) {
    int i = from;
    while (i < len && (src[i]==' '||src[i]=='\t')) i++;
    if (i >= len || !is_ident_start((unsigned char)src[i])) return false;
    int s = i;
    while (i < len && is_ident_cont((unsigned char)src[i])) i++;
    *ps = s; *pe = i; return true;
}

/* Index just past a THEN keyword on/after `from` (skipping strings and other
   tokens), or -1 if a comment/EOL comes first (no THEN). */
static int fmt_after_then(const char *src, int len, int from) {
    int i = from;
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (c==' '||c=='\t') { i++; continue; }
        if (is_comment_start(&MMBASIC_SCHEME, c)) return -1;     /* ' comment */
        if (c=='"') { i++; while (i<len && src[i]!='"') i++; if (i<len) i++; continue; }
        if (c=='&' || (c>='0'&&c<='9')) { i = fmt_skip_number(src, len, i); continue; }
        if (is_ident_start(c)) {
            int s=i; while (i<len && is_ident_cont((unsigned char)src[i])) i++;
            if (word_match_ci(src,s,i,"REM"))  return -1;        /* comment */
            if (word_match_ci(src,s,i,"THEN")) return i;
            continue;
        }
        i++;
    }
    return -1;
}

/* Classify a keyword token [s,e). For two-word forms (END IF, SELECT CASE,
   ELSE IF) and a multi-line IF, sets *consumed_end past everything consumed. */
static int fmt_classify(const char *src, int len, int s, int e, int *consumed_end) {
    *consumed_end = e;
    if (word_match_ci(src,s,e,"FOR")||word_match_ci(src,s,e,"DO")||
        word_match_ci(src,s,e,"SUB")||word_match_ci(src,s,e,"FUNCTION")||
        word_match_ci(src,s,e,"TYPE")||word_match_ci(src,s,e,"WHILE")) return FMT_OPEN;
    if (word_match_ci(src,s,e,"NEXT")||word_match_ci(src,s,e,"LOOP")||
        word_match_ci(src,s,e,"WEND")||word_match_ci(src,s,e,"ENDIF")||
        word_match_ci(src,s,e,"ENDSUB")||word_match_ci(src,s,e,"ENDFUNCTION")) return FMT_CLOSE;
    if (word_match_ci(src,s,e,"ELSEIF")||word_match_ci(src,s,e,"CASE")) return FMT_MID;
    if (word_match_ci(src,s,e,"ELSE")) {                         /* ELSE [IF] */
        int ps,pe;
        if (fmt_peek_ident(src,len,e,&ps,&pe) && word_match_ci(src,ps,pe,"IF")) *consumed_end = pe;
        return FMT_MID;
    }
    if (word_match_ci(src,s,e,"SELECT")) {                       /* opener only with CASE */
        int ps,pe;
        if (fmt_peek_ident(src,len,e,&ps,&pe) && word_match_ci(src,ps,pe,"CASE")) {
            *consumed_end = pe; return FMT_OPEN;
        }
        return FMT_NONE;
    }
    if (word_match_ci(src,s,e,"END")) {                          /* END <word> closes; bare END = program end */
        int ps,pe;
        if (fmt_peek_ident(src,len,e,&ps,&pe) &&
            (word_match_ci(src,ps,pe,"IF")||word_match_ci(src,ps,pe,"SUB")||
             word_match_ci(src,ps,pe,"FUNCTION")||word_match_ci(src,ps,pe,"SELECT")||
             word_match_ci(src,ps,pe,"TYPE"))) { *consumed_end = pe; return FMT_CLOSE; }
        return FMT_NONE;
    }
    if (word_match_ci(src,s,e,"IF")) {                           /* opener only if multi-line */
        int after = fmt_after_then(src, len, e);
        if (after < 0) return FMT_NONE;                          /* no THEN */
        int j = after; while (j<len && (src[j]==' '||src[j]=='\t')) j++;
        if (j >= len) return FMT_OPEN;                           /* nothing after THEN */
        if (is_comment_start(&MMBASIC_SCHEME,(unsigned char)src[j])) return FMT_OPEN;
        if (is_ident_start((unsigned char)src[j])) {
            int k=j; while (k<len && is_ident_cont((unsigned char)src[k])) k++;
            if (word_match_ci(src,j,k,"REM")) return FMT_OPEN;
        }
        return FMT_NONE;                                         /* statement after THEN = single-line */
    }
    return FMT_NONE;
}

/* Scan one line: *net = sum of opener(+1)/closer(-1) effects; *leading_dedent =
   the first significant token is a closer or mid-block keyword. */
static void fmt_scan_line(const char *src, int len, int *net_out, bool *leading_dedent_out) {
    int net = 0; bool first = true, leading_dedent = false; int i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (c==' '||c=='\t') { i++; continue; }
        if (is_comment_start(&MMBASIC_SCHEME, c)) break;
        if (c=='"') { i++; while (i<len && src[i]!='"') i++; if (i<len) i++; first=false; continue; }
        if (c=='&' || (c>='0'&&c<='9')) { i = fmt_skip_number(src,len,i); first=false; continue; }
        if (is_ident_start(c)) {
            int s=i; while (i<len && is_ident_cont((unsigned char)src[i])) i++;
            if (word_match_ci(src,s,i,"REM")) break;
            int ce, cls = fmt_classify(src, len, s, i, &ce);
            if (first) { leading_dedent = (cls==FMT_CLOSE || cls==FMT_MID); first = false; }
            if (cls==FMT_OPEN) net++; else if (cls==FMT_CLOSE) net--;
            i = ce;
            continue;
        }
        first = false; i++;
    }
    *net_out = net; *leading_dedent_out = leading_dedent;
}

/* Rebuild one line: indent_level*2 spaces, then the body with keyword/function
   tokens uppercased to canonical and everything else verbatim, then trailing
   whitespace trimmed. Returns the new length, or -1 if it would overflow out. */
static int fmt_rewrite_line(const char *src, int len, int indent_level,
                            char *out, int out_cap) {
    int o = 0;
    for (int k = 0; k < indent_level*2; k++) { if (o>=out_cap) return -1; out[o++]=' '; }
    int i = 0; while (i<len && (src[i]==' '||src[i]=='\t')) i++;   /* drop old indent */
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (is_comment_start(&MMBASIC_SCHEME, c)) {                /* ' .. : verbatim to EOL */
            while (i<len) { if (o>=out_cap) return -1; out[o++]=src[i++]; }
            break;
        }
        if (c=='"') {                                              /* string: verbatim incl quotes */
            { if (o>=out_cap) return -1; out[o++]=src[i++]; }
            while (i<len && src[i]!='"') { if (o>=out_cap) return -1; out[o++]=src[i++]; }
            if (i<len) { if (o>=out_cap) return -1; out[o++]=src[i++]; }
            continue;
        }
        if (c=='&' || (c>='0'&&c<='9')) {                          /* number: verbatim */
            int e = fmt_skip_number(src, len, i);
            while (i<e) { if (o>=out_cap) return -1; out[o++]=src[i++]; }
            continue;
        }
        if (is_ident_start(c)) {
            int s=i; while (i<len && is_ident_cont((unsigned char)src[i])) i++;
            if (word_match_ci(src,s,i,"REM")) {                    /* canonical REM + verbatim rest */
                const char *p="REM"; while (*p){ if(o>=out_cap)return -1; out[o++]=*p++; }
                while (i<len){ if(o>=out_cap)return -1; out[o++]=src[i++]; }
                break;
            }
            const char *can = fmt_lookup_canon(src,s,i,MMBASIC_KEYWORDS);
            if (!can) can = fmt_lookup_canon(src,s,i,MMBASIC_FUNCTIONS);
            if (can) { for (const char *p=can; *p; p++){ if(o>=out_cap)return -1; out[o++]=*p; } }
            else     { for (int k=s; k<i; k++){ if(o>=out_cap)return -1; out[o++]=src[k]; } }
            continue;
        }
        { if (o>=out_cap) return -1; out[o++]=src[i++]; }          /* operator/other: verbatim */
    }
    while (o>0 && (out[o-1]==' '||out[o-1]=='\t')) o--;            /* trim trailing */
    return o;
}

/* Embellish the active pane: re-indent + uppercase keywords + trim trailing
   whitespace, over the selection (whole lines) or the whole file. Done as one
   stream DELETE of the old region + one stream INSERT of the rebuilt region, so
   it integrates with undo/redo (a full undo is two Ctrl+Z steps). Aborts without
   changing anything if the rebuilt text won't fit or an allocation fails. */
void jbe_format(jbe_state_t *s) {
    jbe_buffer_t *b = JBE_BUF(s);
    if (b->n_lines == 0) return;

    int r1, r2;
    if (JBE_PANE(s)->sel_active) {
        int c1, c2;
        if (JBE_PANE(s)->sel_block) sel_range_block(s, &r1, &c1, &r2, &c2);
        else                        sel_range(s, &r1, &c1, &r2, &c2);
    } else { r1 = 0; r2 = b->n_lines - 1; }
    if (r1 < 0) r1 = 0;
    if (r2 > b->n_lines - 1) r2 = b->n_lines - 1;
    if (r2 < r1) return;

    /* Entry indent level = the block depth of the lines above the region. */
    int level = 0;
    for (int r = 0; r < r1; r++) {
        int net; bool dd; fmt_scan_line(b->lines[r], b->len[r], &net, &dd);
        level += net; if (level < 0) level = 0;
    }

    /* Capture the old region ('\n'-joined) -- this becomes the DELETE payload. */
    int old_len = 0;
    for (int r = r1; r <= r2; r++) old_len += b->len[r] + (r < r2 ? 1 : 0);
    char *old_text = malloc(old_len ? (size_t)old_len : 1);
    if (!old_text) return;
    { int o = 0; for (int r = r1; r <= r2; r++) {
        memcpy(old_text + o, b->lines[r], b->len[r]); o += b->len[r];
        if (r < r2) old_text[o++] = '\n'; } }

    /* Build the reformatted region. Generous cap (indent growth + separators). */
    int cap = old_len + (r2 - r1 + 1) * 64 + 256;
    char *new_text = malloc((size_t)cap);
    if (!new_text) { free(old_text); return; }
    int nlen = 0, lvl = level;
    for (int r = r1; r <= r2; r++) {
        int net; bool dd; fmt_scan_line(b->lines[r], b->len[r], &net, &dd);
        int indent = lvl - (dd ? 1 : 0); if (indent < 0) indent = 0;
        int n = fmt_rewrite_line(b->lines[r], b->len[r], indent, new_text + nlen, cap - nlen - 1);
        if (n < 0) { free(old_text); free(new_text); return; }   /* overflow: leave file untouched */
        nlen += n;
        if (r < r2) new_text[nlen++] = '\n';
        lvl += net; if (lvl < 0) lvl = 0;
    }

    /* Nothing actually changed -> don't dirty the file or push undo records. */
    if (nlen == old_len && memcmp(old_text, new_text, (size_t)nlen) == 0) {
        free(old_text); free(new_text); return;
    }

    int cur_r = JBE_PANE(s)->cur_row, cur_c = JBE_PANE(s)->cur_col;
    int er, ec;
    apply_stream_delete(s, r1, 0, old_text, old_len);
    undo_push(s, JBE_UNDO_DELETE, r1, 0, old_text, old_len, cur_r, cur_c, false);
    apply_stream_insert(s, r1, 0, new_text, nlen, &er, &ec);
    undo_push(s, JBE_UNDO_INSERT, r1, 0, new_text, nlen, r1, 0, false);

    /* Land the cursor at the first non-space of the first formatted line. */
    JBE_PANE(s)->sel_active = false;
    JBE_PANE(s)->cur_row = r1;
    { int c = 0; while (c < b->len[r1] && (b->lines[r1][c]==' ' || b->lines[r1][c]=='\t')) c++;
      JBE_PANE(s)->cur_col = c; }
    jbe_follow_cursor(s);
}

/* Drive the per-line colour fill from the active scheme. Returns false if
   no scheme is active (caller may skip the fg_out fill entirely). */
static bool colour_line(const jbe_syn_scheme_t *scheme,
                        const char *line, int len, uint8_t *fg_out,
                        const z80_sym_t *labels, int n_labels) {
    if (!scheme) return false;
    switch (scheme->flavor) {
        case JBE_SYN_FLAVOR_Z80:
            z80_colour_line(scheme, line, len, fg_out, labels, n_labels);
            return true;
        case JBE_SYN_FLAVOR_BASIC:
            (void)labels; (void)n_labels;               /* BASIC has no labels */
            basic_colour_line(scheme, line, len, fg_out);
            return true;
        case JBE_SYN_FLAVOR_NONE:
        default:
            return false;
    }
}

/* --- Rendering -------------------------------------------------------- */

static void fill_row(int row, uint8_t fg, uint8_t bg) {
    for (int c = 0; c < VGA_COLS; c++) vga_set_char(row, c, ' ', fg, bg);
}

static void render_menu_bar(const jbe_state_t *s) {
    fill_row(JBE_TITLE_ROW, JBE_BAR_FG, JBE_BAR_BG);
    int col = 2;
    for (int m = 0; m < JBE_MENU_COUNT; m++) {
        const char *t = JBE_MENU_TITLES[m];
        bool open = s->menu_active && s->menu_idx == m;
        uint8_t bg = open ? JBE_FG     : JBE_BAR_BG;   /* white tab on active */
        uint8_t fg = open ? JBE_BAR_FG : JBE_BAR_FG;   /* black text either way */
        /* Render with a leading + trailing space to make the highlight read
           as a "tab" cell, not just letters with reverse video. */
        vga_set_char(JBE_TITLE_ROW, col - 1, ' ', fg, bg);
        for (int i = 0; t[i]; i++) {
            uint8_t cfg = (i == 0) ? JBE_MENU_ACCEL_FG : fg;
            vga_set_char(JBE_TITLE_ROW, col + i, (uint8_t)t[i], cfg, bg);
        }
        vga_set_char(JBE_TITLE_ROW, col + (int)strlen(t), ' ', fg, bg);
        col += (int)strlen(t) + 3;
    }
}

/* Dropdown overlay for the active menu, drawn over the editor view. Single-
   line items, accelerator letter in red, current item reverse-video. */
static void render_dropdown(const jbe_state_t *s) {
    if (!s->menu_active) return;
    int n = menu_item_count(s, s->menu_idx);
    int w = menu_item_maxlen(s, s->menu_idx) + 4;       /* ' X item ' + border */
    int x = menu_title_col(s->menu_idx) - 1;
    int y = JBE_VIEW_TOP;                            /* just under the bar */
    if (x + w > VGA_COLS - 1) x = VGA_COLS - 1 - w;
    /* Cyan dropdown panel, one row of padding above & below the items. */
    for (int r = 0; r < n + 2; r++) {
        for (int c = 0; c < w; c++)
            vga_set_char(y + r, x + c, ' ', JBE_DROP_FG, JBE_DROP_BG);
    }
    for (int i = 0; i < n; i++) {
        const menu_item_t *it = &menu_items_for(s, s->menu_idx)[i];
        bool active = (i == s->item_idx);
        uint8_t bg = active ? JBE_DROP_HI_BG : JBE_DROP_BG;
        uint8_t fg = active ? JBE_DROP_HI_FG : JBE_DROP_FG;
        int row = y + 1 + i;
        for (int c = 0; c < w; c++)
            vga_set_char(row, x + c, ' ', fg, bg);
        if (menu_item_is_sep(it)) {                 /* draw a horizontal line */
            for (int c = 1; c < w - 1; c++)
                vga_set_char(row, x + c, 0xC4, fg, bg);   /* CP437 horizontal bar */
            continue;
        }
        /* Find the accelerator letter inside the label (first match,
           case-insensitive) so we colour the right cell — eXit, cuT, … */
        char up = it->accel;
        if (up >= 'a' && up <= 'z') up -= 32;
        int accel_pos = -1;
        for (int c = 0; it->label[c]; c++) {
            char lc = it->label[c];
            if (lc >= 'a' && lc <= 'z') lc -= 32;
            if (lc == up) { accel_pos = c; break; }
        }
        for (int c = 0; it->label[c] && c < w - 2; c++) {
            uint8_t cfg = (c == accel_pos) ? JBE_MENU_ACCEL_FG : fg;
            vga_set_char(row, x + 2 + c, (uint8_t)it->label[c], cfg, bg);
        }
        if (it->hint) {
            int hl = (int)strlen(it->hint);
            int hx = x + w - 2 - hl;
            for (int c = 0; c < hl; c++)
                vga_set_char(row, hx + c, (uint8_t)it->hint[c], fg, bg);
        }
    }
}

/* If c is a bracket, return its partner and set *dir to +1 (search forward for
   an opener) or -1 (search backward for a closer). Returns 0 if c is no bracket. */
static char bracket_partner(char c, int *dir) {
    switch (c) {
        case '(': *dir = +1; return ')';
        case '[': *dir = +1; return ']';
        case '{': *dir = +1; return '}';
        case ')': *dir = -1; return '(';
        case ']': *dir = -1; return '[';
        case '}': *dir = -1; return '{';
        default:  return 0;
    }
}

/* True if column col of this line sits inside a "..." string. Strings are
   assumed not to span lines (true for BASIC and Z80), so a left-to-right parity
   scan of the double-quotes before col is enough. (Z80 'x' char literals are not
   handled -- a deliberate simplification.) */
static bool col_in_string(const char *line, int len, int col) {
    bool in = false;
    for (int i = 0; i < col && i < len; i++) if (line[i] == '"') in = !in;
    return in;
}

/* Find the bracket matching the one at (row, col), honouring nesting of the
   same pair and skipping brackets inside strings. On success writes the partner
   position to the mrow / mcol out-params and returns true. Returns false when
   (row,col) is not a bracket, sits in a string, or has no partner. Scans the
   whole (small) buffer; stops at the first match. */
bool jbe_bracket_match(jbe_state_t *s, int row, int col,
                       int *mrow, int *mcol) {
    if (row < 0 || row >= JBE_BUF(s)->n_lines) return false;
    const char *L = JBE_BUF(s)->lines[row];
    int ln = JBE_BUF(s)->len[row];
    if (col < 0 || col >= ln) return false;
    int dir; char open = L[col]; char want = bracket_partner(open, &dir);
    if (!want || col_in_string(L, ln, col)) return false;

    /* Track string membership incrementally per line (instr = parity of the
       double-quotes before the current column) instead of rescanning [0,k) for
       every character. That keeps the whole scan linear in the buffer size; the
       earlier per-char col_in_string made it quadratic on a long line, which the
       64 KB single-line cap could turn into a real per-frame stall. */
    int depth = 1;
    if (dir > 0) {                                  /* forward: opener -> closer */
        for (int r = row; r < JBE_BUF(s)->n_lines; r++) {
            const char *ll = JBE_BUF(s)->lines[r];
            int n = JBE_BUF(s)->len[r];
            int k = (r == row) ? col + 1 : 0;
            bool instr = false;
            for (int i = 0; i < k && i < n; i++) if (ll[i] == '"') instr = !instr;
            for (; k < n; k++) {
                char ch = ll[k];
                if (ch == '"') { instr = !instr; continue; }
                if (!instr) {
                    if      (ch == open) depth++;
                    else if (ch == want && --depth == 0) { *mrow = r; *mcol = k; return true; }
                }
            }
        }
    } else {                                        /* backward: closer -> opener */
        for (int r = row; r >= 0; r--) {
            const char *ll = JBE_BUF(s)->lines[r];
            int n = JBE_BUF(s)->len[r];
            int k = (r == row) ? col - 1 : n - 1;
            bool instr = false;                     /* parity of quotes in [0,k) */
            for (int i = 0; i < k && i < n; i++) if (ll[i] == '"') instr = !instr;
            for (; k >= 0; k--) {
                char ch = ll[k];
                if (ch != '"' && !instr) {
                    if      (ch == open) depth++;
                    else if (ch == want && --depth == 0) { *mrow = r; *mcol = k; return true; }
                }
                if (k - 1 >= 0 && ll[k - 1] == '"') instr = !instr;  /* step parity to k-1 */
            }
        }
    }
    return false;
}

/* Draw one pane (text + wrap markers + scrollbar + optional cursor) into
   the screen-row range [top_scr_row, bot_scr_row]. The pane and its buffer
   are reached via the standard JBE_PANE/JBE_BUF macros after temporarily
   pointing active_pane at this pane, which keeps every existing helper
   (in_selection, visual_rows_range, z80_collect_labels, …) untouched.
   The active_pane is restored on exit. */
static void render_pane(jbe_state_t *s, int pane_idx,
                        int top_scr_row, int bot_scr_row,
                        bool draw_cursor) {
    int saved_active = s->active_pane;
    s->active_pane = pane_idx;
    int view_height = bot_scr_row - top_scr_row + 1;

    /* Resolve the active scheme once per render. Built-in only for now;
       stap 2c will let a floppy `.syn` file override this lookup. */
    const jbe_syn_scheme_t *scheme = JBE_BUF(s)->active_scheme;

    /* Build the label set per pane: each pane shows a buffer, possibly with
       its own syntax. Discarded at the end of this function. */
    z80_sym_t *labels = 0;
    int n_labels = 0;
    if (scheme && scheme->flavor == JBE_SYN_FLAVOR_Z80)
        n_labels = z80_collect_labels(s, scheme, &labels);

    /* Bracket matching (only for the pane that owns the cursor). The "active"
       bracket is the one under the cursor, or failing that the one just left of
       it (so it lights up right after you type a closing bracket). If it has a
       partner, both ends get the grey chip; if not, the active bracket is shown
       red. brow/bcol mark the active bracket, mrow/mcol its partner. */
    int brow = -1, bcol = -1, mrow = -1, mcol = -1;
    bool br_matched = false;
    if (draw_cursor) {
        int cr = JBE_PANE(s)->cur_row, cc = JBE_PANE(s)->cur_col;
        const char *cl = JBE_BUF(s)->lines[cr];
        int cn = JBE_BUF(s)->len[cr];
        int dir;
        if (cc < cn && bracket_partner(cl[cc], &dir) && !col_in_string(cl, cn, cc)) {
            brow = cr; bcol = cc;
        } else if (cc - 1 >= 0 && cc - 1 < cn && bracket_partner(cl[cc - 1], &dir)
                   && !col_in_string(cl, cn, cc - 1)) {
            brow = cr; bcol = cc - 1;
        }
        if (brow >= 0) br_matched = jbe_bracket_match(s, brow, bcol, &mrow, &mcol);
    }

    /* Visible lines, with char-wrap: a logical line of length L renders into
       ceil(L / JBE_WRAP_WIDTH) sub-rows. On every sub-row except the last of
       its line, a JBE_WRAP_GLYPH continuation-marker sits in col JBE_WRAP_WIDTH
       so the eye can tell which screen rows belong to the same logical line. */
    int screen_row  = top_scr_row;
    int file_row    = JBE_PANE(s)->top_row;
    int last_drawn  = file_row;        /* tracked for scrollbar can_down */
    while (screen_row <= bot_scr_row) {
        fill_row(screen_row, JBE_FG, JBE_BG);
        if (file_row >= JBE_BUF(s)->n_lines) { screen_row++; continue; }
        const char *line = JBE_BUF(s)->lines[file_row];
        int len   = JBE_BUF(s)->len[file_row];
        int subs  = visual_rows_of(len);
        uint8_t *line_fg = (scheme && len > 0) ? malloc(len) : 0;
        bool has_colour = line_fg &&
            colour_line(scheme, line, len, line_fg, labels, n_labels);
        for (int sub = 0; sub < subs && screen_row <= bot_scr_row; sub++) {
            if (sub > 0) fill_row(screen_row, JBE_FG, JBE_BG);
            int from = sub * JBE_WRAP_WIDTH;
            int upto = from + JBE_WRAP_WIDTH;
            if (upto > len) upto = len;
            for (int c = from; c < upto; c++) {
                unsigned char ch = (unsigned char)line[c];
                if (ch < 32 || ch == 127) ch = '.';
                bool sel = in_selection(s, file_row, c);
                uint8_t fg = has_colour ? line_fg[c] : JBE_FG;
                uint8_t cell_fg = sel ? JBE_BG : fg;
                uint8_t cell_bg = sel ? fg     : JBE_BG;
                if (!sel && brow >= 0) {
                    bool is_active = (file_row == brow && c == bcol);
                    bool is_match  = (file_row == mrow && c == mcol);
                    if (br_matched && (is_active || is_match)) {
                        cell_fg = JBE_MATCH_FG;   cell_bg = JBE_MATCH_BG;
                    } else if (!br_matched && is_active) {
                        cell_fg = JBE_UNMATCH_FG; cell_bg = JBE_UNMATCH_BG;
                    }
                }
                vga_set_char(screen_row, c - from, ch, cell_fg, cell_bg);
            }
            if (sub + 1 < subs) {           /* not the last sub-row: show marker */
                vga_set_char(screen_row, JBE_WRAP_WIDTH, JBE_WRAP_GLYPH,
                             VGA_RED, JBE_BG);
            }
            screen_row++;
        }
        free(line_fg);
        last_drawn = file_row;
        file_row++;
    }

    /* Vertical scrollbar (QuickBASIC-style) in the reserved last column.
       Sized against this pane's visible height rather than the whole view. */
    {
        int  total       = JBE_BUF(s)->n_lines > 0 ? JBE_BUF(s)->n_lines : 1;
        bool can_up      = JBE_PANE(s)->top_row > 0;
        bool can_down    = last_drawn + 1 < JBE_BUF(s)->n_lines;
        int  track_h     = view_height - 2;
        if (track_h < 1) track_h = 1;
        int  thumb_size  = (track_h * view_height) / total;
        if (thumb_size < 1)        thumb_size = 1;
        if (thumb_size > track_h)  thumb_size = track_h;
        int  thumb_top   = (track_h * JBE_PANE(s)->top_row) / total;
        if (thumb_top + thumb_size > track_h)
            thumb_top = track_h - thumb_size;

        for (int r = 0; r < view_height; r++) {
            int sr = top_scr_row + r;
            uint8_t code = '|', fg = VGA_WHITE, bg = VGA_BLACK;
            if (r == 0) {
                code = can_up ? '^' : ' ';
            } else if (r == view_height - 1) {
                code = can_down ? 'v' : ' ';
            } else {
                int t = r - 1;
                if (t >= thumb_top && t < thumb_top + thumb_size) {
                    code = ' '; fg = VGA_BLACK; bg = VGA_WHITE;
                }
            }
            vga_set_char(sr, JBE_SCROLLBAR_COL, code, fg, bg);
        }
    }

    /* Cursor: reverse-video block at the visual position. Inactive panes
       skip this so only one cursor is visible at a time. */
    if (draw_cursor) {
        int cur_sub, cur_vcol;
        cursor_visual_pos(JBE_PANE(s)->cur_col, &cur_sub, &cur_vcol);
        int rows_before = visual_rows_range(s, JBE_PANE(s)->top_row, JBE_PANE(s)->cur_row);
        int cr = rows_before + cur_sub;
        if (cr >= 0 && cr < view_height &&
            cur_vcol >= 0 && cur_vcol < JBE_VIEW_WIDTH) {
            int sr = top_scr_row + cr;
            vga_char_t cell = vga_text_buffer[sr][cur_vcol];
            /* When the cursor sits on an unbalanced bracket, draw the block red
               instead of the usual reverse video so the error is visible even
               under the cursor. */
            bool on_unmatched = (!br_matched && brow == JBE_PANE(s)->cur_row
                                 && bcol == JBE_PANE(s)->cur_col);
            vga_set_char(sr, cur_vcol, cell.code ? cell.code : ' ',
                         on_unmatched ? JBE_UNMATCH_FG : JBE_BG,
                         on_unmatched ? JBE_UNMATCH_BG : JBE_FG);
        }
    }

    free(labels);
    s->active_pane = saved_active;
}

/* Horizontal divider drawn between two panes when the split is active. */
static void render_divider(int row) {
    for (int c = 0; c < VGA_COLS; c++)
        vga_set_char(row, c, '-', JBE_BAR_FG, JBE_BAR_BG);
}

/* Write `t` at (row,col), clipped at maxcol so a long input can never write past
   the box (or off the screen edge). */
static void box_text(int row, int col, const char *t, int maxcol,
                     uint8_t fg, uint8_t bg) {
    for (int i = 0; t[i] && col + i <= maxcol; i++)
        vga_set_char(row, col + i, (uint8_t)t[i], fg, bg);
}

/* The Find / Replace prompt as a framed, black-on-yellow box anchored just above
   the status bar. Find is 3 rows (one input line); Replace is 4 rows (the search
   text, then "with <replacement>"). The input caret blinks via s->caret_on. The
   status bar underneath keeps its normal Ln/Col/file readout -- the box is what
   makes the search mode obvious, so the bar no longer has to be hijacked. */
/* Draw a framed, black-on-yellow prompt box: full width, `rows` tall, with its
   bottom border on the row just above the status bar. `title` is written into
   the top border (it doubles as a key legend). Returns the first content row. */
static int draw_prompt_box(int rows, const char *title) {
    const uint8_t FG = JBE_PROMPT_FG, BG = JBE_PROMPT_BG;   /* black on yellow */
    /* CP437 single-line box-drawing glyphs (present in font_8x12). */
    const uint8_t TL = 218, TR = 191, BL = 192, BR = 217, HZ = 196, VT = 179;
    int rgt = VGA_COLS - 1, inr = rgt - 1;
    int top = JBE_STATUS_ROW - rows, bot = JBE_STATUS_ROW - 1;
    vga_set_char(top, 0, TL, FG, BG); vga_set_char(top, rgt, TR, FG, BG);
    vga_set_char(bot, 0, BL, FG, BG); vga_set_char(bot, rgt, BR, FG, BG);
    for (int c = 1; c < rgt; c++) { vga_set_char(top, c, HZ, FG, BG);
                                    vga_set_char(bot, c, HZ, FG, BG); }
    for (int r = top + 1; r < bot; r++) {
        vga_set_char(r, 0, VT, FG, BG);
        for (int c = 1; c < rgt; c++) vga_set_char(r, c, ' ', FG, BG);
        vga_set_char(r, rgt, VT, FG, BG);
    }
    box_text(top, 2, title, inr, FG, BG);
    return top + 1;
}

static void render_search_box(jbe_state_t *s) {
    jbe_pane_t *p    = JBE_PANE(s);
    const uint8_t FG = JBE_PROMPT_FG, BG = JBE_PROMPT_BG;   /* black on yellow */
    const int inr = VGA_COLS - 2;          /* last usable interior column */
    const int x   = 2;                     /* left text inset             */
    bool repl = p->replace_active;
    int row1 = draw_prompt_box(repl ? 4 : 3, repl
        ? " Replace   Enter to replace, Ctrl+N/Ctrl+P to move, Ctrl+A for all, Tab to switch field and Esc to close "
        : " Find   Use Enter/Ctrl+N to find next, Ctrl+P to find previous and Esc to close ");

    if (!repl) {                            /* --- Find --- */
        box_text(row1, x, p->find_query, inr, FG, BG);
        int caret = x + p->find_query_len;
        if (s->caret_on && caret <= inr) vga_set_char(row1, caret, '_', FG, BG);
        if (p->find_query_len > 0 && p->find_match_row < 0)
            box_text(row1, inr - 10, "<not found>", inr, FG, BG);
        return;
    }

    /* --- Replace: row1 = search text, row2 = "with <replacement>".
           replace_phase is the focused field (0 = search, 1 = "with"). --- */
    int row2 = row1 + 1;
    box_text(row1, x, p->find_query, inr, FG, BG);
    char with[160];
    snprintf(with, sizeof with, "with %s", p->replace_with);
    box_text(row2, x, with, inr, FG, BG);
    /* blinking caret in whichever field has focus */
    if (p->replace_phase == 0) {
        int caret = x + p->find_query_len;
        if (s->caret_on && caret <= inr) vga_set_char(row1, caret, '_', FG, BG);
    } else {
        int caret = x + 5 + p->replace_with_len;       /* "with " is 5 chars */
        if (s->caret_on && caret <= inr) vga_set_char(row2, caret, '_', FG, BG);
    }
    if (p->find_query_len > 0 && p->find_match_row < 0)
        box_text(row1, inr - 10, "<not found>", inr, FG, BG);
}

/* File→Save As prompt: a framed box with the path being typed + a blinking caret. */
static void render_saveas_box(jbe_state_t *s) {
    const uint8_t FG = JBE_PROMPT_FG, BG = JBE_PROMPT_BG;
    const int inr = VGA_COLS - 2;
    int row1 = draw_prompt_box(3, " Save As   Enter to save, Esc to cancel ");
    box_text(row1, 2, s->save_as_name, inr, FG, BG);
    int caret = 2 + s->save_as_len;
    if (s->caret_on && caret <= inr) vga_set_char(row1, caret, '_', FG, BG);
}

/* Search→Go to Line prompt: a framed box with the line number being typed. */
static void render_goto_box(jbe_state_t *s) {
    const uint8_t FG = JBE_PROMPT_FG, BG = JBE_PROMPT_BG;
    const int inr = VGA_COLS - 2;
    int row1 = draw_prompt_box(3, " Go to Line   Enter to jump, Esc to cancel ");
    box_text(row1, 2, s->goto_buf, inr, FG, BG);
    int caret = 2 + s->goto_len;
    if (s->caret_on && caret <= inr) vga_set_char(row1, caret, '_', FG, BG);
}

/* Shared "unsaved changes" confirmation for Close / un-split / New / Open. */
static void render_confirm_box(jbe_state_t *s) {
    const uint8_t FG = JBE_PROMPT_FG, BG = JBE_PROMPT_BG;
    const int inr = VGA_COLS - 2;
    int row1 = draw_prompt_box(4, " Unsaved changes ");
    int row2 = row1 + 1;
    /* The document at risk: for an un-split it is the OTHER pane's file (the one
       being dropped); otherwise the active one. */
    const char *who = (s->confirm_action == JBE_CONFIRM_UNSPLIT)
        ? s->buffers[s->panes[s->active_pane ^ 1].buf_idx].filename
        : JBE_BUF(s)->filename;
    char line[160];
    snprintf(line, sizeof line, "\"%s\" has unsaved changes.", who);
    box_text(row1, 2, line, inr, FG, BG);
    box_text(row2, 2, "Y to discard and continue, any other key to cancel", inr, FG, BG);
}

/* ---- Japi Commander: a two-pane file manager in a floating window --------- */
/* Centred window (like the F1 help) so the editor stays visible around it.
   Layout inside: top border + a cwd header per pane + the two lists + a
   two-line key legend + bottom border. A vertical divider splits the panes. */
#define JFC_W       108
#define JFC_H        52
#define JFC_LEFT    ((VGA_COLS - JFC_W) / 2)        /* 9  */
#define JFC_TOP     ((VGA_ROWS - JFC_H) / 2)        /* 6  */
#define JFC_RIGHT   (JFC_LEFT + JFC_W - 1)          /* 116 */
#define JFC_BOTTOM  (JFC_TOP + JFC_H - 1)           /* 57 */
#define JFC_DIV     (JFC_LEFT + JFC_W / 2)          /* 63: divider column */
#define JFC_HDR_ROW (JFC_TOP + 1)                   /* pane cwd headers     */
#define JFC_LIST_ROW (JFC_TOP + 2)
#define JFC_LIST_H  (JFC_H - 6)                      /* border+hdr+msg+2 legend+border */
#define JFC_MSG_ROW  (JFC_BOTTOM - 3)                /* prompt / confirm / result    */
#define JFC_LEG1_ROW (JFC_BOTTOM - 2)                /* legend: keys row             */
#define JFC_LEG2_ROW (JFC_BOTTOM - 1)                /* legend: action labels row    */
#define JFC_P0_COL  (JFC_LEFT + 2)
#define JFC_P0_W    (JFC_DIV - JFC_P0_COL - 1)
#define JFC_P1_COL  (JFC_DIV + 2)
#define JFC_P1_W    (JFC_RIGHT - 1 - JFC_P1_COL)

/* Join cwd + name with the platform's path rule (no separator after ':' or '/'). */
static void cmd_join(char *out, int out_max, const char *cwd, const char *name) {
    int n = (int)strlen(cwd);
    bool sep = (n > 0 && cwd[n-1] != ':' && cwd[n-1] != '/');
    snprintf(out, (size_t)out_max, "%s%s%s", cwd, sep ? "/" : "", name);
}

static void commander_open(jbe_state_t *s) {
    s->commander_active         = true;
    s->commander_pane           = 0;
    s->commander_msg[0]         = 0;
    s->commander_input_active   = false;
    s->commander_confirm_delete = false;
    s->commander_input_len      = 0;
    s->commander_input_cur      = 0;
    s->commander_input[0]       = 0;
    s->clip_n                   = 0;
    for (int p = 0; p < 2; p++) {
        ui_filelist_t *w = &s->commander_list[p];
        ui_filelist_default_colors(w);
        w->fg = VGA_WHITE; w->bg = VGA_BLUE; w->dir_fg = VGA_YELLOW;
        w->sel_fg = VGA_BLACK; w->sel_bg = VGA_CYAN;
        ui_filelist_open(w, p == 0 ? "A:" : "C:", JFC_LIST_ROW,
                         p == 0 ? JFC_P0_COL : JFC_P1_COL, JFC_LIST_H,
                         p == 0 ? JFC_P0_W : JFC_P1_W);
    }
}

/* Stream-copy one file in chunks so it is binary-safe (a .kbd is full of NUL
   bytes). Returns bytes copied, or -1 on error.
   static buffers: a japi_file_t embeds a ~550-byte FatFs FIL; two on the ~2 KB
   Core 0 stack would overflow it, so the Commander copies one file at a time. */
static long cmd_copy_file(const char *spath, const char *dpath) {
    static japi_file_t fin, fout;
    static uint8_t     buf[512];
    if (!japi_fopen(&fin, spath, JAPI_READ))  return -1;
    if (!japi_fopen(&fout, dpath, JAPI_WRITE)) { japi_fclose(&fin); return -1; }
    long total = 0; bool ok = true; int n;
    while ((n = japi_fread(&fin, buf, (int)sizeof buf)) > 0) {
        if (japi_fwrite(&fout, buf, n) != n) { ok = false; break; }
        total += n;
    }
    japi_fclose(&fin);
    japi_fclose(&fout);
    return ok ? total : -1;
}

/* True if `path` is a directory (probe by trying to open it as one). */
static bool cmd_is_dir(const char *path) {
    japi_dir_t probe;
    if (!japi_opendir(&probe, path)) return false;
    japi_closedir(&probe);
    return true;
}

/* Recursive directory copy / delete for the Commander.
   Depth is capped at CMD_MAX_DEPTH and the path at UI_FILELIST_PATH_MAX, which
   keeps two things bounded: (1) the core-0 stack (one japi_dir_t ~60 B + a couple
   of path buffers per level), and (2) FatFs's open-object table (FF_FS_LOCK = 16):
   the open-directory chain is one handle per level, plus at most two transient
   file handles during a copy, so ~depth+2 -- well under 16 at depth 10. Trees
   deeper than the cap (or a path longer than the limit) make that branch fail
   gracefully rather than crash/hang. */
#define CMD_MAX_DEPTH 10
#define CMD_TREE_PATH 256          /* child-path buffer; longer paths are refused */

/* True if joining dir+name fits in a CMD_TREE_PATH buffer (no silent truncation). */
static bool cmd_path_fits(const char *dir, const char *name) {
    return (int)strlen(dir) + 1 + (int)strlen(name) < CMD_TREE_PATH;
}

static bool cmd_copy_tree(const char *src, const char *dst, int depth) {
    if (depth > CMD_MAX_DEPTH) return false;
    japi_mkdir(dst);                       /* create the destination dir (ok if it exists) */
    japi_dir_t d;
    if (!japi_opendir(&d, src)) return false;
    char name[UI_FILELIST_NAME_MAX + 1];
    bool ok = true;
    while (japi_readdir(&d, name, (int)sizeof name)) {
        if (!cmd_path_fits(src, name) || !cmd_path_fits(dst, name)) { ok = false; continue; }
        char cs[CMD_TREE_PATH], cd[CMD_TREE_PATH];
        cmd_join(cs, sizeof cs, src, name);
        cmd_join(cd, sizeof cd, dst, name);
        if (cmd_is_dir(cs)) { if (!cmd_copy_tree(cs, cd, depth + 1)) ok = false; }
        else                { if (cmd_copy_file(cs, cd) < 0)        ok = false; }
    }
    japi_closedir(&d);
    return ok;
}

static bool cmd_remove_tree(const char *path, int depth) {
    if (depth > CMD_MAX_DEPTH) return false;
    japi_dir_t d;
    if (japi_opendir(&d, path)) {
        char name[UI_FILELIST_NAME_MAX + 1];
        while (japi_readdir(&d, name, (int)sizeof name)) {
            if (!cmd_path_fits(path, name)) continue;
            char child[CMD_TREE_PATH];
            cmd_join(child, sizeof child, path, name);
            if (cmd_is_dir(child)) cmd_remove_tree(child, depth + 1);
            else                   japi_remove(child);
        }
        japi_closedir(&d);
    }
    return japi_rmdir(path);                /* drop the now-empty directory */
}

/* True if `child` is the same path as `parent` or lies inside its subtree --
   used to refuse pasting/moving a folder into itself or its own descendant. */
static bool cmd_path_within(const char *parent, const char *child) {
    int n = (int)strlen(parent);
    if (strncmp(parent, child, (size_t)n) != 0) return false;
    return child[n] == '\0' || child[n] == '/' || child[n] == ':';
}

/* Reload a pane in place, keeping the selection on a valid row. */
static void cmd_reload(ui_filelist_t *w) {
    int keep = w->sel;
    ui_filelist_open(w, w->cwd, w->view_row, w->view_col, w->view_h, w->view_w);
    if (keep < w->n_entries)      w->sel = keep;
    else if (w->n_entries > 0)    w->sel = w->n_entries - 1;
}

/* The selected entry in the active pane, or NULL; refuses only ".." (a file or a
   folder can both be renamed now that the platform has japi_rename). */
static const ui_filelist_entry_t *cmd_pick_file(jbe_state_t *s, const char *verb) {
    ui_filelist_t *a = &s->commander_list[s->commander_pane];
    if (a->n_entries == 0) return NULL;
    const ui_filelist_entry_t *e = &a->entries[a->sel];
    if (strcmp(e->name, "..") == 0) {
        snprintf(s->commander_msg, sizeof s->commander_msg, "Cannot %s \"..\"", verb);
        return NULL;
    }
    return e;
}

/* The files to act on: every tagged file, or the current one if none are
   tagged. ".." is always skipped. Returns the count. Pass names == NULL to
   count only -- this avoids forcing the caller to reserve a names buffer
   (2 KB) on the RP2350 core-0 stack just to find out how many files match.

   with_dirs lets the single-current-entry case also pick a folder. Folders can
   now be tagged (commander_tag allows them), and copy/cut/delete all handle a
   folder via the recursive helpers, so the tagged loop collects folders too;
   the per-item dispatch (file vs folder) happens where the work is done. */
static int commander_collect(jbe_state_t *s,
                             char names[][UI_FILELIST_NAME_MAX + 1], int max,
                             bool with_dirs) {
    ui_filelist_t *a = &s->commander_list[s->commander_pane];
    int n = 0, any = 0;
    for (int i = 0; i < a->n_entries; i++) if (a->entries[i].tagged) any = 1;
    if (any) {
        for (int i = 0; i < a->n_entries && n < max; i++)
            if (a->entries[i].tagged && strcmp(a->entries[i].name, "..") != 0) {
                if (names) snprintf(names[n], UI_FILELIST_NAME_MAX + 1, "%s", a->entries[i].name);
                n++;
            }
    } else if (a->n_entries > 0
               && strcmp(a->entries[a->sel].name, "..") != 0
               && (with_dirs || !a->entries[a->sel].is_dir)) {
        if (names) snprintf(names[n], UI_FILELIST_NAME_MAX + 1, "%s", a->entries[a->sel].name);
        n++;
    }
    return n;
}

/* Ctrl+C / Ctrl+X: snapshot the tagged-or-current files onto the clipboard. */
static void commander_clip(jbe_state_t *s, bool cut) {
    ui_filelist_t *a = &s->commander_list[s->commander_pane];
    s->clip_n = commander_collect(s, s->clip_names, JBE_CLIP_MAX, true);
    if (s->clip_n == 0) {
        snprintf(s->commander_msg, sizeof s->commander_msg,
                 "Nothing to %s (pick a file or folder)", cut ? "cut" : "copy");
        return;
    }
    snprintf(s->clip_src, sizeof s->clip_src, "%s", a->cwd);
    s->clip_cut = cut;
    snprintf(s->commander_msg, sizeof s->commander_msg,
             "%s %d item%s -- switch pane and Paste (Ctrl+V)",
             cut ? "Cut" : "Copied", s->clip_n, s->clip_n == 1 ? "" : "s");
}

/* Do one clipboard item i into pane dst. dest_exists means a same-named target is
   present and the user approved overwriting it (clean replace first). Files and
   folders both handled; a same-drive move is an atomic rename, a cross-drive move
   is copy-then-delete. Returns true on success. */
static bool cmd_paste_one(jbe_state_t *s, ui_filelist_t *dst, int i, bool dest_exists) {
    char sp[CMD_TREE_PATH], dp[CMD_TREE_PATH];
    cmd_join(sp, sizeof sp, s->clip_src, s->clip_names[i]);
    cmd_join(dp, sizeof dp, dst->cwd, s->clip_names[i]);
    bool srcdir = cmd_is_dir(sp);
    if (dest_exists) {                          /* approved overwrite = clean replace */
        if (cmd_is_dir(dp)) cmd_remove_tree(dp, 0); else japi_remove(dp);
    }
    bool same_drive = ((s->clip_src[0] | 0x20) == (dst->cwd[0] | 0x20));   /* 'a'/'c' */
    if (s->clip_cut) {
        if (same_drive) return japi_rename(sp, dp);   /* atomic, also a non-empty folder */
        bool ok = srcdir ? cmd_copy_tree(sp, dp, 0) : (cmd_copy_file(sp, dp) >= 0);
        if (ok) { if (srcdir) cmd_remove_tree(sp, 0); else japi_remove(sp); }
        return ok;
    }
    return srcdir ? cmd_copy_tree(sp, dp, 0) : (cmd_copy_file(sp, dp) >= 0);
}

/* Ctrl+V: copy (or move, if cut) the clipboard into the active pane. Resumable:
   on a name clash it sets commander_confirm_overwrite and returns; the key
   handler answers Y/N/A and calls back in to continue from commander_paste_idx. */
static void commander_paste(jbe_state_t *s) {
    if (s->clip_n == 0) {
        snprintf(s->commander_msg, sizeof s->commander_msg, "Clipboard is empty"); return;
    }
    ui_filelist_t *dst = &s->commander_list[s->commander_pane];
    while (s->commander_paste_idx < s->clip_n) {
        int i = s->commander_paste_idx;
        char sp[CMD_TREE_PATH], dp[CMD_TREE_PATH];
        cmd_join(sp, sizeof sp, s->clip_src, s->clip_names[i]);
        cmd_join(dp, sizeof dp, dst->cwd, s->clip_names[i]);
        /* refuse the impossible: same location, or a folder into itself/its subtree */
        if (strcmp(sp, dp) == 0 || (cmd_is_dir(sp) && cmd_path_within(sp, dp))) {
            s->commander_paste_idx++; continue;
        }
        bool exists = japi_exists(dp);
        if (exists && !s->commander_overwrite_all) {
            s->commander_confirm_overwrite = true;   /* pause for a Y/N/A answer */
            return;
        }
        if (cmd_paste_one(s, dst, i, exists)) s->commander_paste_done++;
        s->commander_paste_idx++;
    }
    bool cut = s->clip_cut; int done = s->commander_paste_done;
    cmd_reload(&s->commander_list[0]);
    cmd_reload(&s->commander_list[1]);
    snprintf(s->commander_msg, sizeof s->commander_msg,
             "%s %d item%s", cut ? "Moved" : "Pasted", done, done == 1 ? "" : "s");
    if (cut) s->clip_n = 0;                          /* a cut clipboard is consumed */
    s->commander_paste_idx = 0; s->commander_paste_done = 0; s->commander_overwrite_all = false;
}

/* Delete (confirmed): remove the tagged-or-current files. */
static void commander_delete(jbe_state_t *s) {
    /* Static, not on the stack: this 2 KB array would overflow the RP2350
       core-0 stack (~2 KB) and hang the machine. The Commander is modal and
       runs only on core 0, so a single shared buffer is safe. */
    static char names[JBE_CLIP_MAX][UI_FILELIST_NAME_MAX + 1];
    int n = commander_collect(s, names, JBE_CLIP_MAX, true);
    ui_filelist_t *a = &s->commander_list[s->commander_pane];
    int ok = 0;
    for (int i = 0; i < n; i++) {
        char path[200]; cmd_join(path, sizeof path, a->cwd, names[i]);
        /* A folder (even a non-empty one) is removed recursively; a file plainly. */
        bool done = cmd_is_dir(path) ? cmd_remove_tree(path, 0) : japi_remove(path);
        if (done) ok++;
    }
    cmd_reload(a);
    if (ok == n)
        snprintf(s->commander_msg, sizeof s->commander_msg,
                 "Deleted %d item%s", ok, ok == 1 ? "" : "s");
    else
        snprintf(s->commander_msg, sizeof s->commander_msg,
                 "Deleted %d of %d (some entries could not be removed)", ok, n);
}

/* Space: toggle the tag on the current entry, then step down. Shift+Up/Down
   extend a contiguous tagged run. Files and folders can both be tagged; only
   ".." is never tagged (copy/cut/delete all handle a folder recursively). */
static void commander_tag(ui_filelist_t *a, int idx) {
    if (idx >= 0 && idx < a->n_entries
        && strcmp(a->entries[idx].name, "..") != 0)
        a->entries[idx].tagged = !a->entries[idx].tagged;
}
static void commander_tag_set(ui_filelist_t *a, int idx) {
    if (idx >= 0 && idx < a->n_entries
        && strcmp(a->entries[idx].name, "..") != 0)
        a->entries[idx].tagged = true;
}

/* Ctrl+N (after the name prompt): create a folder in the active pane. */
static void commander_do_mkdir(jbe_state_t *s, const char *name) {
    ui_filelist_t *a = &s->commander_list[s->commander_pane];
    char path[200]; cmd_join(path, sizeof path, a->cwd, name);
    bool ok = japi_mkdir(path);
    cmd_reload(a);
    snprintf(s->commander_msg, sizeof s->commander_msg,
             ok ? "Created folder %s" : "Could not create %s", name);
}

/* ASCII case-insensitive name compare (dependency-free, no strcasecmp). Used to
   spot a rename that only changes letter case, which needs special handling on
   a case-insensitive volume. */
static bool names_equal_ci(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return false;
    }
    return *a == *b;            /* equal only if both ended together */
}

/* Ctrl+R (after the name prompt): rename the selected file or folder. Uses the
   platform's atomic japi_rename, which works on a non-empty folder too. */
static void commander_do_rename(jbe_state_t *s, const char *newname) {
    const ui_filelist_entry_t *e = cmd_pick_file(s, "rename");
    if (!e) return;
    ui_filelist_t *a = &s->commander_list[s->commander_pane];
    char oldname[UI_FILELIST_NAME_MAX + 1]; snprintf(oldname, sizeof oldname, "%s", e->name);

    if (strcmp(oldname, newname) == 0) {        /* nothing actually changed */
        snprintf(s->commander_msg, sizeof s->commander_msg, "Name unchanged");
        return;
    }

    char spath[200], dpath[200];
    cmd_join(spath, sizeof spath, a->cwd, oldname);
    cmd_join(dpath, sizeof dpath, a->cwd, newname);

    bool ok;
    if (names_equal_ci(oldname, newname)) {
        /* Case-only rename (e.g. file.txt -> FILE.TXT). On a case-insensitive
           volume (the SD card / FAT) the old and new names are one and the same
           entry, so rename via a temporary name: old -> tmp -> new. A rename (not
           a byte copy), so it works for a folder too. */
        char tpath[200];
        cmd_join(tpath, sizeof tpath, a->cwd, ".jbe_rename.tmp");
        japi_remove(tpath);                     /* clear any stale temp from a prior crash */
        ok = japi_rename(spath, tpath) && japi_rename(tpath, dpath);
    } else {
        if (japi_exists(dpath)) {
            snprintf(s->commander_msg, sizeof s->commander_msg, "%s already exists", newname);
            return;
        }
        ok = japi_rename(spath, dpath);
    }
    cmd_reload(a);
    if (ok) snprintf(s->commander_msg, sizeof s->commander_msg, "Renamed to %s", newname);
    else    snprintf(s->commander_msg, sizeof s->commander_msg, "Rename of %s failed", oldname);
}

/* Open the modal name prompt (kind 0 = mkdir, 1 = rename, prefilled). */
static void commander_prompt(jbe_state_t *s, int kind, const char *prefill) {
    s->commander_input_active = true;
    s->commander_input_kind   = kind;
    s->commander_msg[0]       = 0;
    snprintf(s->commander_input, sizeof s->commander_input, "%s", prefill ? prefill : "");
    s->commander_input_len    = (int)strlen(s->commander_input);
    s->commander_input_cur    = s->commander_input_len;   /* caret starts at the end */
}

static void commander_handle_key(jbe_state_t *s, uint16_t k) {
    /* Modal name prompt for mkdir / rename. The field edits like a one-line
       text box: the caret (commander_input_cur) moves with the arrows / Home /
       End, typing inserts at the caret, and Backspace / Delete remove the
       character before / at it. */
    if (s->commander_input_active) {
        int  *cur = &s->commander_input_cur;
        int  *len = &s->commander_input_len;
        char *buf = s->commander_input;
        if (k == JAPI_KEY_ESCAPE) { s->commander_input_active = false; return; }
        if (k == JAPI_KEY_ENTER) {
            s->commander_input_active = false;
            if (*len > 0) {
                if (s->commander_input_kind == 0) commander_do_mkdir(s, buf);
                else                              commander_do_rename(s, buf);
            }
            return;
        }
        if (k == JAPI_KEY_LEFT)  { if (*cur > 0)    (*cur)--;   return; }
        if (k == JAPI_KEY_RIGHT) { if (*cur < *len) (*cur)++;   return; }
        if (k == JAPI_KEY_HOME)  { *cur = 0;                    return; }
        if (k == JAPI_KEY_END)   { *cur = *len;                 return; }
        if (k == JAPI_KEY_BACKSPACE) {
            if (*cur > 0) {                       /* drop the char left of the caret */
                memmove(buf + *cur - 1, buf + *cur, (size_t)(*len - *cur) + 1);
                (*cur)--; (*len)--;
            }
            return;
        }
        if (k == JAPI_KEY_DELETE) {
            if (*cur < *len) {                    /* drop the char at the caret */
                memmove(buf + *cur, buf + *cur + 1, (size_t)(*len - *cur));
                (*len)--;
            }
            return;
        }
        if (k >= 32 && k < 127 && *len < (int)sizeof s->commander_input - 1) {
            memmove(buf + *cur + 1, buf + *cur, (size_t)(*len - *cur) + 1);
            buf[*cur] = (char)k;                  /* insert at the caret */
            (*cur)++; (*len)++;
        }
        return;
    }
    /* Delete confirmation: Y deletes, anything else cancels. */
    if (s->commander_confirm_delete) {
        s->commander_confirm_delete = false;
        if (k == 'y' || k == 'Y') commander_delete(s);
        return;
    }

    /* Overwrite confirmation during a paste: Y this one, N skip it, A all the
       rest, anything else cancels the remainder. Then resume the paste. */
    if (s->commander_confirm_overwrite) {
        s->commander_confirm_overwrite = false;
        ui_filelist_t *dst = &s->commander_list[s->commander_pane];
        if (k == 'y' || k == 'Y') {
            if (cmd_paste_one(s, dst, s->commander_paste_idx, true)) s->commander_paste_done++;
            s->commander_paste_idx++;
            commander_paste(s);
        } else if (k == 'a' || k == 'A') {
            s->commander_overwrite_all = true;
            commander_paste(s);
        } else if (k == 'n' || k == 'N') {
            s->commander_paste_idx++;
            commander_paste(s);
        } else {                                /* Esc / anything else: stop here */
            cmd_reload(&s->commander_list[0]);
            cmd_reload(&s->commander_list[1]);
            snprintf(s->commander_msg, sizeof s->commander_msg,
                     "Paste cancelled (%d done)", s->commander_paste_done);
            s->commander_paste_idx = 0; s->commander_paste_done = 0;
            s->commander_overwrite_all = false;
        }
        return;
    }

    if (k == JAPI_KEY_ESCAPE || k == JAPI_KEY_CTRL('J')) { s->commander_active = false; return; }
    if (k == JAPI_KEY_TAB) { s->commander_pane ^= 1; return; }   /* switch active pane */

    ui_filelist_t *act = &s->commander_list[s->commander_pane];
    if (k == JAPI_KEY_CTAB) { ui_filelist_key(act, JAPI_KEY_TAB); return; } /* switch drive */

    /* Multi-select: Space tags the current file then steps down; Shift+Up/Down
       extend a contiguous tagged run (like Shift-select in Windows). */
    if (k == ' ')            { commander_tag(act, act->sel);
                               ui_filelist_key(act, JAPI_KEY_DOWN); return; }
    if (k == JAPI_KEY_SDOWN) { commander_tag_set(act, act->sel);
                               ui_filelist_key(act, JAPI_KEY_DOWN);
                               commander_tag_set(act, act->sel); return; }
    if (k == JAPI_KEY_SUP)   { commander_tag_set(act, act->sel);
                               ui_filelist_key(act, JAPI_KEY_UP);
                               commander_tag_set(act, act->sel); return; }

    /* Windows-style file operations. */
    if (k == JAPI_KEY_CTRL('C')) { commander_clip(s, false); return; }   /* copy  */
    if (k == JAPI_KEY_CTRL('X')) { commander_clip(s, true);  return; }   /* cut   */
    if (k == JAPI_KEY_CTRL('V')) {                                       /* paste */
        s->commander_paste_idx = 0; s->commander_paste_done = 0;
        s->commander_overwrite_all = false;
        commander_paste(s); return;
    }
    if (k == JAPI_KEY_CTRL('R')) {                                       /* rename */
        const ui_filelist_entry_t *e = cmd_pick_file(s, "rename");
        if (e) commander_prompt(s, 1, e->name);
        return;
    }
    if (k == JAPI_KEY_CTRL('N')) { commander_prompt(s, 0, ""); return; } /* new folder */
    if (k == JAPI_KEY_DELETE) {                                          /* delete (confirm) */
        /* Count only (NULL names) -- no need to reserve a 2 KB names buffer on
           the core-0 stack just to decide whether to raise the prompt. */
        if (commander_collect(s, NULL, JBE_CLIP_MAX, true) > 0)
            { s->commander_confirm_delete = true; s->commander_msg[0] = 0; }
        return;
    }
    /* Navigation (arrows, PgUp/PgDn, Home/End) and Enter (into folders). */
    ui_filelist_key(act, k);
}

/* Clear one row's interior cells inside the Commander window. */
static void jfc_clear_row(int row, uint8_t fg, uint8_t bg) {
    for (int c = JFC_LEFT + 1; c < JFC_RIGHT; c++) vga_set_char(row, c, ' ', fg, bg);
}

static void render_commander(jbe_state_t *s) {
    const uint8_t BG = VGA_BLUE, BORD = VGA_WHITE, SHADOW = 0x15;
    const uint8_t TL=201, TR=187, BL=200, BR=188, HZ=205, VT=186, DIV=179;

    /* drop shadow so the window floats over the editor */
    for (int r = JFC_TOP + 1; r <= JFC_BOTTOM + 1; r++)
        vga_set_char(r, JFC_RIGHT + 1, ' ', SHADOW, SHADOW);
    for (int c = JFC_LEFT + 1; c <= JFC_RIGHT + 1; c++)
        vga_set_char(JFC_BOTTOM + 1, c, ' ', SHADOW, SHADOW);

    /* interior */
    for (int r = JFC_TOP; r <= JFC_BOTTOM; r++)
        for (int c = JFC_LEFT; c <= JFC_RIGHT; c++)
            vga_set_char(r, c, ' ', VGA_WHITE, BG);

    /* double-line outer border + centred title */
    vga_set_char(JFC_TOP, JFC_LEFT, TL, BORD, BG);     vga_set_char(JFC_TOP, JFC_RIGHT, TR, BORD, BG);
    vga_set_char(JFC_BOTTOM, JFC_LEFT, BL, BORD, BG);  vga_set_char(JFC_BOTTOM, JFC_RIGHT, BR, BORD, BG);
    for (int c = JFC_LEFT+1; c < JFC_RIGHT; c++) {
        vga_set_char(JFC_TOP, c, HZ, BORD, BG); vga_set_char(JFC_BOTTOM, c, HZ, BORD, BG);
    }
    for (int r = JFC_TOP+1; r < JFC_BOTTOM; r++) {
        vga_set_char(r, JFC_LEFT, VT, BORD, BG); vga_set_char(r, JFC_RIGHT, VT, BORD, BG);
    }
    const char *title = " Japi Commander ";
    vga_print(JFC_TOP, JFC_LEFT + (JFC_W - (int)strlen(title))/2, title, VGA_BLACK, VGA_CYAN);

    /* vertical divider between the two panes */
    for (int r = JFC_HDR_ROW; r < JFC_LIST_ROW + JFC_LIST_H; r++)
        vga_set_char(r, JFC_DIV, DIV, BORD, BG);

    /* per-pane cwd header (active pane marked with a triangle + yellow) + list */
    for (int p = 0; p < 2; p++) {
        bool active = (p == s->commander_pane);
        int  hcol = (p == 0) ? JFC_P0_COL : JFC_P1_COL;
        int  hw   = (p == 0) ? JFC_P0_W   : JFC_P1_W;
        char hdr[160];
        snprintf(hdr, sizeof hdr, "%s%s", active ? "\020 " : "  ", s->commander_list[p].cwd);
        if ((int)strlen(hdr) > hw) hdr[hw] = 0;
        vga_print(JFC_HDR_ROW, hcol, hdr, active ? VGA_YELLOW : VGA_WHITE, BG);

        ui_filelist_t *w = &s->commander_list[p];   /* dim the inactive selection bar */
        uint8_t sf = w->sel_fg, sb = w->sel_bg;
        if (!active) { w->sel_fg = VGA_WHITE; w->sel_bg = VGA_BLUE; }
        ui_filelist_render(w);
        w->sel_fg = sf; w->sel_bg = sb;
    }

    /* The key legend, built as little reverse-printed blocks (black on cyan):
       the key on the top row, the action spelled out below it, both left-
       aligned. The gap between blocks is spread so they fill the window width. */
    jfc_clear_row(JFC_MSG_ROW,  VGA_WHITE, BG);
    jfc_clear_row(JFC_LEG1_ROW, VGA_WHITE, BG);
    jfc_clear_row(JFC_LEG2_ROW, VGA_WHITE, BG);
    static const char *const LEG_KEYS[] =
        { "Ctrl+C", "Ctrl+X", "Ctrl+V", "Del", "Ctrl+R", "Ctrl+N",
          "Space", "Tab", "Ctrl+Tab", "Esc" };
    static const char *const LEG_ACTS[] =
        { "Copy", "Cut", "Paste", "Delete", "Rename", "New folder",
          "Select", "Switch pane", "Switch drive", "Close" };
    const int NLEG = (int)(sizeof LEG_KEYS / sizeof LEG_KEYS[0]);
    int sumw = 0;
    for (int i = 0; i < NLEG; i++) {
        int kl = (int)strlen(LEG_KEYS[i]), al = (int)strlen(LEG_ACTS[i]);
        sumw += (kl > al ? kl : al) + 2;          /* +2 for one pad each side */
    }
    int avail = (JFC_RIGHT - 1) - (JFC_LEFT + 2);
    int gap = (NLEG > 1) ? (avail - sumw) / (NLEG - 1) : 0;
    if (gap < 1) gap = 1;
    if (gap > 3) gap = 3;
    int x = JFC_LEFT + 2;
    for (int i = 0; i < NLEG; i++) {
        int kl = (int)strlen(LEG_KEYS[i]), al = (int)strlen(LEG_ACTS[i]);
        int w  = (kl > al ? kl : al) + 2;
        for (int c = x; c < x + w; c++) {         /* reverse-video chip */
            vga_set_char(JFC_LEG1_ROW, c, ' ', VGA_BLACK, VGA_CYAN);
            vga_set_char(JFC_LEG2_ROW, c, ' ', VGA_BLACK, VGA_CYAN);
        }
        vga_print(JFC_LEG1_ROW, x + 1, LEG_KEYS[i], VGA_BLACK, VGA_CYAN);
        vga_print(JFC_LEG2_ROW, x + 1, LEG_ACTS[i], VGA_BLACK, VGA_CYAN);
        x += w + gap;
    }

    /* The message row above the legend: a name prompt, a delete confirmation,
       or the last result. */
    if (s->commander_input_active) {
        const char *label = s->commander_input_kind == 0 ? "New folder name:"
                                                         : "Rename to:";
        char line[160];
        snprintf(line, sizeof line, " %s %s", label, s->commander_input);
        jfc_clear_row(JFC_MSG_ROW, JBE_PROMPT_FG, JBE_PROMPT_BG);
        vga_print(JFC_MSG_ROW, JFC_LEFT + 2, line, JBE_PROMPT_FG, JBE_PROMPT_BG);
        /* Block caret drawn in reverse video (yellow on black), like the editor
           cursor: it sits on the character it would push right, or on a blank
           cell just past the end of the text. Layout: leading space + label +
           one space, then the field. */
        int input_col = JFC_LEFT + 2 + 1 + (int)strlen(label) + 1;
        int caret_col = input_col + s->commander_input_cur;
        char cch = (s->commander_input_cur < s->commander_input_len)
                   ? s->commander_input[s->commander_input_cur] : ' ';
        if (caret_col < JFC_RIGHT)
            vga_set_char(JFC_MSG_ROW, caret_col, cch, JBE_PROMPT_BG, JBE_PROMPT_FG);
    } else if (s->commander_confirm_delete) {
        ui_filelist_t *a = &s->commander_list[s->commander_pane];
        int tagged = 0; bool has_dir = false;
        for (int i = 0; i < a->n_entries; i++)
            if (a->entries[i].tagged) { tagged++; if (a->entries[i].is_dir) has_dir = true; }
        if (tagged == 0 && a->n_entries) has_dir = a->entries[a->sel].is_dir;   /* current entry */
        char line[160];
        if (tagged > 1)
            snprintf(line, sizeof line, " Delete %d items%s ?   Y = yes, any other key = no",
                     tagged, has_dir ? " and all folder contents" : "");
        else
            snprintf(line, sizeof line, " Delete %s%s ?   Y = yes, any other key = no",
                     a->n_entries ? a->entries[a->sel].name : "",
                     has_dir ? " and all its contents" : "");
        jfc_clear_row(JFC_MSG_ROW, JBE_PROMPT_FG, JBE_PROMPT_BG);
        vga_print(JFC_MSG_ROW, JFC_LEFT + 2, line, JBE_PROMPT_FG, JBE_PROMPT_BG);
    } else if (s->commander_confirm_overwrite) {
        const char *nm = (s->commander_paste_idx < s->clip_n)
                         ? s->clip_names[s->commander_paste_idx] : "";
        char line[160];
        snprintf(line, sizeof line, " Overwrite %s ?   Y = yes, N = no, A = all", nm);
        jfc_clear_row(JFC_MSG_ROW, JBE_PROMPT_FG, JBE_PROMPT_BG);
        vga_print(JFC_MSG_ROW, JFC_LEFT + 2, line, JBE_PROMPT_FG, JBE_PROMPT_BG);
    } else if (s->commander_msg[0]) {
        vga_print(JFC_MSG_ROW, JFC_LEFT + 2, s->commander_msg, VGA_CYAN, BG);
    }
}

/* ---- F1 Help: a centred, framed window that floats over the editor --------
 * 92x52 cells, centred, with a drop shadow, so the editor stays visible all
 * around it. Content + anchors come from help_text.h (generated from the
 * manual by tools/mkhelp.py). The id->line lookup and the scroll/open helpers
 * live higher up (before jbe_handle_key, which calls them). */

static void render_help(const jbe_state_t *s) {
    enum { W = 92, H = 52 };
    const int left = (VGA_COLS - W) / 2;          /* 17 */
    const int top  = (VGA_ROWS - H) / 2;          /*  6 */
    const uint8_t BG = VGA_BLUE, FG = VGA_WHITE, BORD = VGA_WHITE;
    const uint8_t HEAD = VGA_YELLOW, SHADOW = 0x15;   /* dim grey */
    const uint8_t TL=201, TR=187, BL=200, BR=188, HZ=205, VT=186;  /* double line */

    /* drop shadow, offset one cell right and down, so the window floats */
    for (int r = top + 1; r <= top + H; r++)
        vga_set_char(r, left + W, ' ', SHADOW, SHADOW);
    for (int c = left + 1; c <= left + W; c++)
        vga_set_char(top + H, c, ' ', SHADOW, SHADOW);

    /* interior */
    for (int r = top; r < top + H; r++)
        for (int c = left; c < left + W; c++)
            vga_set_char(r, c, ' ', FG, BG);

    /* double-line border */
    vga_set_char(top,       left,     TL, BORD, BG);
    vga_set_char(top,       left+W-1, TR, BORD, BG);
    vga_set_char(top+H-1,   left,     BL, BORD, BG);
    vga_set_char(top+H-1,   left+W-1, BR, BORD, BG);
    for (int c = left+1; c < left+W-1; c++) {
        vga_set_char(top,     c, HZ, BORD, BG);
        vga_set_char(top+H-1, c, HZ, BORD, BG);
    }
    for (int r = top+1; r < top+H-1; r++) {
        vga_set_char(r, left,     VT, BORD, BG);
        vga_set_char(r, left+W-1, VT, BORD, BG);
    }

    /* title, centred in the top border (black on cyan) */
    const char *title = " Help ";
    vga_print(top, left + (W - (int)strlen(title)) / 2, title, VGA_BLACK, VGA_CYAN);

    /* content */
    const int inner_top = top + 2, inner_left = left + 3, rows = H - 4;
    for (int i = 0; i < rows; i++) {
        int li = s->help_top + i;
        if (li >= HELP_NLINES) break;
        vga_print(inner_top + i, inner_left, HELP_LINES[li].text,
                  HELP_LINES[li].heading ? HEAD : FG, BG);
    }

    /* footer hint, centred in the bottom border */
    const char *foot = " \030\031 PgUp/PgDn   Esc to close ";
    vga_print(top+H-1, left + (W - (int)strlen(foot)) / 2, foot, VGA_BLACK, VGA_CYAN);
}

/* The Options -> CPU speed chooser: a small centred dialog. */
static void render_cpu_dialog(const jbe_state_t *s) {
    enum { W = 52, H = 11 };
    const int left = (VGA_COLS - W) / 2, top = (VGA_ROWS - H) / 2;
    const uint8_t BG = VGA_BLUE, BORD = VGA_WHITE, SHADOW = 0x15;
    const uint8_t TL=201, TR=187, BL=200, BR=188, HZ=205, VT=186;
    int cur = cpu_tier_index(japi_get_cpu_clock_mhz());
    static const char *labels[3] = { "260 MHz   safe / fallback",
                                     "324 MHz   default high gear",
                                     "390 MHz   opt-in turbo" };
    for (int r = top + 1; r <= top + H; r++) vga_set_char(r, left + W, ' ', SHADOW, SHADOW);
    for (int c = left + 1; c <= left + W; c++) vga_set_char(top + H, c, ' ', SHADOW, SHADOW);
    for (int r = top; r < top + H; r++)
        for (int c = left; c < left + W; c++) vga_set_char(r, c, ' ', VGA_WHITE, BG);
    vga_set_char(top, left, TL, BORD, BG);         vga_set_char(top, left+W-1, TR, BORD, BG);
    vga_set_char(top+H-1, left, BL, BORD, BG);     vga_set_char(top+H-1, left+W-1, BR, BORD, BG);
    for (int c = left+1; c < left+W-1; c++) { vga_set_char(top, c, HZ, BORD, BG);
                                              vga_set_char(top+H-1, c, HZ, BORD, BG); }
    for (int r = top+1; r < top+H-1; r++) { vga_set_char(r, left, VT, BORD, BG);
                                            vga_set_char(r, left+W-1, VT, BORD, BG); }
    const char *title = " CPU speed ";
    vga_print(top, left + (W - (int)strlen(title))/2, title, VGA_BLACK, VGA_CYAN);

    for (int i = 0; i < 3; i++) {
        int row = top + 2 + i;
        bool selrow = (i == s->cpu_sel);
        uint8_t fg = selrow ? VGA_BLACK : VGA_WHITE, bg = selrow ? VGA_CYAN : BG;
        for (int c = left+1; c < left+W-1; c++) vga_set_char(row, c, ' ', fg, bg);
        char line[64];
        snprintf(line, sizeof line, "%s %s%s", selrow ? "\020" : " ",
                 labels[i], i == cur ? "   (current)" : "");
        vga_print(row, left + 3, line, fg, bg);
    }
    vga_print(top + H - 3, left + 3, "\030\031 choose   Enter apply   Esc cancel", VGA_WHITE, BG);
    vga_print(top + H - 2, left + 3, "Changing the speed reboots (~1 s).", VGA_YELLOW, BG);
}

void jbe_render(const jbe_state_t *cs) {
    /* The pane-render helper has to flip active_pane while it runs, so we
       cast away const for that scope; active_pane is restored before the
       function returns and the state ends up unchanged. */
    jbe_state_t *s = (jbe_state_t *)cs;

    render_menu_bar(s);

    if (!s->split_active) {
        render_pane(s, 0, JBE_VIEW_TOP, JBE_VIEW_BOTTOM, true);
    } else {
        int div = s->split_row;
        if (div < JBE_VIEW_TOP + 1)      div = JBE_VIEW_TOP + 1;
        if (div > JBE_VIEW_BOTTOM - 1)   div = JBE_VIEW_BOTTOM - 1;
        render_pane(s, 0, JBE_VIEW_TOP, div - 1, s->active_pane == 0);
        render_divider(div);
        render_pane(s, 1, div + 1, JBE_VIEW_BOTTOM, s->active_pane == 1);
    }

    /* Status bar. Only the "Cannot open" notice still tints it yellow; the
       interactive prompts (Save As, the unsaved-changes confirm, Find/Replace)
       now draw their own framed boxes just above it, so the bar keeps its normal
       cursor + file readout underneath. */
    bool awaiting_input = s->open_msg[0];
    uint8_t bar_fg = awaiting_input ? JBE_PROMPT_FG : JBE_BAR_FG;
    uint8_t bar_bg = awaiting_input ? JBE_PROMPT_BG : JBE_BAR_BG;
    fill_row(JBE_STATUS_ROW, bar_fg, bar_bg);
    char status[512];
    if (s->open_msg[0]) {
        snprintf(status, sizeof status, " %s   (press any key)", s->open_msg);
    } else {
        const char *rec = s->macro_recording ? "   [REC]" : "";
        snprintf(status, sizeof status,
                 " Ln %d, Col %d   %d line%s   %s%s%s",
                 JBE_PANE(s)->cur_row + 1, JBE_PANE(s)->cur_col + 1,
                 JBE_BUF(s)->n_lines, JBE_BUF(s)->n_lines == 1 ? "" : "s",
                 JBE_BUF(s)->filename, JBE_BUF(s)->dirty ? "*" : "", rec);
    }
    vga_print(JBE_STATUS_ROW, 0, status, bar_fg, bar_bg);

    /* Framed black-on-yellow prompt boxes just above the status bar. At most one
       of these modes is active at a time. */
    if (JBE_PANE(s)->find_active || JBE_PANE(s)->replace_active)
        render_search_box(s);
    else if (s->save_as_active)
        render_saveas_box(s);
    else if (s->goto_active)
        render_goto_box(s);
    else if (s->close_confirm)
        render_confirm_box(s);

    /* File→Open dialog overlay. Sits below the dropdown so a menu opened
       on top of an active dialog still wins, though in practice the dialog
       blocks all keys including Alt. */
    if (s->open_active) {
        /* Fill cyan panel. */
        for (int r = 0; r < JBE_OPEN_DLG_H; r++) {
            for (int c = 0; c < JBE_OPEN_DLG_W; c++) {
                vga_set_char(JBE_OPEN_DLG_ROW + r, JBE_OPEN_DLG_COL + c,
                             ' ', JBE_DROP_FG, JBE_DROP_BG);
            }
        }
        /* Title with cwd. cwd can be up to UI_FILELIST_PATH_MAX, so size
           the buffer to fit any cwd, then hard-truncate to the dialog
           width before printing so it can't overflow the panel. Black on
           cyan matches the dialog body — yellow on cyan was too low
           contrast (both share the green channel). */
        char title[UI_FILELIST_PATH_MAX + 16];
        snprintf(title, sizeof title, " Open: %s", s->open_dlg.cwd);
        if ((int)strlen(title) > JBE_OPEN_DLG_W) title[JBE_OPEN_DLG_W] = 0;
        vga_print(JBE_OPEN_DLG_ROW, JBE_OPEN_DLG_COL, title,
                  JBE_DROP_FG, JBE_DROP_BG);
        /* Separator row below title. */
        for (int c = 0; c < JBE_OPEN_DLG_W; c++) {
            vga_set_char(JBE_OPEN_DLG_ROW + 1, JBE_OPEN_DLG_COL + c,
                         '-', JBE_DROP_FG, JBE_DROP_BG);
        }
        /* Help line at the bottom — or a delete confirmation (black on yellow,
           like the other input prompts) when one is pending. */
        int help_row = JBE_OPEN_DLG_ROW + JBE_OPEN_DLG_H - 1;
        if (s->open_dlg.confirm_delete) {
            char msg[UI_FILELIST_NAME_MAX + 40];
            snprintf(msg, sizeof msg, " Delete %s ?  Y=yes  other key=no",
                     s->open_dlg.entries[s->open_dlg.sel].name);
            if ((int)strlen(msg) > JBE_OPEN_DLG_W) msg[JBE_OPEN_DLG_W] = 0;
            for (int c = 0; c < JBE_OPEN_DLG_W; c++)
                vga_set_char(help_row, JBE_OPEN_DLG_COL + c, ' ',
                             JBE_PROMPT_FG, JBE_PROMPT_BG);
            vga_print(help_row, JBE_OPEN_DLG_COL, msg, JBE_PROMPT_FG, JBE_PROMPT_BG);
        } else {
            vga_print(help_row, JBE_OPEN_DLG_COL,
                      " Enter=open  Tab=A:/C:  Del=delete  Esc=cancel",
                      JBE_DROP_FG, JBE_DROP_BG);
        }

        ui_filelist_render(&s->open_dlg);
    }

    /* Dropdown overlay last so it covers everything below the bar. */
    render_dropdown(s);

    /* F1 Help floats on top of everything, with the editor visible around it. */
    /* The Japi Commander and the F1 help float over the editor (it stays
       visible around them). Only one is ever active at a time. */
    if (s->commander_active)  render_commander(s);
    if (s->help_active)       render_help(s);
    if (s->cpu_dialog_active) render_cpu_dialog(s);
}
