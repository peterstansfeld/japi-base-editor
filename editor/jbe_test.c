/* Headless test for JBE MVP step 1.
 * Writes a small file to A: via the platform API, loads it, drives keys
 * through the sim injection API, and asserts cursor/viewport behaviour.
 * Exit 0 = pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "japi_base.h"
#include "japi_sim.h"
#include "jbe.h"
#include "help_text.h"   /* HELP_ANCHORS[], to assert F1 jumps to the right line */

/* Line number of a help anchor, or -1. Mirrors help_anchor_line() in jbe.c. */
static int help_anchor(const char *id) {
    for (int i = 0; HELP_ANCHORS[i].id; i++)
        if (strcmp(HELP_ANCHORS[i].id, id) == 0) return HELP_ANCHORS[i].line;
    return -1;
}

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

/* Build a small fixture file on the simulated A: drive. */
static bool make_fixture(const char *path, const char *content) {
    japi_file_t f;
    if (!japi_fopen(&f, path, JAPI_WRITE)) return false;
    int n = (int)strlen(content);
    bool ok = (japi_fwrite(&f, content, n) == n);
    japi_fclose(&f);
    return ok;
}

/* Tracked git fixtures on A: that the tests must never leave altered:
   scratch.txt is the editor's sandbox file and demo.z80 a syntax sample, both
   committed. We snapshot their bytes at startup and rewrite them on exit, so a
   stray delete/rename in any test (now or later) can never dirty the working
   tree. Rewriting identical bytes is a no-op as far as git is concerned. */
static struct { const char *path; char *data; int len; bool had; }
    g_fixtures[] = { { "A:scratch.txt", NULL, 0, false },
                     { "A:demo.z80",    NULL, 0, false } };

static void fixtures_snapshot(void) {
    for (size_t i = 0; i < sizeof g_fixtures / sizeof g_fixtures[0]; i++) {
        japi_file_t f;
        if (!japi_fopen(&f, g_fixtures[i].path, JAPI_READ)) continue;
        int cap = 4096, len = 0; char *buf = malloc((size_t)cap);
        for (;;) {
            if (len == cap) { cap *= 2; buf = realloc(buf, (size_t)cap); }
            int n = japi_fread(&f, buf + len, cap - len);
            if (n <= 0) break;
            len += n;
        }
        japi_fclose(&f);
        g_fixtures[i].data = buf; g_fixtures[i].len = len; g_fixtures[i].had = true;
    }
}

static void fixtures_restore(void) {
    for (size_t i = 0; i < sizeof g_fixtures / sizeof g_fixtures[0]; i++) {
        if (!g_fixtures[i].had) continue;
        japi_file_t f;
        if (japi_fopen(&f, g_fixtures[i].path, JAPI_WRITE)) {
            japi_fwrite(&f, g_fixtures[i].data, g_fixtures[i].len);
            japi_fclose(&f);
        }
        free(g_fixtures[i].data); g_fixtures[i].data = NULL;
    }
}

/* Read a whole file into out[] as a NUL-terminated string. */
static bool slurp(const char *path, char *out, int max) {
    japi_file_t f;
    if (!japi_fopen(&f, path, JAPI_READ)) return false;
    int n = japi_fread(&f, out, max - 1);
    japi_fclose(&f);
    if (n < 0) n = 0;
    out[n] = 0;
    return true;
}

/* Feed a whole string as individual keystrokes. */
static void type_str(jbe_state_t *s, const char *text) {
    for (const char *p = text; *p; p++) jbe_handle_key(s, (uint16_t)(unsigned char)*p);
}

int main(void) {
    japi_init();

    /* Protect the committed A: fixtures: snapshot now, restore on exit. */
    fixtures_snapshot();
    atexit(fixtures_restore);

    /* 5 lines, with a deliberately long 3rd line for horizontal scroll. */
    const char *FIXTURE =
        "first line\n"
        "second\n"
        "this is a much longer line used to exercise horizontal scrolling well past 127 columns -- xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"
        "fourth\n"
        "fifth and last\n";
    CHECK(make_fixture("A:jbe_t.txt", FIXTURE), "write fixture to A:");

    jbe_state_t st;
    jbe_init(&st);
    CHECK(jbe_load(&st, "A:jbe_t.txt"), "load fixture");
    CHECK(JBE_BUF(&st)->n_lines == 5,              "5 lines parsed");
    CHECK(JBE_PANE(&st)->cur_row == 0 && JBE_PANE(&st)->cur_col == 0, "cursor starts at (0,0)");
    CHECK(JBE_PANE(&st)->top_row == 0, "viewport starts at row 0");

    /* DOWN/RIGHT/END/HOME basics */
    jbe_handle_key(&st, JAPI_KEY_DOWN);  CHECK(JBE_PANE(&st)->cur_row == 1, "DOWN -> row 1");
    jbe_handle_key(&st, JAPI_KEY_END);   CHECK(JBE_PANE(&st)->cur_col == JBE_BUF(&st)->len[1], "END at end of 'second'");
    jbe_handle_key(&st, JAPI_KEY_HOME);  CHECK(JBE_PANE(&st)->cur_col == 0, "HOME -> col 0");

    /* Column clipped after vertical move into a shorter line */
    JBE_PANE(&st)->cur_col = 0; JBE_PANE(&st)->cur_row = 0;
    jbe_handle_key(&st, JAPI_KEY_END);   /* col = strlen("first line")=10 */
    jbe_handle_key(&st, JAPI_KEY_DOWN);  /* "second" len 6 -> col clipped to 6 */
    CHECK(JBE_PANE(&st)->cur_col == 6, "column clipped to shorter line");

    /* Long line wraps over multiple visual rows (no horizontal scroll). */
    JBE_PANE(&st)->cur_row = 2; JBE_PANE(&st)->cur_col = 0;
    jbe_handle_key(&st, JAPI_KEY_END);
    CHECK(JBE_PANE(&st)->cur_col == JBE_BUF(&st)->len[2], "END jumped to end of long line");
    /* line 2 in the fixture is >125 chars, so it must span at least 2 sub-rows */
    {
        int subs = (JBE_BUF(&st)->len[2] + JBE_WRAP_WIDTH - 1) / JBE_WRAP_WIDTH;
        CHECK(subs >= 2, "long line wraps into >=2 visual rows");
    }

    /* DOWN inside a wrapped line stays on the same file row but advances
       one visual sub-row (per-visual-row navigation). */
    JBE_PANE(&st)->cur_row = 2; JBE_PANE(&st)->cur_col = 10;
    jbe_handle_key(&st, JAPI_KEY_DOWN);
    CHECK(JBE_PANE(&st)->cur_row == 2, "DOWN stays on same file row while wrap remaining");
    CHECK(JBE_PANE(&st)->cur_col == 10 + JBE_WRAP_WIDTH, "DOWN advances one sub-row");

    /* UP from the second sub-row returns to col 10 on the same file row. */
    jbe_handle_key(&st, JAPI_KEY_UP);
    CHECK(JBE_PANE(&st)->cur_row == 2 && JBE_PANE(&st)->cur_col == 10, "UP returns within wrapped line");

    /* Vertical scroll past viewport with PgDn (file has only 5 lines so it
       clamps to last line; viewport adjusts to keep cursor visible). */
    JBE_PANE(&st)->cur_row = 0; JBE_PANE(&st)->top_row = 0;
    jbe_handle_key(&st, JAPI_KEY_PGDN);
    CHECK(JBE_PANE(&st)->cur_row == JBE_BUF(&st)->n_lines - 1, "PGDN clamps to last line");

    /* Render does not crash; check that the wrap marker appears at the
       boundary of the long line on at least one visual sub-row. */
    JBE_PANE(&st)->cur_row = 0; JBE_PANE(&st)->cur_col = 0; JBE_PANE(&st)->top_row = 0;
    jbe_render(&st);
    {
        bool found = false;
        for (int r = JBE_VIEW_TOP; r <= JBE_VIEW_BOTTOM && !found; r++) {
            vga_char_t cell = vga_text_buffer[r][JBE_WRAP_WIDTH];
            if (cell.code == JBE_WRAP_GLYPH) found = true;
        }
        CHECK(found, "wrap marker drawn on a wrapped sub-row");
    }

    jbe_free(&st);
    japi_remove("A:jbe_t.txt");

    /* ----- MVP step 2: editing ---------------------------------------- */
    {
        jbe_state_t e;
        jbe_init(&e);
        /* jbe_handle_key short-circuits on n_lines==0, so seed an empty line. */
        CHECK(make_fixture("A:jbe_e.txt", ""), "write empty fixture");
        CHECK(jbe_load(&e, "A:jbe_e.txt"), "load empty fixture");
        CHECK(JBE_BUF(&e)->n_lines == 1 && JBE_BUF(&e)->len[0] == 0, "one empty line after load");
        CHECK(!JBE_BUF(&e)->dirty, "dirty clear after load");

        /* Type "hello" */
        const char *s = "hello";
        for (int i = 0; s[i]; i++) jbe_handle_key(&e, (uint16_t)s[i]);
        CHECK(JBE_BUF(&e)->n_lines == 1 && strcmp(JBE_BUF(&e)->lines[0], "hello") == 0, "typed 'hello'");
        CHECK(JBE_PANE(&e)->cur_row == 0 && JBE_PANE(&e)->cur_col == 5, "cursor after 'hello' at (0,5)");
        CHECK(JBE_BUF(&e)->dirty, "dirty set after typing");

        /* Enter at col 2 -> split into "he" / "llo" */
        JBE_PANE(&e)->cur_col = 2;
        jbe_handle_key(&e, JAPI_KEY_ENTER);
        CHECK(JBE_BUF(&e)->n_lines == 2, "split into two lines");
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "he")  == 0, "line 0 == 'he'");
        CHECK(strcmp(JBE_BUF(&e)->lines[1], "llo") == 0, "line 1 == 'llo'");
        CHECK(JBE_PANE(&e)->cur_row == 1 && JBE_PANE(&e)->cur_col == 0, "cursor at (1,0) after Enter");

        /* Backspace at start of line 1 -> join back into 'hello', cursor at (0,2) */
        jbe_handle_key(&e, JAPI_KEY_BACKSPACE);
        CHECK(JBE_BUF(&e)->n_lines == 1 && strcmp(JBE_BUF(&e)->lines[0], "hello") == 0, "join via Backspace");
        CHECK(JBE_PANE(&e)->cur_row == 0 && JBE_PANE(&e)->cur_col == 2, "cursor restored to join point");

        /* Delete at cursor (col 2 of 'hello') removes 'l' -> 'helo' */
        jbe_handle_key(&e, JAPI_KEY_DELETE);
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "helo") == 0, "Delete removed 'l' -> 'helo'");
        CHECK(JBE_PANE(&e)->cur_col == 2, "cursor stays at 2 after forward delete");

        /* Set up ["ab","cd"]; Delete at end of line 0 joins -> "abcd" */
        jbe_free(&e);
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_e.txt", "ab\ncd\n"), "write ab/cd fixture");
        CHECK(jbe_load(&e, "A:jbe_e.txt"), "load ab/cd");
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 2;
        jbe_handle_key(&e, JAPI_KEY_DELETE);
        CHECK(JBE_BUF(&e)->n_lines == 1 && strcmp(JBE_BUF(&e)->lines[0], "abcd") == 0,
              "Delete at EOL joins next line");
        CHECK(JBE_PANE(&e)->cur_row == 0 && JBE_PANE(&e)->cur_col == 2, "cursor stays at join point");

        jbe_free(&e);
        japi_remove("A:jbe_e.txt");
    }

    /* ----- MVP step 3: save (Ctrl+S) ---------------------------------- */
    {
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_s.txt", "alpha\nbeta\n"), "write 2-line fixture");
        CHECK(jbe_load(&e, "A:jbe_s.txt"), "load 2-line fixture");
        CHECK(!JBE_BUF(&e)->dirty, "load clears dirty");
        CHECK(strcmp(JBE_BUF(&e)->path, "A:jbe_s.txt") == 0, "path stored on load");

        /* Insert 'X' at start of line 0 -> 'Xalpha' */
        jbe_handle_key(&e, 'X');
        CHECK(JBE_BUF(&e)->dirty, "typing sets dirty");
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "Xalpha") == 0, "first line is 'Xalpha'");

        /* Ctrl+S saves -> dirty clears, file content matches edit */
        jbe_handle_key(&e, JAPI_KEY_CTRL('S'));
        if (e.save_request) { e.save_request = false; jbe_save(&e); }  /* loop does the write */
        CHECK(!JBE_BUF(&e)->dirty, "Ctrl+S clears dirty");

        jbe_state_t e2;
        jbe_init(&e2);
        CHECK(jbe_load(&e2, "A:jbe_s.txt"), "reload saved file");
        CHECK(JBE_BUF(&e2)->n_lines == 2,                          "reload: 2 lines");
        CHECK(strcmp(JBE_BUF(&e2)->lines[0], "Xalpha") == 0,       "reload: line 0 = 'Xalpha'");
        CHECK(strcmp(JBE_BUF(&e2)->lines[1], "beta")   == 0,       "reload: line 1 = 'beta'");

        jbe_free(&e);
        jbe_free(&e2);
        japi_remove("A:jbe_s.txt");
    }

    /* ----- MVP step 5a: stream selection + Ctrl+C / Ctrl+V ------------ */
    {
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_5a.txt", "hello world\nsecond line\nthird\n"),
              "write 3-line fixture for 5a");
        CHECK(jbe_load(&e, "A:jbe_5a.txt"), "load 5a fixture");

        /* Single-line selection: Shift+RIGHT x5 from (0,0) selects "hello" */
        for (int i = 0; i < 5; i++) jbe_handle_key(&e, JAPI_KEY_SRIGHT);
        CHECK(JBE_PANE(&e)->sel_active, "selection active after Shift+RIGHT");
        CHECK(JBE_PANE(&e)->sel_row == 0 && JBE_PANE(&e)->sel_col == 0, "anchor at start");
        CHECK(JBE_PANE(&e)->cur_row == 0 && JBE_PANE(&e)->cur_col == 5, "cursor at end of selection");

        /* Ctrl+C copies "hello" to clipboard; selection stays */
        jbe_handle_key(&e, JAPI_KEY_CTRL('C'));
        CHECK(e.clip && e.clip_len == 5 && strncmp(e.clip, "hello", 5) == 0,
              "clip = 'hello'");
        CHECK(JBE_PANE(&e)->sel_active, "selection still active after copy");

        /* Plain arrow clears the selection */
        jbe_handle_key(&e, JAPI_KEY_RIGHT);
        CHECK(!JBE_PANE(&e)->sel_active, "plain arrow clears selection");

        /* Move to end-of-line-0 + Ctrl+V appends "hello" -> "hello worldhello" */
        jbe_handle_key(&e, JAPI_KEY_END);
        jbe_handle_key(&e, JAPI_KEY_CTRL('V'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "hello worldhello") == 0, "stream paste appends");
        CHECK(JBE_PANE(&e)->cur_col == 16, "cursor after pasted text");

        /* Multi-line selection: start fresh, select first two lines fully */
        jbe_free(&e); jbe_init(&e);
        CHECK(jbe_load(&e, "A:jbe_5a.txt"), "reload 5a fixture");
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 0;
        jbe_handle_key(&e, JAPI_KEY_SDOWN);
        jbe_handle_key(&e, JAPI_KEY_SEND);
        /* Selection now: (0,0) -> (1,len[1]) which spans "hello world\nsecond line" */
        jbe_handle_key(&e, JAPI_KEY_CTRL('C'));
        CHECK(e.clip_len == 11 + 1 + 11, "multi-line clip length"); /* "hello world" + '\n' + "second line" */
        CHECK(strncmp(e.clip, "hello world\nsecond line", 23) == 0, "multi-line clip content");

        /* Paste-over-selection replaces */
        jbe_free(&e); jbe_init(&e);
        CHECK(make_fixture("A:jbe_5a.txt", "abcdef\n"), "write 'abcdef'");
        CHECK(jbe_load(&e, "A:jbe_5a.txt"), "reload short");
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 2;
        jbe_handle_key(&e, JAPI_KEY_SRIGHT);
        jbe_handle_key(&e, JAPI_KEY_SRIGHT);   /* selection: "cd" */
        /* Manually seed clip = "XY" so we don't depend on copy */
        free(e.clip);
        e.clip = malloc(3); memcpy(e.clip, "XY", 2); e.clip[2] = 0;
        e.clip_len = 2; e.clip_block = false;
        jbe_handle_key(&e, JAPI_KEY_CTRL('V'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "abXYef") == 0, "paste replaces selection");
        CHECK(!JBE_PANE(&e)->sel_active, "selection cleared after paste");

        jbe_free(&e);
        japi_remove("A:jbe_5a.txt");
    }

    /* ----- MVP step 5b: Ctrl+X cut + overtype-on-selection ------------ */
    {
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_5b.txt", "abcdef\nghij\n"), "write 5b fixture");
        CHECK(jbe_load(&e, "A:jbe_5b.txt"), "load 5b fixture");

        /* Cut "cd" from "abcdef" */
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 2;
        jbe_handle_key(&e, JAPI_KEY_SRIGHT);
        jbe_handle_key(&e, JAPI_KEY_SRIGHT);
        jbe_handle_key(&e, JAPI_KEY_CTRL('X'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "abef") == 0, "Ctrl+X removed 'cd'");
        CHECK(e.clip_len == 2 && strncmp(e.clip, "cd", 2) == 0, "clip has 'cd'");
        CHECK(!JBE_PANE(&e)->sel_active, "selection gone after cut");
        CHECK(JBE_PANE(&e)->cur_row == 0 && JBE_PANE(&e)->cur_col == 2, "cursor at cut point");

        /* Typing replaces the selection */
        JBE_PANE(&e)->cur_col = 0;
        jbe_handle_key(&e, JAPI_KEY_SRIGHT);
        jbe_handle_key(&e, JAPI_KEY_SRIGHT);   /* selection: "ab" */
        jbe_handle_key(&e, 'Z');
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "Zef") == 0, "typing replaces selection");
        CHECK(!JBE_PANE(&e)->sel_active, "selection cleared by overtype");
        CHECK(JBE_PANE(&e)->cur_col == 1, "cursor after typed char");

        /* Backspace deletes the selection without removing extra chars */
        jbe_handle_key(&e, JAPI_KEY_HOME);
        jbe_handle_key(&e, JAPI_KEY_SRIGHT);
        jbe_handle_key(&e, JAPI_KEY_SRIGHT);   /* selection: "Ze" */
        jbe_handle_key(&e, JAPI_KEY_BACKSPACE);
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "f") == 0, "BS over selection deletes range");
        CHECK(JBE_PANE(&e)->cur_col == 0, "cursor at start of deleted range");

        /* Enter on a selection replaces it with a newline split */
        jbe_free(&e); jbe_init(&e);
        CHECK(make_fixture("A:jbe_5b.txt", "XYZW\n"), "write XYZW");
        CHECK(jbe_load(&e, "A:jbe_5b.txt"), "reload XYZW");
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 1;
        jbe_handle_key(&e, JAPI_KEY_SRIGHT);
        jbe_handle_key(&e, JAPI_KEY_SRIGHT);   /* selection: "YZ" */
        jbe_handle_key(&e, JAPI_KEY_ENTER);
        CHECK(JBE_BUF(&e)->n_lines == 2,                    "Enter on selection splits");
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "X") == 0,      "head 'X'");
        CHECK(strcmp(JBE_BUF(&e)->lines[1], "W") == 0,      "tail 'W' on new line");

        jbe_free(&e);
        japi_remove("A:jbe_5b.txt");
    }

    /* ----- MVP step 5c: block selection + dimension-aware clipboard --- */
    {
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_5c.txt",
                           "foo(); // legacy\n"
                           "bar(); // legacy\n"
                           "baz(); // legacy\n"),
              "write 5c fixture");
        CHECK(jbe_load(&e, "A:jbe_5c.txt"), "load 5c fixture");

        /* Block-select cols 7..16 over 3 rows (the "// legacy" column) */
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 7;
        for (int i = 0; i < 9; i++) jbe_handle_key(&e, JAPI_KEY_CSRIGHT);
        jbe_handle_key(&e, JAPI_KEY_CSDOWN);
        jbe_handle_key(&e, JAPI_KEY_CSDOWN);
        CHECK(JBE_PANE(&e)->sel_active && JBE_PANE(&e)->sel_block,     "block selection active");

        /* Ctrl+X removes the column from all 3 lines, rows stay */
        jbe_handle_key(&e, JAPI_KEY_CTRL('X'));
        CHECK(JBE_BUF(&e)->n_lines == 3,                  "Ctrl+X on block keeps row count");
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "foo(); ") == 0, "row 0 prefix kept");
        CHECK(strcmp(JBE_BUF(&e)->lines[1], "bar(); ") == 0, "row 1 prefix kept");
        CHECK(strcmp(JBE_BUF(&e)->lines[2], "baz(); ") == 0, "row 2 prefix kept");
        CHECK(e.clip_block, "clip marked as block");

        /* Paste at end of row 0 (col 7). Three "// legacy" segments land
           on rows 0/1/2 each at col 7 -> we restore the original 3 lines. */
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = (int)strlen("foo(); ");
        jbe_handle_key(&e, JAPI_KEY_CTRL('V'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "foo(); // legacy") == 0, "row 0 restored");
        CHECK(strcmp(JBE_BUF(&e)->lines[1], "bar(); // legacy") == 0, "row 1 restored");
        CHECK(strcmp(JBE_BUF(&e)->lines[2], "baz(); // legacy") == 0, "row 2 restored");

        /* Easier targeted check: paste again at row 0, end of "foo(); // legacy",
           cursor col 16 — segment 1 -> row 1 col 16 (padded), seg 2 -> row 2 col 16 */
        jbe_free(&e); jbe_init(&e);
        CHECK(make_fixture("A:jbe_5c.txt",
                           "AAA\n"
                           "BBB\n"
                           "CCC\n"),
              "write 3x3 fixture");
        CHECK(jbe_load(&e, "A:jbe_5c.txt"), "reload");

        /* Block-select the middle column (col 1..2) over all 3 rows */
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 1;
        jbe_handle_key(&e, JAPI_KEY_CSRIGHT);
        jbe_handle_key(&e, JAPI_KEY_CSDOWN);
        jbe_handle_key(&e, JAPI_KEY_CSDOWN);
        jbe_handle_key(&e, JAPI_KEY_CTRL('C'));
        CHECK(e.clip_block && e.clip_len == 5, "block clip length 5");  /* "A\nB\nC" */
        CHECK(strncmp(e.clip, "A\nB\nC", 5) == 0, "block clip = 'A\\nB\\nC'");

        /* Paste at end of row 2 (col 3). Seg 0 goes to row 2 col 3
           ("CCCA"); rows 3 + 4 are appended and padded to col 3 with
           spaces before each segment so the rectangle keeps its shape. */
        JBE_PANE(&e)->cur_row = 2; JBE_PANE(&e)->cur_col = 3; JBE_PANE(&e)->sel_active = false;
        jbe_handle_key(&e, JAPI_KEY_CTRL('V'));
        CHECK(strcmp(JBE_BUF(&e)->lines[2], "CCCA") == 0, "block paste seg 0 appended");
        CHECK(JBE_BUF(&e)->n_lines == 5,                  "two new rows created");
        CHECK(strcmp(JBE_BUF(&e)->lines[3], "   B") == 0, "row 3 padded then 'B'");
        CHECK(strcmp(JBE_BUF(&e)->lines[4], "   C") == 0, "row 4 padded then 'C'");

        /* Paste into a too-short row pads with spaces */
        jbe_free(&e); jbe_init(&e);
        CHECK(make_fixture("A:jbe_5c.txt", "X\n\nY\n"), "write x-empty-y");
        CHECK(jbe_load(&e, "A:jbe_5c.txt"), "reload");
        free(e.clip);
        e.clip = malloc(4); memcpy(e.clip, "1\n2", 3); e.clip[3] = 0;
        e.clip_len = 3; e.clip_block = true;
        JBE_PANE(&e)->cur_row = 1; JBE_PANE(&e)->cur_col = 4;          /* row 1 is empty, col 4 is past EOL */
        jbe_handle_key(&e, JAPI_KEY_CTRL('V'));
        CHECK(strcmp(JBE_BUF(&e)->lines[1], "    1") == 0, "empty row padded then '1'");
        /* Row 2 is "Y" (len 1); padding from col 1 to col 4 with spaces,
           then '2' inserted on col 4 -> "Y   2". */
        CHECK(strcmp(JBE_BUF(&e)->lines[2], "Y   2") == 0, "short row kept + padding + '2'");

        jbe_free(&e);
        japi_remove("A:jbe_5c.txt");
    }

    /* ----- MVP step 10: undo / redo (Ctrl+Z / Ctrl+Y) ----------------- */
    {
        /* 1. Single-char insert + undo. */
        jbe_state_t e; jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", ""), "write empty for undo");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load empty for undo");
        jbe_handle_key(&e, 'a'); jbe_handle_key(&e, 'b'); jbe_handle_key(&e, 'c');
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "abc") == 0, "typed 'abc'");
        /* "abc" is one word -> all three chars are coalesced into ONE record.
           One Ctrl+Z therefore undoes the whole word in a single step. */
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "") == 0, "Ctrl+Z undoes whole coalesced word");
        CHECK(JBE_PANE(&e)->cur_col == 0, "cursor restored to start");
        jbe_free(&e);

        /* 2. Word-boundary coalescing: "hallo wereld" -> 3 records
              (word, space, word). Each Ctrl+Z removes one. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", ""), "write empty 2");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load empty 2");
        const char *hw = "hallo wereld";
        for (int i = 0; hw[i]; i++) jbe_handle_key(&e, (uint16_t)hw[i]);
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "hallo wereld") == 0, "typed 'hallo wereld'");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "hallo ") == 0, "Ctrl+Z 1: 'wereld' gone");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "hallo") == 0, "Ctrl+Z 2: space gone");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "") == 0, "Ctrl+Z 3: 'hallo' gone");
        jbe_free(&e);

        /* 3. Enter (SPLIT) + undo. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", "abcdef\n"), "write 'abcdef'");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load 'abcdef'");
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 3;
        jbe_handle_key(&e, JAPI_KEY_ENTER);
        CHECK(JBE_BUF(&e)->n_lines == 2 && strcmp(JBE_BUF(&e)->lines[0], "abc") == 0
              && strcmp(JBE_BUF(&e)->lines[1], "def") == 0, "Enter split into 'abc'/'def'");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(JBE_BUF(&e)->n_lines == 1 && strcmp(JBE_BUF(&e)->lines[0], "abcdef") == 0,
              "undo of Enter rejoins line");
        CHECK(JBE_PANE(&e)->cur_row == 0 && JBE_PANE(&e)->cur_col == 3, "cursor restored to split point");
        jbe_free(&e);

        /* 4. Backspace at line start (JOIN) + undo. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", "ab\ncd\n"), "write 'ab\\ncd'");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load 'ab\\ncd'");
        JBE_PANE(&e)->cur_row = 1; JBE_PANE(&e)->cur_col = 0;
        jbe_handle_key(&e, JAPI_KEY_BACKSPACE);
        CHECK(JBE_BUF(&e)->n_lines == 1 && strcmp(JBE_BUF(&e)->lines[0], "abcd") == 0,
              "Backspace joined into 'abcd'");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(JBE_BUF(&e)->n_lines == 2 && strcmp(JBE_BUF(&e)->lines[0], "ab") == 0
              && strcmp(JBE_BUF(&e)->lines[1], "cd") == 0, "undo of join restores split");
        CHECK(JBE_PANE(&e)->cur_row == 1 && JBE_PANE(&e)->cur_col == 0, "cursor at restored line start");
        jbe_free(&e);

        /* 5. Forward Delete + undo (mid-line and at end-of-line/join). */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", "ab\ncd\n"), "write 'ab\\ncd' #5");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load #5");
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 0;
        jbe_handle_key(&e, JAPI_KEY_DELETE);    /* delete 'a' */
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "b") == 0, "Delete removed 'a'");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "ab") == 0, "undo restored 'a'");
        JBE_PANE(&e)->cur_col = 2;                          /* end of 'ab' -> Delete joins */
        jbe_handle_key(&e, JAPI_KEY_DELETE);
        CHECK(JBE_BUF(&e)->n_lines == 1 && strcmp(JBE_BUF(&e)->lines[0], "abcd") == 0,
              "Delete at EOL joined lines");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(JBE_BUF(&e)->n_lines == 2 && strcmp(JBE_BUF(&e)->lines[1], "cd") == 0,
              "undo restored the split");
        jbe_free(&e);

        /* 6. Stream selection-delete + undo restores everything in one step. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", "hello world\nsecond line\n"),
              "write 2-line for sel-del");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load for sel-del");
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 6;
        jbe_handle_key(&e, JAPI_KEY_SDOWN);
        jbe_handle_key(&e, JAPI_KEY_SEND);     /* selection: "world\nsecond line" */
        jbe_handle_key(&e, JAPI_KEY_BACKSPACE); /* over-selection BS = delete sel */
        CHECK(JBE_BUF(&e)->n_lines == 1 && strcmp(JBE_BUF(&e)->lines[0], "hello ") == 0,
              "selection deleted");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(JBE_BUF(&e)->n_lines == 2 && strcmp(JBE_BUF(&e)->lines[0], "hello world") == 0
              && strcmp(JBE_BUF(&e)->lines[1], "second line") == 0,
              "one Ctrl+Z restores multi-line selection");
        jbe_free(&e);

        /* 7. Block selection-delete + undo. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", "AAA\nBBB\nCCC\n"), "write 3x3");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load 3x3");
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 1;
        jbe_handle_key(&e, JAPI_KEY_CSRIGHT);
        jbe_handle_key(&e, JAPI_KEY_CSDOWN);
        jbe_handle_key(&e, JAPI_KEY_CSDOWN);   /* block: middle column over 3 rows */
        jbe_handle_key(&e, JAPI_KEY_CTRL('X'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "AA") == 0 && strcmp(JBE_BUF(&e)->lines[1], "BB") == 0
              && strcmp(JBE_BUF(&e)->lines[2], "CC") == 0, "block cut middle column");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "AAA") == 0 && strcmp(JBE_BUF(&e)->lines[1], "BBB") == 0
              && strcmp(JBE_BUF(&e)->lines[2], "CCC") == 0, "undo restores block column");
        jbe_free(&e);

        /* 8. Stream paste + undo. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", "hello\n"), "write 'hello'");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load 'hello'");
        free(e.clip);
        e.clip = malloc(6); memcpy(e.clip, "XX\nYY", 5); e.clip[5] = 0;
        e.clip_len = 5; e.clip_block = false;
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 5;
        jbe_handle_key(&e, JAPI_KEY_CTRL('V'));
        CHECK(JBE_BUF(&e)->n_lines == 2 && strcmp(JBE_BUF(&e)->lines[0], "helloXX") == 0
              && strcmp(JBE_BUF(&e)->lines[1], "YY") == 0, "stream paste applied");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(JBE_BUF(&e)->n_lines == 1 && strcmp(JBE_BUF(&e)->lines[0], "hello") == 0,
              "undo restores pre-paste");
        jbe_free(&e);

        /* 9. Block paste + undo (within existing rows). */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", "1234\n5678\n9000\n"), "write 3-num");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load 3-num");
        free(e.clip);
        e.clip = malloc(6); memcpy(e.clip, "A\nB\nC", 5); e.clip[5] = 0;
        e.clip_len = 5; e.clip_block = true;
        JBE_PANE(&e)->cur_row = 0; JBE_PANE(&e)->cur_col = 2;
        jbe_handle_key(&e, JAPI_KEY_CTRL('V'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "12A34") == 0 && strcmp(JBE_BUF(&e)->lines[1], "56B78") == 0
              && strcmp(JBE_BUF(&e)->lines[2], "90C00") == 0, "block paste inserted column");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "1234") == 0 && strcmp(JBE_BUF(&e)->lines[1], "5678") == 0
              && strcmp(JBE_BUF(&e)->lines[2], "9000") == 0, "undo removes block column");
        jbe_free(&e);

        /* 10. Replace + undo: two Ctrl+Z steps restore original line. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", "alpha beta alpha\n"), "write for replace");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load for replace");
        /* Open Replace, type the query (incremental highlight), Tab to the
           "with" field, type the replacement, then Enter replaces the current
           match and advances; Esc closes. */
        jbe_handle_key(&e, JAPI_KEY_CTRL('R'));
        const char *q = "alpha"; for (int i=0;q[i];i++) jbe_handle_key(&e,(uint16_t)q[i]);
        jbe_handle_key(&e, JAPI_KEY_TAB);     /* focus the "with" field */
        const char *w = "ZZZ";   for (int i=0;w[i];i++) jbe_handle_key(&e,(uint16_t)w[i]);
        jbe_handle_key(&e, JAPI_KEY_ENTER);   /* replace the current match */
        jbe_handle_key(&e, JAPI_KEY_ESCAPE);  /* exit replace mode */
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "ZZZ beta alpha") == 0, "replace applied once");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));   /* undo INSERT 'ZZZ' */
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));   /* undo DELETE 'alpha' */
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "alpha beta alpha") == 0,
              "two Ctrl+Z restore original after Replace");
        jbe_free(&e);

        /* 11. Redo round-trip. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", ""), "write empty redo");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load empty redo");
        for (int i = 0; i < 3; i++) jbe_handle_key(&e, (uint16_t)("abc"[i]));
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));     /* "abc" -> "" (one word) */
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "") == 0, "after Ctrl+Z: empty");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Y'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "abc") == 0, "Ctrl+Y restored 'abc'");
        jbe_free(&e);

        /* 12. New edit clears the redo future. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", ""), "write empty for redo-clear");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load for redo-clear");
        jbe_handle_key(&e, 'a'); jbe_handle_key(&e, 'b');
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));   /* undo word -> "" */
        CHECK(JBE_BUF(&e)->redo.count > 0, "redo populated after undo");
        jbe_handle_key(&e, 'z');                   /* new edit clears redo */
        CHECK(JBE_BUF(&e)->redo.count == 0, "new edit cleared redo stack");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Y'));   /* should be no-op */
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "z") == 0, "Ctrl+Y after cleared redo is no-op");
        jbe_free(&e);

        /* 13. Budget eviction keeps combined payload bounded. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", ""), "write empty for budget");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load for budget");
        /* Type lots of distinct words separated by punctuation so each gets
           its own non-coalescable record. 5000 words of length ~8 = ~40 KB
           of payload, definitely above the 32 KB budget. */
        for (int w = 0; w < 5000; w++) {
            char tok[16];
            int n = snprintf(tok, sizeof tok, "a%dx;", w);
            for (int i = 0; i < n; i++) jbe_handle_key(&e, (uint16_t)tok[i]);
        }
        CHECK(JBE_BUF(&e)->undo.bytes_used + JBE_BUF(&e)->redo.bytes_used <= JBE_UNDO_BUDGET_BYTES,
              "combined payload stays under budget");
        jbe_free(&e);

        /* 14. Coalescing breaks on cursor movement. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", ""), "write for coalesce-break");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load for coalesce-break");
        jbe_handle_key(&e, 'a'); jbe_handle_key(&e, 'b');
        jbe_handle_key(&e, JAPI_KEY_LEFT);    /* cursor move */
        jbe_handle_key(&e, JAPI_KEY_RIGHT);   /* back to (0,2) */
        jbe_handle_key(&e, 'c');              /* should NOT coalesce with "ab" */
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "abc") == 0, "typed 'abc' (after cursor wiggle)");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "ab") == 0, "Ctrl+Z removes only the 'c'");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(strcmp(JBE_BUF(&e)->lines[0], "") == 0, "Ctrl+Z removes 'ab'");
        jbe_free(&e);

        /* 15. Dirty-flag returns to false on undo back to saved state. */
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_u.txt", "x\n"), "write 'x' for dirty test");
        CHECK(jbe_load(&e, "A:jbe_u.txt"), "load 'x'");
        CHECK(!JBE_BUF(&e)->dirty, "dirty=false after load");
        JBE_PANE(&e)->cur_col = 1;
        jbe_handle_key(&e, 'y');
        CHECK(JBE_BUF(&e)->dirty, "dirty=true after type");
        jbe_handle_key(&e, JAPI_KEY_CTRL('S'));
        if (e.save_request) { e.save_request = false; jbe_save(&e); }  /* loop does the write */
        CHECK(!JBE_BUF(&e)->dirty, "dirty=false after save");
        jbe_handle_key(&e, 'z');
        CHECK(JBE_BUF(&e)->dirty, "dirty=true after edit past save");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(!JBE_BUF(&e)->dirty, "dirty=false: undo back to saved state");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(JBE_BUF(&e)->dirty, "dirty=true: undo past saved state");
        jbe_handle_key(&e, JAPI_KEY_CTRL('Y'));
        CHECK(!JBE_BUF(&e)->dirty, "dirty=false: redo back to saved state");
        jbe_free(&e);

        japi_remove("A:jbe_u.txt");
    }

    /* ----- Step 11: split-screen (shared views + per-pane files) ------ */
    {
        jbe_state_t ed;
        jbe_init(&ed);
        CHECK(make_fixture("A:jbe_sp0.txt", "alpha\nbeta\n"),   "split: fixture 0");
        CHECK(jbe_load(&ed, "A:jbe_sp0.txt"),                   "split: load pane 0");
        CHECK(!ed.split_active,                                 "split: starts off");

        /* Open the split (Window→Split): both panes start on the SAME file,
           two views of one buffer. Focus stays on pane 0. */
        jbe_handle_key(&ed, JAPI_KEY_ALT('V'));
        jbe_handle_key(&ed, JAPI_KEY_ALT('W'));
        CHECK(ed.split_active,                                  "split: Alt+V then Alt+W opens");
        CHECK(ed.active_pane == 0,                              "split: focus stays in pane 0");
        CHECK(ed.panes[1].buf_idx == ed.panes[0].buf_idx,      "split: both panes share one buffer");

        /* A shared edit shows through both views (same buffer). */
        JBE_PANE(&ed)->cur_row = 0; JBE_PANE(&ed)->cur_col = 0;
        jbe_handle_key(&ed, 'Z');
        CHECK(strcmp(ed.buffers[0].lines[0], "Zalpha") == 0,    "split: edit in shared buffer");
        ed.active_pane = 1;
        CHECK(strcmp(JBE_BUF(&ed)->lines[0], "Zalpha") == 0,    "split: pane 1 sees the same edit");

        /* Now give pane 1 its own file via File->New: it must detach to the
           free slot and leave pane 0's buffer untouched. */
        jbe_new(&ed);
        CHECK(ed.panes[1].buf_idx != ed.panes[0].buf_idx,      "split: New detaches pane 1");
        CHECK(strcmp(JBE_BUF(&ed)->filename, "(untitled)") == 0,"split: pane 1 is a fresh file");
        jbe_handle_key(&ed, 'X');
        CHECK(strcmp(JBE_BUF(&ed)->lines[0], "X") == 0,         "split: edit lands in pane 1's file");
        CHECK(strcmp(ed.buffers[ed.panes[0].buf_idx].lines[0], "Zalpha") == 0,
                                                                "split: pane 0 file untouched");

        /* File->Close on pane 1 (its own dirty file) asks first, then EMPTIES
           the pane without removing it: the split stays, pane 0 stays. */
        jbe_handle_key(&ed, JAPI_KEY_CTRL('W'));
        CHECK(ed.close_confirm && ed.confirm_action == JBE_CONFIRM_CLOSE,      "split: Close dirty asks first");
        jbe_handle_key(&ed, 'Y');
        CHECK(ed.close_request,                                "split: Close confirmed");
        ed.close_request = false; jbe_close_active(&ed);
        CHECK(ed.split_active && ed.active_pane == 1,          "split: Close keeps pane + split");
        CHECK(strcmp(JBE_BUF(&ed)->filename, "(untitled)") == 0,"split: pane 1 now empty untitled");
        CHECK(JBE_BUF(&ed)->n_lines == 1 && JBE_BUF(&ed)->len[0] == 0,
                                                                "split: pane 1 is blank");
        CHECK(strcmp(ed.buffers[ed.panes[0].buf_idx].lines[0], "Zalpha") == 0,
                                                                "split: pane 0 still its file");

        /* Un-split via Window->Split: keep the active pane (pane 1, blank+clean)
           and drop pane 0. Pane 0 is dirty ("Zalpha"), so it asks first. */
        jbe_handle_key(&ed, JAPI_KEY_ALT('V'));
        jbe_handle_key(&ed, JAPI_KEY_ALT('W'));
        CHECK(ed.close_confirm && ed.confirm_action == JBE_CONFIRM_UNSPLIT,       "split: un-split dirty pane asks");
        jbe_handle_key(&ed, 'n');
        CHECK(!ed.close_confirm && ed.split_active,            "split: 'n' keeps the split open");

        /* Confirm the un-split: collapses to one pane keeping pane 1's blank
           doc (the un-split runs immediately on 'Y', no request round-trip). */
        jbe_handle_key(&ed, JAPI_KEY_ALT('V'));
        jbe_handle_key(&ed, JAPI_KEY_ALT('W'));
        jbe_handle_key(&ed, 'Y');
        CHECK(!ed.split_active && ed.active_pane == 0,         "split: un-split collapses to one pane");
        CHECK(ed.panes[0].buf_idx == 0,                        "split: survivor lives in slot 0");
        CHECK(strcmp(JBE_BUF(&ed)->filename, "(untitled)") == 0,"split: kept the active (blank) pane");

        /* Ctrl+Tab is a no-op when there is no split open. */
        jbe_handle_key(&ed, JAPI_KEY_CTAB);
        CHECK(ed.active_pane == 0,                              "split: Ctrl+Tab idle without split");

        /* Re-open the split (shared views, both clean) for the divider tests;
           render must not crash with it open. */
        jbe_handle_key(&ed, JAPI_KEY_ALT('V'));
        jbe_handle_key(&ed, JAPI_KEY_ALT('W'));
        jbe_render(&ed);
        CHECK(ed.split_active,                                  "split: re-opened for resize tests");

        /* Divider resize via Ctrl+Up / Ctrl+Down. split_row is at its initial
           mid-screen default. */
        int start_row = ed.split_row;
        jbe_handle_key(&ed, JAPI_KEY_CUP);
        CHECK(ed.split_row == start_row - 1,                    "resize: Ctrl+Up moves divider up");
        jbe_handle_key(&ed, JAPI_KEY_CDOWN);
        jbe_handle_key(&ed, JAPI_KEY_CDOWN);
        CHECK(ed.split_row == start_row + 1,                    "resize: Ctrl+Down moves divider down");

        /* Clamp at the top edge: enough Ctrl+Up presses must not push the
           divider above JBE_VIEW_TOP+1. */
        for (int i = 0; i < 100; i++) jbe_handle_key(&ed, JAPI_KEY_CUP);
        CHECK(ed.split_row == JBE_VIEW_TOP + 1,                 "resize: clamps at top");

        /* Clamp at the bottom edge. */
        for (int i = 0; i < 100; i++) jbe_handle_key(&ed, JAPI_KEY_CDOWN);
        CHECK(ed.split_row == JBE_VIEW_BOTTOM - 1,              "resize: clamps at bottom");

        /* Menu route does the same thing, via the dropdown accelerators
           'U' (Move divider up) and 'D' (Move divider down) in the View menu. */
        ed.split_row = start_row;
        jbe_handle_key(&ed, JAPI_KEY_ALT('V'));
        jbe_handle_key(&ed, JAPI_KEY_ALT('U'));
        CHECK(ed.split_row == start_row - 1,                    "resize: menu Move divider up");
        jbe_handle_key(&ed, JAPI_KEY_ALT('V'));
        jbe_handle_key(&ed, JAPI_KEY_ALT('D'));
        CHECK(ed.split_row == start_row,                        "resize: menu Move divider down");

        /* Un-split (panes share one clean buffer, so no confirm and it happens
           at once) — resize is then a no-op. */
        jbe_handle_key(&ed, JAPI_KEY_ALT('V'));
        jbe_handle_key(&ed, JAPI_KEY_ALT('W'));
        CHECK(!ed.split_active,                                 "resize: split closed");
        int idle_row = ed.split_row;
        jbe_handle_key(&ed, JAPI_KEY_CUP);
        jbe_handle_key(&ed, JAPI_KEY_CDOWN);
        CHECK(ed.split_row == idle_row,                         "resize: idle without split");

        jbe_free(&ed);
        japi_remove("A:jbe_sp0.txt");
    }

    /* ----- Step 12: undo of block paste that grew/padded the buffer --- */
    {
        /* Scenario A: paste GROWS the buffer past EOF (no padding —
           start_col == 0 so short rows don't need spaces). */
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_up.txt", "alpha\nbeta\n"), "padfix A: fixture");
        CHECK(jbe_load(&e, "A:jbe_up.txt"),                  "padfix A: load");
        int  n_before_A = JBE_BUF(&e)->n_lines;          /* 2 */
        char l0_A[32]; strcpy(l0_A, JBE_BUF(&e)->lines[0]);
        char l1_A[32]; strcpy(l1_A, JBE_BUF(&e)->lines[1]);

        free(e.clip);
        e.clip = malloc(8); memcpy(e.clip, "X\nY\nZ\nW", 7); e.clip[7] = 0;
        e.clip_len = 7; e.clip_block = true;
        JBE_PANE(&e)->cur_row = 1; JBE_PANE(&e)->cur_col = 0;   /* col 0 → no padding */
        jbe_handle_key(&e, JAPI_KEY_CTRL('V'));
        CHECK(JBE_BUF(&e)->n_lines == 5,                "padfix A: paste added 3 rows");
        CHECK(strcmp(JBE_BUF(&e)->lines[1], "Xbeta") == 0, "padfix A: row 1 inserted X");
        CHECK(strcmp(JBE_BUF(&e)->lines[2], "Y") == 0,     "padfix A: grown row 2");
        CHECK(strcmp(JBE_BUF(&e)->lines[4], "W") == 0,     "padfix A: grown row 4");

        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(JBE_BUF(&e)->n_lines == n_before_A,       "padfix A: undo restored row count");
        CHECK(strcmp(JBE_BUF(&e)->lines[0], l0_A) == 0, "padfix A: undo restored row 0");
        CHECK(strcmp(JBE_BUF(&e)->lines[1], l1_A) == 0, "padfix A: undo restored row 1");

        jbe_free(&e);
        japi_remove("A:jbe_up.txt");
    }
    {
        /* Scenario B: paste PADS short rows (all targets exist, no grow). */
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_up.txt",
                           "longer line zero\n"
                           "x\n"
                           "longer line two\n"),
              "padfix B: fixture");
        CHECK(jbe_load(&e, "A:jbe_up.txt"),                  "padfix B: load");
        int  n_before_B = JBE_BUF(&e)->n_lines;          /* 3 */
        char l0_B[32]; strcpy(l0_B, JBE_BUF(&e)->lines[0]);
        char l1_B[32]; strcpy(l1_B, JBE_BUF(&e)->lines[1]);
        char l2_B[32]; strcpy(l2_B, JBE_BUF(&e)->lines[2]);

        free(e.clip);
        e.clip = malloc(4); memcpy(e.clip, "P\nQ", 3); e.clip[3] = 0;
        e.clip_len = 3; e.clip_block = true;
        /* Row 1 = "x" (len 1); col 5 → 4 padding spaces, then 'P'.
           Row 2 = "longer line two" (len 15) → no padding, 'Q' inserted at col 5. */
        JBE_PANE(&e)->cur_row = 1; JBE_PANE(&e)->cur_col = 5;
        jbe_handle_key(&e, JAPI_KEY_CTRL('V'));
        CHECK(JBE_BUF(&e)->n_lines == n_before_B,        "padfix B: no rows added");
        CHECK(strcmp(JBE_BUF(&e)->lines[1], "x    P") == 0, "padfix B: row 1 padded + P");

        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(JBE_BUF(&e)->n_lines == n_before_B,       "padfix B: undo kept row count");
        CHECK(strcmp(JBE_BUF(&e)->lines[0], l0_B) == 0, "padfix B: row 0 untouched");
        CHECK(strcmp(JBE_BUF(&e)->lines[1], l1_B) == 0, "padfix B: padding stripped from row 1");
        CHECK(strcmp(JBE_BUF(&e)->lines[2], l2_B) == 0, "padfix B: Q removed from row 2");

        jbe_free(&e);
        japi_remove("A:jbe_up.txt");
    }
    {
        /* Scenario C: paste BOTH pads a short row AND grows past EOF. */
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_up.txt",
                           "AAAAA\n"      /* len 5 */
                           "B\n"          /* len 1 — will be padded */
                           "CC\n"),       /* len 2 — will be padded */
              "padfix C: fixture");
        CHECK(jbe_load(&e, "A:jbe_up.txt"),                  "padfix C: load");
        int  n_before_C = JBE_BUF(&e)->n_lines;          /* 3 */
        char l0_C[32]; strcpy(l0_C, JBE_BUF(&e)->lines[0]);
        char l1_C[32]; strcpy(l1_C, JBE_BUF(&e)->lines[1]);
        char l2_C[32]; strcpy(l2_C, JBE_BUF(&e)->lines[2]);

        free(e.clip);
        e.clip = malloc(8); memcpy(e.clip, "1\n2\n3\n4", 7); e.clip[7] = 0;
        e.clip_len = 7; e.clip_block = true;
        /* Paste at row 1, col 4. Seg 0 → row 1 (padded), seg 1 → row 2
           (padded), seg 2 → row 3 (grown), seg 3 → row 4 (grown). */
        JBE_PANE(&e)->cur_row = 1; JBE_PANE(&e)->cur_col = 4;
        jbe_handle_key(&e, JAPI_KEY_CTRL('V'));
        CHECK(JBE_BUF(&e)->n_lines == 5,                "padfix C: 2 rows grown");
        CHECK(strcmp(JBE_BUF(&e)->lines[1], "B   1") == 0, "padfix C: row 1 padded + 1");
        CHECK(strcmp(JBE_BUF(&e)->lines[2], "CC  2") == 0, "padfix C: row 2 padded + 2");
        CHECK(strcmp(JBE_BUF(&e)->lines[3], "    3") == 0, "padfix C: grown row 3");
        CHECK(strcmp(JBE_BUF(&e)->lines[4], "    4") == 0, "padfix C: grown row 4");

        jbe_handle_key(&e, JAPI_KEY_CTRL('Z'));
        CHECK(JBE_BUF(&e)->n_lines == n_before_C,       "padfix C: undo restored row count");
        CHECK(strcmp(JBE_BUF(&e)->lines[0], l0_C) == 0, "padfix C: row 0 untouched");
        CHECK(strcmp(JBE_BUF(&e)->lines[1], l1_C) == 0, "padfix C: row 1 unpadded");
        CHECK(strcmp(JBE_BUF(&e)->lines[2], l2_C) == 0, "padfix C: row 2 unpadded");

        /* Redo: pad+grow must come back exactly. */
        jbe_handle_key(&e, JAPI_KEY_CTRL('Y'));
        CHECK(JBE_BUF(&e)->n_lines == 5,                "padfix C: redo regrew");
        CHECK(strcmp(JBE_BUF(&e)->lines[1], "B   1") == 0, "padfix C: redo row 1");
        CHECK(strcmp(JBE_BUF(&e)->lines[4], "    4") == 0, "padfix C: redo grown row 4");

        jbe_free(&e);
        japi_remove("A:jbe_up.txt");
    }

    /* ----- Step 13: .syn parser ---------------------------------------- */
    {
        /* Tiny helper: count entries in a NULL-terminated string list. */
        #define LIST_LEN(L) ({ int _n = 0; if (L) while ((L)[_n]) _n++; _n; })

        /* Z80-equivalent text. Every field of the returned scheme should
           match the values the built-in Z80_SCHEME uses. */
        static const char Z80_SYN_TEXT[] =
            "# Z80 scheme, equivalent to the built-in\n"
            "name=Z80\n"
            "flavor=z80\n"
            "extensions=.z80,.asm,.s\n"
            "comment=;\n"
            "keywords=ADC ADD AND BIT CALL CCF CP CPD CPDR CPI CPIR CPL\n"
            "directives=ORG DB DW\n"
            "registers=A B C D E H L\n"
            "color_default=white\n"
            "color_keyword=white\n"
            "color_register=magenta\n"
            "color_directive=cyan\n"
            "color_number=yellow\n"
            "color_string=cyan\n"
            "color_comment=green\n"
            "color_label=orange\n";

        jbe_syn_scheme_t *p = jbe_syn_scheme_parse(Z80_SYN_TEXT, (int)sizeof Z80_SYN_TEXT - 1);
        CHECK(p != 0,                                       "parse: scheme returned");
        CHECK(p && p->_owns_storage,                        "parse: owns its storage");
        CHECK(p && p->name && strcmp(p->name, "Z80") == 0,  "parse: name=Z80");
        CHECK(p && p->flavor == JBE_SYN_FLAVOR_Z80,         "parse: flavor=z80");
        CHECK(p && p->comment_chars && strcmp(p->comment_chars, ";") == 0,
                                                            "parse: comment=;");
        CHECK(p && LIST_LEN(p->extensions) == 3,            "parse: 3 extensions");
        CHECK(p && p->extensions && strcmp(p->extensions[0], ".z80") == 0,
                                                            "parse: ext[0]=.z80");
        CHECK(p && p->extensions && strcmp(p->extensions[1], ".asm") == 0,
                                                            "parse: ext[1]=.asm");
        CHECK(p && p->extensions && strcmp(p->extensions[2], ".s") == 0,
                                                            "parse: ext[2]=.s");
        CHECK(p && LIST_LEN(p->keywords) == 12,             "parse: 12 keywords");
        CHECK(p && p->keywords && strcmp(p->keywords[0], "ADC") == 0,
                                                            "parse: kw[0]=ADC");
        CHECK(p && LIST_LEN(p->directives) == 3,            "parse: 3 directives");
        CHECK(p && LIST_LEN(p->registers) == 7,             "parse: 7 registers");
        CHECK(p && p->color_default == VGA_WHITE,           "parse: color_default=white");
        CHECK(p && p->color_register == VGA_MAGENTA,        "parse: color_register=magenta");
        CHECK(p && p->color_directive == VGA_CYAN,          "parse: color_directive=cyan");
        CHECK(p && p->color_number == VGA_YELLOW,           "parse: color_number=yellow");
        CHECK(p && p->color_comment == VGA_GREEN,           "parse: color_comment=green");
        CHECK(p && p->color_label == 0x3A,                  "parse: color_label=orange");
        jbe_syn_scheme_free(p);

        /* Comments, blank lines, whitespace around '=', missing optional
           lists should all parse cleanly. */
        static const char SPARSE_TEXT[] =
            "\n"
            "# only the required keys\n"
            "  name  =  Mini  \n"
            "flavor=z80\n"
            "\n";
        p = jbe_syn_scheme_parse(SPARSE_TEXT, (int)sizeof SPARSE_TEXT - 1);
        CHECK(p != 0,                                       "parse: sparse accepted");
        CHECK(p && p->name && strcmp(p->name, "Mini") == 0, "parse: name trimmed");
        CHECK(p && p->extensions == 0,                      "parse: no extensions OK");
        CHECK(p && p->keywords == 0,                        "parse: no keywords OK");
        jbe_syn_scheme_free(p);

        /* Missing name → parser refuses. */
        static const char NO_NAME[] = "flavor=z80\n";
        p = jbe_syn_scheme_parse(NO_NAME, (int)sizeof NO_NAME - 1);
        CHECK(p == 0,                                       "parse: rejects missing name");

        /* Missing flavor (or flavor=none) → parser refuses too. */
        static const char NO_FLAVOR[] = "name=NoFlavor\n";
        p = jbe_syn_scheme_parse(NO_FLAVOR, (int)sizeof NO_FLAVOR - 1);
        CHECK(p == 0,                                       "parse: rejects missing flavor");

        /* Unknown colour name silently defaults to white (forward-compat). */
        static const char BAD_COLOR[] =
            "name=BC\nflavor=z80\ncolor_keyword=heliotrope\n";
        p = jbe_syn_scheme_parse(BAD_COLOR, (int)sizeof BAD_COLOR - 1);
        CHECK(p && p->color_keyword == VGA_WHITE,           "parse: unknown colour → white");
        jbe_syn_scheme_free(p);

        /* NULL / zero-length input is safe. */
        CHECK(jbe_syn_scheme_parse(0, 10) == 0,             "parse: NULL text rejected");
        CHECK(jbe_syn_scheme_parse("name=X", 0) == 0,       "parse: zero len rejected");
        jbe_syn_scheme_free(0);   /* NULL-safe free, must not crash */

        #undef LIST_LEN
    }

    /* ----- Step 14: floppy override of built-in scheme ---------------- */
    {
        /* Without an override on the floppy, opening a .z80 file should
           give the built-in Z80 scheme. (color_label is 0x3A orange.) */
        japi_remove("C:config/syntax/z80.syn");      /* defensive — make sure no override exists */
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:plain.z80", "  ld a,1\n"),    "lookup: write fixture");
        CHECK(jbe_load(&e, "A:plain.z80"),                  "lookup: load .z80");
        CHECK(strcmp(JBE_BUF(&e)->syntax_name, "Z80") == 0,           "lookup: syntax = Z80");
        CHECK(JBE_BUF(&e)->active_scheme != 0,              "lookup: scheme installed");
        CHECK(JBE_BUF(&e)->active_scheme &&
              !JBE_BUF(&e)->active_scheme->_owns_storage,   "lookup: built-in (not owned)");
        CHECK(JBE_BUF(&e)->active_scheme &&
              JBE_BUF(&e)->active_scheme->color_label == 0x3A, "lookup: built-in orange label");
        jbe_free(&e);

        /* Now drop an override on the floppy with a deliberately different
           color (label=red instead of orange). Reload and verify the
           override is picked up. */
        const char OVERRIDE[] =
            "name=Z80\n"
            "flavor=z80\n"
            "extensions=.z80\n"
            "comment=;\n"
            "color_label=red\n";
        japi_mkdir("C:config"); japi_mkdir("C:config/syntax");   /* idempotent; needed on a fresh simdisk */
        CHECK(make_fixture("C:config/syntax/z80.syn", OVERRIDE),   "lookup: write override");
        jbe_init(&e);
        CHECK(jbe_load(&e, "A:plain.z80"),                  "lookup: reload .z80");
        CHECK(JBE_BUF(&e)->active_scheme != 0,              "lookup: override installed");
        CHECK(JBE_BUF(&e)->active_scheme &&
              JBE_BUF(&e)->active_scheme->_owns_storage,    "lookup: override is owned");
        CHECK(JBE_BUF(&e)->active_scheme &&
              JBE_BUF(&e)->active_scheme->color_label == VGA_RED, "lookup: override red label");

        /* Switching to NONE via the menu clears the active scheme and
           frees the owned override. */
        jbe_handle_key(&e, JAPI_KEY_ALT('O'));   /* Options menu */
        jbe_handle_key(&e, JAPI_KEY_ALT('N'));   /* Syntax: None */
        CHECK(JBE_BUF(&e)->syntax_name[0] == 0,          "lookup: menu → None");
        CHECK(JBE_BUF(&e)->active_scheme == 0,              "lookup: scheme cleared");

        /* Switching back to Z80 via the menu picks up the override again. */
        jbe_handle_key(&e, JAPI_KEY_ALT('O'));
        jbe_handle_key(&e, JAPI_KEY_ALT('Z'));   /* Syntax: Z80 */
        CHECK(strcmp(JBE_BUF(&e)->syntax_name, "Z80") == 0,           "lookup: menu → Z80");
        CHECK(JBE_BUF(&e)->active_scheme &&
              JBE_BUF(&e)->active_scheme->color_label == VGA_RED, "lookup: menu re-picks override");

        /* Remove the override and reload → built-in returns. */
        japi_remove("C:config/syntax/z80.syn");
        jbe_free(&e);
        jbe_init(&e);
        CHECK(jbe_load(&e, "A:plain.z80"),                  "lookup: reload without override");
        CHECK(JBE_BUF(&e)->active_scheme &&
              !JBE_BUF(&e)->active_scheme->_owns_storage,   "lookup: back to built-in");
        CHECK(JBE_BUF(&e)->active_scheme &&
              JBE_BUF(&e)->active_scheme->color_label == 0x3A, "lookup: built-in orange again");
        jbe_free(&e);
        japi_remove("A:plain.z80");
    }

    /* ----- Step 15: Basic scheme + BASIC-flavor lexer --------------- */
    {
        /* Auto-detect from .bas extension installs the Basic scheme. */
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:demo.bas",
                           "10 PRINT \"hello\"\n"
                           "20 FOR I = 1 TO 10 : NEXT I\n"
                           "30 REM end of program\n"
                           "40 ' apostrophe comment\n"
                           "50 X = &H1F + 100\n"),
              "mmbasic: write .bas fixture");
        CHECK(jbe_load(&e, "A:demo.bas"),                   "mmbasic: load .bas");
        CHECK(strcmp(JBE_BUF(&e)->syntax_name, "Basic") == 0,       "mmbasic: syntax = Basic");
        CHECK(JBE_BUF(&e)->active_scheme != 0,              "mmbasic: scheme installed");
        CHECK(JBE_BUF(&e)->active_scheme &&
              JBE_BUF(&e)->active_scheme->flavor == JBE_SYN_FLAVOR_BASIC,
                                                            "mmbasic: flavor = BASIC");
        CHECK(JBE_BUF(&e)->active_scheme &&
              strcmp(JBE_BUF(&e)->active_scheme->name, "Basic") == 0,
                                                            "mmbasic: name correct");
        /* Floppy override should also work via the same machinery — same
           lookup path as Z80, just keyed on the lower-cased scheme name. */
        japi_mkdir("C:config"); japi_mkdir("C:config/syntax");
        const char OVERRIDE[] =
            "name=Basic\n"
            "flavor=basic\n"
            "extensions=.bas\n"
            "comment='\n"
            "keywords=PRINT INPUT FOR NEXT\n"
            "color_keyword=red\n";
        CHECK(make_fixture("C:config/syntax/basic.syn", OVERRIDE),
                                                            "mmbasic: write override");
        jbe_free(&e); jbe_init(&e);
        CHECK(jbe_load(&e, "A:demo.bas"),                   "mmbasic: reload .bas");
        CHECK(JBE_BUF(&e)->active_scheme &&
              JBE_BUF(&e)->active_scheme->_owns_storage,    "mmbasic: override owned");
        CHECK(JBE_BUF(&e)->active_scheme &&
              JBE_BUF(&e)->active_scheme->color_keyword == VGA_RED,
                                                            "mmbasic: override red keyword");

        /* Cleanup */
        japi_remove("C:config/syntax/basic.syn");
        jbe_free(&e);
        japi_remove("A:demo.bas");
    }

    /* ----- Step 16: dynamic Options→Syntax menu ----------------------- */
    {
        /* Clean slate on the simulated floppy. */
        japi_mkdir("C:config"); japi_mkdir("C:config/syntax");
        japi_remove("C:config/syntax/z80.syn");
        japi_remove("C:config/syntax/basic.syn");
        japi_remove("C:config/syntax/forth.syn");

        jbe_state_t e;
        jbe_init(&e);
        /* jbe_handle_key returns early on an empty buffer, so seed one
           line. We don't care about its content — only the menu state. */
        CHECK(make_fixture("A:dyn.txt", "x\n"),            "dyn: write seed");
        CHECK(jbe_load(&e, "A:dyn.txt"),                   "dyn: load seed");

        /* Open the Options menu — rebuild should give None + 2 built-ins. */
        jbe_handle_key(&e, JAPI_KEY_ALT('O'));
        CHECK(e.menu_active && e.menu_idx == 5,            "dyn: Options menu opened");
        CHECK(e.options_n == 4,                            "dyn: 4 items (None + 2 built-ins + CPU)");
        CHECK(strcmp(e.options_labels[0], "Syntax: None") == 0,    "dyn: item 0 = None");
        CHECK(strcmp(e.options_labels[1], "Syntax: Z80") == 0,     "dyn: item 1 = Z80");
        CHECK(strcmp(e.options_labels[2], "Syntax: Basic") == 0, "dyn: item 2 = Basic");
        CHECK(strcmp(e.options_labels[3], "CPU speed...") == 0,    "dyn: last item = CPU speed");
        CHECK(e.cpu_item_index == 3,                       "dyn: CPU item index tracked");
        CHECK(e.options_names[0][0] == 0,                  "dyn: name 0 empty");
        CHECK(strcmp(e.options_names[1], "Z80") == 0,      "dyn: name 1 = Z80");
        CHECK(strcmp(e.options_names[2], "Basic") == 0,  "dyn: name 2 = Basic");
        jbe_handle_key(&e, JAPI_KEY_ESCAPE);
        CHECK(!e.menu_active,                              "dyn: Esc closes menu");

        /* Drop a user-only scheme on the floppy (Forth). After menu open,
           it should appear as a 4th item. */
        CHECK(make_fixture("C:config/syntax/forth.syn",
                           "name=Forth\nflavor=z80\nextensions=.fs\nkeywords=DUP DROP OVER\n"),
              "dyn: write forth.syn");
        jbe_handle_key(&e, JAPI_KEY_ALT('O'));
        CHECK(e.options_n == 5,                            "dyn: Forth added (5 items incl CPU)");
        CHECK(strcmp(e.options_labels[3], "Syntax: Forth") == 0,  "dyn: item 3 = Forth");
        CHECK(strcmp(e.options_names[3], "Forth") == 0,    "dyn: name 3 = Forth");

        /* Selecting Forth via its accelerator installs the user-only
           scheme on the active buffer. */
        jbe_handle_key(&e, JAPI_KEY_ALT('F'));
        CHECK(strcmp(JBE_BUF(&e)->syntax_name, "Forth") == 0,
                                                           "dyn: buffer uses Forth");
        CHECK(JBE_BUF(&e)->active_scheme &&
              JBE_BUF(&e)->active_scheme->_owns_storage,   "dyn: Forth scheme is owned");
        CHECK(JBE_BUF(&e)->active_scheme &&
              strcmp(JBE_BUF(&e)->active_scheme->name, "Forth") == 0,
                                                           "dyn: active_scheme name = Forth");

        /* Now drop a Z80 override on the floppy with name=Z80. Item count
           should stay 4 (override collapses with built-in). */
        CHECK(make_fixture("C:config/syntax/z80.syn",
                           "name=Z80\nflavor=z80\nextensions=.z80\ncolor_label=red\n"),
              "dyn: write Z80 override");
        jbe_handle_key(&e, JAPI_KEY_ALT('O'));
        CHECK(e.options_n == 5,                            "dyn: override collapses (5 items incl CPU)");
        /* Built-in Z80 entry now resolves to the override (red label). */
        jbe_handle_key(&e, JAPI_KEY_ALT('Z'));
        CHECK(strcmp(JBE_BUF(&e)->syntax_name, "Z80") == 0,         "dyn: buffer = Z80");
        CHECK(JBE_BUF(&e)->active_scheme &&
              JBE_BUF(&e)->active_scheme->color_label == VGA_RED,
                                                           "dyn: Z80 picks up override colour");

        japi_remove("C:config/syntax/z80.syn");
        japi_remove("C:config/syntax/forth.syn");
        japi_remove("A:dyn.txt");
        jbe_free(&e);
    }

    /* ----- Step 17: auto-detect from user .syn extensions ------------- */
    {
        japi_mkdir("C:config"); japi_mkdir("C:config/syntax");
        japi_remove("C:config/syntax/z80.syn");
        japi_remove("C:config/syntax/forth.syn");
        japi_remove("C:config/syntax/wrong.syn");

        /* Without a user scheme, .fs has no auto-detect — built-ins only. */
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:hello.fs", ": HELLO ;\n"),    "auto: write .fs fixture");
        CHECK(jbe_load(&e, "A:hello.fs"),                   "auto: load .fs");
        CHECK(JBE_BUF(&e)->syntax_name[0] == 0,             "auto: no built-in for .fs");
        CHECK(JBE_BUF(&e)->active_scheme == 0,              "auto: no scheme active");
        jbe_free(&e);

        /* Drop a Forth scheme that claims .fs and .4th. Reload —
           auto-detect picks up the user scheme. */
        CHECK(make_fixture("C:config/syntax/forth.syn",
                           "name=Forth\n"
                           "flavor=z80\n"
                           "extensions=.fs,.4th\n"
                           "keywords=DUP DROP OVER SWAP\n"),
                                                            "auto: write forth.syn");
        jbe_init(&e);
        CHECK(jbe_load(&e, "A:hello.fs"),                   "auto: reload .fs");
        CHECK(strcmp(JBE_BUF(&e)->syntax_name, "Forth") == 0,
                                                            "auto: .fs → Forth (user)");
        CHECK(JBE_BUF(&e)->active_scheme &&
              JBE_BUF(&e)->active_scheme->_owns_storage,    "auto: parsed scheme installed");

        /* Second extension from the same file (.4th) also matches. */
        jbe_free(&e); jbe_init(&e);
        CHECK(make_fixture("A:other.4th", ": MAIN ;\n"),    "auto: write .4th fixture");
        CHECK(jbe_load(&e, "A:other.4th"),                  "auto: load .4th");
        CHECK(strcmp(JBE_BUF(&e)->syntax_name, "Forth") == 0,
                                                            "auto: .4th → Forth too");

        /* Built-ins still win: even if a user scheme claims .z80, the
           Z80 built-in matches first. */
        CHECK(make_fixture("C:config/syntax/wrong.syn",
                           "name=Wrong\nflavor=z80\nextensions=.z80\n"),
                                                            "auto: write wrong.syn");
        jbe_free(&e); jbe_init(&e);
        CHECK(make_fixture("A:t.z80", "  ld a,1\n"),        "auto: write t.z80");
        CHECK(jbe_load(&e, "A:t.z80"),                      "auto: load t.z80");
        CHECK(strcmp(JBE_BUF(&e)->syntax_name, "Z80") == 0, "auto: built-in Z80 wins");

        japi_remove("C:config/syntax/forth.syn");
        japi_remove("C:config/syntax/wrong.syn");
        japi_remove("A:hello.fs");
        japi_remove("A:other.4th");
        japi_remove("A:t.z80");
        jbe_free(&e);
    }

    /* ----- MVP step 18: keyboard macros (Ctrl+T record / Ctrl+P play) - */
    {
        jbe_state_t m; jbe_init(&m);
        CHECK(make_fixture("A:mac.txt", "\n"),     "macro: write empty fixture");
        CHECK(jbe_load(&m, "A:mac.txt"),           "macro: load fixture");

        /* Replay with nothing recorded is a no-op (does not crash). */
        jbe_handle_key(&m, JAPI_KEY_CTRL('P'));
        CHECK(JBE_BUF(&m)->n_lines == 1 && JBE_BUF(&m)->len[0] == 0,
              "macro: Ctrl+P on empty macro is no-op");

        /* Record: Ctrl+T, type 'hi', Ctrl+T. */
        jbe_handle_key(&m, JAPI_KEY_CTRL('T'));
        CHECK(m.macro_recording,                   "macro: recording started");
        CHECK(m.macro_len == 0,                    "macro: buffer reset on record");
        jbe_handle_key(&m, 'h');
        jbe_handle_key(&m, 'i');
        jbe_handle_key(&m, JAPI_KEY_CTRL('T'));
        CHECK(!m.macro_recording,                  "macro: recording stopped");
        CHECK(m.macro_len == 2,                    "macro: 2 keys captured (no Ctrl+T)");
        CHECK(m.macro_buf[0] == 'h' && m.macro_buf[1] == 'i',
              "macro: stored keys are 'h','i'");
        CHECK(strcmp(JBE_BUF(&m)->lines[0], "hi") == 0,
              "macro: keys typed live during record");

        /* Replay: cursor back to start, then Ctrl+P prepends another "hi". */
        jbe_handle_key(&m, JAPI_KEY_HOME);
        jbe_handle_key(&m, JAPI_KEY_CTRL('P'));
        CHECK(strcmp(JBE_BUF(&m)->lines[0], "hihi") == 0,
              "macro: replay re-typed 'hi' at cursor");
        CHECK(!m.macro_playing,                    "macro: playing flag cleared");
        CHECK(m.macro_len == 2,                    "macro: replay did not re-record itself");

        /* Empty macro: Ctrl+T Ctrl+T with no keys in between → len=0, replay no-op. */
        jbe_handle_key(&m, JAPI_KEY_CTRL('T'));
        jbe_handle_key(&m, JAPI_KEY_CTRL('T'));
        CHECK(m.macro_len == 0,                    "macro: empty record yields len=0");
        int before_lines = JBE_BUF(&m)->n_lines;
        int before_len0  = JBE_BUF(&m)->len[0];
        jbe_handle_key(&m, JAPI_KEY_CTRL('P'));
        CHECK(JBE_BUF(&m)->n_lines == before_lines && JBE_BUF(&m)->len[0] == before_len0,
              "macro: empty-macro replay changes nothing");

        /* Ctrl+P is filtered from recording (so a macro can't accidentally
           include its own replay trigger). */
        jbe_handle_key(&m, JAPI_KEY_CTRL('T'));
        jbe_handle_key(&m, 'x');
        jbe_handle_key(&m, JAPI_KEY_CTRL('P'));    /* would replay empty, no-op */
        jbe_handle_key(&m, 'y');
        jbe_handle_key(&m, JAPI_KEY_CTRL('T'));
        CHECK(m.macro_len == 2 && m.macro_buf[0] == 'x' && m.macro_buf[1] == 'y',
              "macro: Ctrl+P excluded from recorded stream");

        /* Overflow: record more than JBE_MACRO_MAX keys, expect truncation. */
        jbe_handle_key(&m, JAPI_KEY_CTRL('T'));    /* start fresh */
        for (int i = 0; i < JBE_MACRO_MAX + 50; i++) {
            jbe_handle_key(&m, JAPI_KEY_LEFT);     /* harmless navigation */
        }
        jbe_handle_key(&m, JAPI_KEY_CTRL('T'));
        CHECK(m.macro_len == JBE_MACRO_MAX,
              "macro: overflow capped at JBE_MACRO_MAX");

        japi_remove("A:mac.txt");
        jbe_free(&m);
    }

    /* ----- MVP step 19: File→Open dialog ------------------------------ */
    {
        const char *DIR    = "A:_t19";
        const char *SUBDIR = "A:_t19/sub";
        japi_mkdir(DIR);
        japi_mkdir(SUBDIR);
        CHECK(make_fixture("A:_t19/file1.txt", "one\n"),    "open: write file1");
        CHECK(make_fixture("A:_t19/file2.txt", "two\n"),    "open: write file2");
        CHECK(make_fixture("A:_t19/sub/c.txt", "three\n"),  "open: write sub/c.txt");

        /* Widget at a subdir: yields synthetic ".." plus its real contents,
           sorted dirs-first then files-alphabetical. */
        ui_filelist_t w;
        ui_filelist_default_colors(&w);
        CHECK(ui_filelist_open(&w, DIR, 0, 0, 20, 50),          "open: widget opens DIR");
        CHECK(w.n_entries == 4,                                 "open: 4 entries");
        CHECK(strcmp(w.entries[0].name, "..") == 0 && w.entries[0].is_dir, "open: .. first");
        CHECK(strcmp(w.entries[1].name, "sub") == 0 && w.entries[1].is_dir, "open: sub second");
        CHECK(strcmp(w.entries[2].name, "file1.txt") == 0 && !w.entries[2].is_dir, "open: file1 third");
        CHECK(strcmp(w.entries[3].name, "file2.txt") == 0 && !w.entries[3].is_dir, "open: file2 fourth");

        /* Enter on a file → PICK, picked_path is the absolute path. */
        w.sel = 2;
        ui_filelist_event_t ev = ui_filelist_key(&w, JAPI_KEY_ENTER);
        CHECK(ev == UI_FILELIST_PICK,                           "open: Enter on file → PICK");
        char picked[UI_FILELIST_PATH_MAX];
        CHECK(ui_filelist_picked_path(&w, picked, sizeof picked), "open: picked_path returns true");
        CHECK(strcmp(picked, "A:_t19/file1.txt") == 0,          "open: picked == A:_t19/file1.txt");

        /* Enter on a subdir → CD, widget reloads with new cwd. */
        CHECK(ui_filelist_open(&w, DIR, 0, 0, 20, 50),          "open: reopen widget");
        w.sel = 1;
        ev = ui_filelist_key(&w, JAPI_KEY_ENTER);
        CHECK(ev == UI_FILELIST_CD,                             "open: Enter on dir → CD");
        CHECK(strcmp(w.cwd, "A:_t19/sub") == 0,                 "open: cwd advanced into sub");
        CHECK(w.n_entries == 2,                                 "open: sub has 2 entries");

        /* Enter on ".." → CD back to parent. */
        w.sel = 0;
        ev = ui_filelist_key(&w, JAPI_KEY_ENTER);
        CHECK(ev == UI_FILELIST_CD,                             "open: Enter on .. → CD");
        CHECK(strcmp(w.cwd, "A:_t19") == 0,                     "open: cwd back to parent");

        /* Esc → CANCEL. */
        ev = ui_filelist_key(&w, JAPI_KEY_ESCAPE);
        CHECK(ev == UI_FILELIST_CANCEL,                         "open: Esc → CANCEL");

        /* Root yields no synthetic ".." entry. */
        CHECK(ui_filelist_open(&w, "A:", 0, 0, 20, 50),         "open: widget opens at root");
        bool has_dotdot = false;
        for (int i = 0; i < w.n_entries; i++) {
            if (strcmp(w.entries[i].name, "..") == 0) { has_dotdot = true; break; }
        }
        CHECK(!has_dotdot,                                      "open: no .. entry at root");

        /* Navigation: Up/Down clamp, Home/End jump. */
        CHECK(ui_filelist_open(&w, DIR, 0, 0, 20, 50),          "open: reopen for nav");
        for (int i = 0; i < 100; i++) ui_filelist_key(&w, JAPI_KEY_DOWN);
        CHECK(w.sel == w.n_entries - 1,                         "open: Down clamps to last");
        for (int i = 0; i < 100; i++) ui_filelist_key(&w, JAPI_KEY_UP);
        CHECK(w.sel == 0,                                       "open: Up clamps to first");
        ui_filelist_key(&w, JAPI_KEY_END);
        CHECK(w.sel == w.n_entries - 1,                         "open: End → last");
        ui_filelist_key(&w, JAPI_KEY_HOME);
        CHECK(w.sel == 0,                                       "open: Home → first");

        /* Empty / missing drive (e.g. no SD card): the list has 0 entries, but
           Tab must still switch drives and Esc must still cancel. (Regression:
           the empty-list guard used to swallow both.) Force the empty state. */
        CHECK(ui_filelist_open(&w, "A:", 0, 0, 20, 50),         "open: reopen A: root");
        w.n_entries = 0;                                        /* pretend A: is empty/absent */
        ev = ui_filelist_key(&w, JAPI_KEY_TAB);
        CHECK(ev == UI_FILELIST_CD,                            "open: Tab works on empty list");
        CHECK(w.cwd[0] == 'C',                                 "open: empty A: + Tab -> C:");
        w.n_entries = 0;                                        /* force empty again */
        CHECK(ui_filelist_key(&w, JAPI_KEY_ESCAPE) == UI_FILELIST_CANCEL,
                                                               "open: Esc cancels on empty list");

        /* JBE integration: Alt+F + Alt+O opens dialog, Esc closes it. */
        jbe_state_t je; jbe_init(&je);
        CHECK(make_fixture("A:_t19a.txt", "seed\n"),            "open: write seed");
        CHECK(jbe_load(&je, "A:_t19a.txt"),                     "open: load seed buffer");
        jbe_handle_key(&je, JAPI_KEY_ALT('F'));
        CHECK(je.menu_active && je.menu_idx == 0,               "open: Alt+F opens File menu");
        jbe_handle_key(&je, JAPI_KEY_ALT('O'));
        CHECK(je.open_active,                                   "open: Alt+O opens dialog");
        CHECK(!je.menu_active,                                  "open: File menu closed");
        jbe_handle_key(&je, JAPI_KEY_ESCAPE);
        CHECK(!je.open_active,                                  "open: Esc closes dialog");
        CHECK(!je.open_request,                                 "open: no open_request after Esc");

        /* JBE integration: picking a file raises open_request with the path. */
        je.open_active = true;
        ui_filelist_open(&je.open_dlg, DIR, 0, 0, 20, 50);
        int fi = -1;
        for (int i = 0; i < je.open_dlg.n_entries; i++) {
            if (strcmp(je.open_dlg.entries[i].name, "file1.txt") == 0) { fi = i; break; }
        }
        CHECK(fi >= 0,                                          "open: file1.txt visible in dialog");
        je.open_dlg.sel = fi;
        jbe_handle_key(&je, JAPI_KEY_ENTER);
        CHECK(!je.open_active,                                  "open: dialog closes on pick");
        CHECK(je.open_request,                                  "open: open_request raised");
        CHECK(strcmp(je.open_path, "A:_t19/file1.txt") == 0,    "open: open_path correct");

        japi_remove("A:_t19/file1.txt");
        japi_remove("A:_t19/file2.txt");
        japi_remove("A:_t19/sub/c.txt");
        japi_remove("A:_t19a.txt");
        jbe_free(&je);
    }

    /* ----- Robust Open: an undisplayable file must not wedge the editor --- */
    {
        jbe_state_t s; jbe_init(&s);
        CHECK(make_fixture("A:_rb.txt", "good line\n"),         "robust: seed text file");
        CHECK(jbe_load(&s, "A:_rb.txt"),                        "robust: load text file");
        CHECK(strcmp(JBE_BUF(&s)->lines[0], "good line") == 0,  "robust: text loaded");

        /* A binary file (contains a NUL byte) -- like a screenshot BMP. */
        { japi_file_t f;
          CHECK(japi_fopen(&f, "A:_rb.bin", JAPI_WRITE),        "robust: write a binary file");
          char blob[6] = { 'B', 'M', 0, 'x', '\n', 'y' };
          japi_fwrite(&f, blob, 6);
          japi_fclose(&f); }

        /* Opening it must FAIL gracefully: returns false, the current document
           is left intact, the buffer stays valid, and a notice is set. */
        CHECK(!jbe_load(&s, "A:_rb.bin"),                       "robust: binary load refused");
        CHECK(strcmp(JBE_BUF(&s)->lines[0], "good line") == 0,  "robust: current file untouched");
        CHECK(JBE_BUF(&s)->n_lines >= 1,                        "robust: buffer still valid");
        CHECK(s.open_msg[0] != 0,                               "robust: a notice is shown");

        /* A file bigger than JBE_MAX_FILE_BYTES is refused BEFORE any big
           allocation (this is what really stopped the hang on a 787 KB BMP). */
        { japi_file_t f;
          CHECK(japi_fopen(&f, "A:_rb.big", JAPI_WRITE),        "robust: write oversize file");
          char chunk[256]; memset(chunk, 'a', sizeof chunk);
          for (int i = 0; i < (JBE_MAX_FILE_BYTES / 256) + 8; i++)
              japi_fwrite(&f, chunk, sizeof chunk);            /* > JBE_MAX_FILE_BYTES */
          japi_fclose(&f); }
        CHECK(!jbe_load(&s, "A:_rb.big"),                       "robust: oversize file refused");
        CHECK(strcmp(JBE_BUF(&s)->lines[0], "good line") == 0,  "robust: untouched after oversize");

        /* A missing file: also refused, document still intact. */
        CHECK(!jbe_load(&s, "A:_nope_xyz.txt"),                 "robust: missing file refused");
        CHECK(strcmp(JBE_BUF(&s)->lines[0], "good line") == 0,  "robust: still untouched");

        /* A successful load clears the notice. */
        CHECK(jbe_load(&s, "A:_rb.txt"),                        "robust: reload good file");
        CHECK(s.open_msg[0] == 0,                               "robust: success clears the notice");

        /* jbe_new (a fresh document) also clears the notice -- this is what
           stops the boot "Cannot open A:scratch.txt" from lingering after the
           startup fallback. */
        CHECK(!jbe_load(&s, "A:_rb.bin"),                       "robust: re-trigger a notice");
        CHECK(s.open_msg[0] != 0,                               "robust: notice set again");
        jbe_new(&s);
        CHECK(s.open_msg[0] == 0,                               "robust: jbe_new clears the notice");

        japi_remove("A:_rb.txt");
        japi_remove("A:_rb.bin");
        japi_remove("A:_rb.big");
        jbe_free(&s);
    }

    /* ----- MVP step 20: extra Ctrl-shortcuts + Macro menu ------------- */
    {
        jbe_state_t s; jbe_init(&s);
        CHECK(make_fixture("A:_t20.txt", "x\n"),                 "shortcut: seed");
        CHECK(jbe_load(&s, "A:_t20.txt"),                        "shortcut: load seed");

        /* Ctrl+N raises new_request (consumed by main loop). */
        s.new_request = false;
        jbe_handle_key(&s, JAPI_KEY_CTRL('N'));
        CHECK(s.new_request,                                     "shortcut: Ctrl+N raises new_request");
        s.new_request = false;

        /* Ctrl+O opens the file-open dialog. Esc to clean up. */
        jbe_handle_key(&s, JAPI_KEY_CTRL('O'));
        CHECK(s.open_active,                                     "shortcut: Ctrl+O opens dialog");
        jbe_handle_key(&s, JAPI_KEY_ESCAPE);
        CHECK(!s.open_active,                                    "shortcut: Esc closes dialog");

        /* Ctrl+Q sets quit. */
        s.quit = false;
        jbe_handle_key(&s, JAPI_KEY_CTRL('Q'));
        CHECK(s.quit,                                            "shortcut: Ctrl+Q raises quit");
        s.quit = false;

        /* Ctrl+D is no longer bound — Split is menu-only now, so it's a no-op. */
        CHECK(!s.split_active,                                   "shortcut: split off initially");
        jbe_handle_key(&s, JAPI_KEY_CTRL('D'));
        CHECK(!s.split_active,                                   "shortcut: Ctrl+D is a no-op");

        /* Macro is title index 4 (File/Edit/View/Search/Macro/Options). Alt+M opens it. */
        jbe_handle_key(&s, JAPI_KEY_ALT('M'));
        CHECK(s.menu_active && s.menu_idx == 4,                  "macro-menu: Alt+M opens Macro");
        /* Item 0 = Record start/stop, accelerator 'R'. */
        jbe_handle_key(&s, JAPI_KEY_ALT('R'));
        CHECK(s.macro_recording,                                 "macro-menu: Alt+R starts recording");
        CHECK(!s.menu_active,                                    "macro-menu: menu closed after activate");
        /* Stop via the shortcut. */
        jbe_handle_key(&s, JAPI_KEY_CTRL('T'));
        CHECK(!s.macro_recording,                                "macro-menu: Ctrl+T stops recording");

        /* Menu route for Play (Macro→Play) replays the macro. We need something
           recorded first — a single Left key — so we can see the replay
           effect. Use HOME to park the cursor first. */
        jbe_handle_key(&s, JAPI_KEY_HOME);
        jbe_handle_key(&s, JAPI_KEY_CTRL('T'));      /* start recording */
        jbe_handle_key(&s, JAPI_KEY_RIGHT);          /* move col 0 -> 1 */
        jbe_handle_key(&s, JAPI_KEY_CTRL('T'));      /* stop recording */
        CHECK(s.macro_len == 1 && s.macro_buf[0] == JAPI_KEY_RIGHT,
                                                                 "macro-menu: macro captured Right");
        jbe_handle_key(&s, JAPI_KEY_HOME);
        jbe_handle_key(&s, JAPI_KEY_ALT('M'));
        jbe_handle_key(&s, JAPI_KEY_ALT('P'));       /* Alt+P activates Play */
        CHECK(JBE_PANE(&s)->cur_col == 1,                        "macro-menu: Alt+P replayed Right");

        /* Options is the last title (index 5). */
        jbe_handle_key(&s, JAPI_KEY_ALT('O'));
        CHECK(s.menu_active && s.menu_idx == 5,                  "menu: Options is now idx 5");
        jbe_handle_key(&s, JAPI_KEY_ESCAPE);

        /* View (the renamed split menu) sits right after Edit, at index 2. */
        jbe_handle_key(&s, JAPI_KEY_ALT('V'));
        CHECK(s.menu_active && s.menu_idx == 2,                  "menu: View is idx 2 (after Edit)");
        jbe_handle_key(&s, JAPI_KEY_ESCAPE);

        /* Regression: navigating to Options via arrows (not Alt+O direct)
           used to leave options_n at zero, then DOWN inside it crashed
           with modulo-by-zero. menu_open is now invoked on every arrow
           transition so the dynamic rebuild runs each time. */
        jbe_handle_key(&s, JAPI_KEY_ALT('M'));                   /* Macro (idx 4) */
        CHECK(s.menu_idx == 4,                                   "navfix: start in Macro");
        jbe_handle_key(&s, JAPI_KEY_RIGHT);                      /* → Options (idx 5) */
        CHECK(s.menu_idx == 5,                                   "navfix: RIGHT to Options");
        CHECK(s.options_n > 0,                                   "navfix: Options rebuilt on arrow nav");
        /* DOWN must not crash even after an arrow-transition into a menu.
           Guarded by the n>0 check on item_idx update. */
        jbe_handle_key(&s, JAPI_KEY_DOWN);
        CHECK(s.item_idx >= 0 && s.item_idx < s.options_n,       "navfix: DOWN in Options stays bounded");
        jbe_handle_key(&s, JAPI_KEY_ESCAPE);

        japi_remove("A:_t20.txt");
        jbe_free(&s);
    }

    /* ----- Tab indent / Shift+Tab dedent (line + block) -------------- */
    {
        jbe_state_t s; jbe_init(&s);
        CHECK(make_fixture("A:_t21.txt", "alpha\nbeta\ngamma\n"),  "tab: seed");
        CHECK(jbe_load(&s, "A:_t21.txt"),                          "tab: load");

        /* No selection: Tab inserts spaces to the next tab stop at the cursor. */
        JBE_PANE(&s)->cur_row = 0; JBE_PANE(&s)->cur_col = 0;
        jbe_handle_key(&s, JAPI_KEY_TAB);
        CHECK(strcmp(JBE_BUF(&s)->lines[0], "  alpha") == 0,      "tab: inserts 2 spaces at col 0");
        CHECK(JBE_PANE(&s)->cur_col == 2,                          "tab: cursor advances by 2");

        /* Shift+Tab dedents the current line. */
        jbe_handle_key(&s, JAPI_KEY_STAB);
        CHECK(strcmp(JBE_BUF(&s)->lines[0], "alpha") == 0,        "tab: Shift+Tab removes the indent");

        /* Block indent: select rows 0..2, Tab shifts all three right by 2. */
        JBE_PANE(&s)->sel_active = true;
        JBE_PANE(&s)->sel_row = 0; JBE_PANE(&s)->sel_col = 0;
        JBE_PANE(&s)->cur_row = 2; JBE_PANE(&s)->cur_col = 0;
        jbe_handle_key(&s, JAPI_KEY_TAB);
        CHECK(strcmp(JBE_BUF(&s)->lines[0], "  alpha") == 0 &&
              strcmp(JBE_BUF(&s)->lines[1], "  beta")  == 0 &&
              strcmp(JBE_BUF(&s)->lines[2], "  gamma") == 0,       "tab: block indent +2 on all rows");

        /* Block dedent brings them back. */
        jbe_handle_key(&s, JAPI_KEY_STAB);
        CHECK(strcmp(JBE_BUF(&s)->lines[0], "alpha") == 0 &&
              strcmp(JBE_BUF(&s)->lines[1], "beta")  == 0 &&
              strcmp(JBE_BUF(&s)->lines[2], "gamma") == 0,         "tab: block dedent -2 on all rows");

        japi_remove("A:_t21.txt");
        jbe_free(&s);
    }

    /* ----- File: Save As + Save-on-untitled (the New->Save bug) ------- */
    {
        jbe_state_t s; jbe_init(&s);
        jbe_new(&s);                                  /* untitled: no path */
        CHECK(JBE_BUF(&s)->path[0] == 0,              "saveas: new buffer has no path");
        type_str(&s, "hi");
        japi_remove("A:_sa.bas");

        /* Save on an untitled buffer must open the Save As prompt, not fail
           silently (this was the reported bug). */
        jbe_handle_key(&s, JAPI_KEY_CTRL('S'));
        CHECK(s.save_as_active,                       "saveas: Ctrl+S on untitled opens Save As");
        CHECK(strcmp(s.save_as_name, "A:") == 0,      "saveas: prompt prefilled with A: drive");

        type_str(&s, "_sa.bas");                       /* user types the name after A: */
        jbe_handle_key(&s, JAPI_KEY_ENTER);
        if (s.save_request) { s.save_request = false; jbe_save(&s); }   /* loop does the write */
        CHECK(!s.save_as_active,                       "saveas: Enter closes the prompt");
        CHECK(strcmp(JBE_BUF(&s)->path, "A:_sa.bas") == 0,  "saveas: path adopted");
        CHECK(strcmp(JBE_BUF(&s)->filename, "_sa.bas") == 0,"saveas: basename as filename");

        char back[64];
        CHECK(slurp("A:_sa.bas", back, sizeof back),   "saveas: file exists on disk");
        CHECK(strcmp(back, "hi\n") == 0,               "saveas: file holds the text");

        /* A second Save now writes straight to the path (no prompt). */
        jbe_handle_key(&s, JAPI_KEY_CTRL('S'));
        if (s.save_request) { s.save_request = false; jbe_save(&s); }   /* loop does the write */
        CHECK(!s.save_as_active,                       "saveas: titled Save skips the prompt");

        japi_remove("A:_sa.bas");
        jbe_free(&s);
    }

    /* ----- File: Close (empties the active pane; never removes it) ----- */
    {
        jbe_state_t s; jbe_init(&s);
        jbe_new(&s);                                   /* pane 0 / buffer 0 */
        strcpy(JBE_BUF(&s)->filename, "one");
        type_str(&s, "hello");

        /* Single pane: Close on a dirty doc asks first; Y empties it to a fresh
           untitled. The pane stays (there is always one pane). */
        jbe_handle_key(&s, JAPI_KEY_CTRL('W'));
        CHECK(s.close_confirm && !s.close_request,     "close: dirty doc asks to confirm");
        jbe_handle_key(&s, 'n');
        CHECK(!s.close_confirm && !s.close_request,    "close: 'n' cancels");
        jbe_handle_key(&s, JAPI_KEY_CTRL('W'));
        jbe_handle_key(&s, 'Y');
        CHECK(s.close_request,                         "close: Y confirms");
        s.close_request = false; jbe_close_active(&s);
        CHECK(!s.split_active,                         "close: single pane stays single");
        CHECK(strcmp(JBE_BUF(&s)->filename, "(untitled)") == 0 && !JBE_BUF(&s)->dirty,
                                                        "close: doc reset to fresh untitled");

        /* Split, give pane 1 its own dirty file, then Close it: the pane is
           EMPTIED but stays, and pane 0 is left untouched. */
        strcpy(JBE_BUF(&s)->filename, "one");          /* pane 0 = "one" */
        jbe_handle_key(&s, JAPI_KEY_ALT('V'));
        jbe_handle_key(&s, JAPI_KEY_ALT('W'));         /* split, shared views, focus pane 0 */
        s.active_pane = 1;
        jbe_new(&s);                                   /* pane 1 detaches to its own buffer */
        strcpy(JBE_BUF(&s)->filename, "two");
        type_str(&s, "zzz");
        jbe_handle_key(&s, JAPI_KEY_CTRL('W'));
        CHECK(s.close_confirm && s.confirm_action == JBE_CONFIRM_CLOSE, "close: split pane Close asks (not un-split)");
        jbe_handle_key(&s, 'Y');
        s.close_request = false; jbe_close_active(&s);
        CHECK(s.split_active && s.active_pane == 1,     "close: split + pane stay after Close");
        CHECK(strcmp(JBE_BUF(&s)->filename, "(untitled)") == 0, "close: pane 1 emptied");
        CHECK(strcmp(s.buffers[s.panes[0].buf_idx].filename, "one") == 0,
                                                        "close: pane 0 untouched");

        /* Closing one of two SHARED views loses nothing (the file lives on in
           the other view), so Close empties without asking even when dirty. */
        jbe_free(&s); jbe_init(&s);
        jbe_new(&s); strcpy(JBE_BUF(&s)->filename, "shared"); type_str(&s, "data");
        jbe_handle_key(&s, JAPI_KEY_ALT('V'));
        jbe_handle_key(&s, JAPI_KEY_ALT('W'));         /* both panes share the dirty "shared" */
        jbe_handle_key(&s, JAPI_KEY_CTRL('W'));        /* close the active (pane 0) view */
        CHECK(!s.close_confirm && s.close_request,     "close: shared view closes without asking");
        s.close_request = false; jbe_close_active(&s);
        CHECK(s.split_active,                          "close: split stays after closing a view");
        CHECK(strcmp(JBE_BUF(&s)->filename, "(untitled)") == 0, "close: this view emptied");
        CHECK(strcmp(s.buffers[s.panes[1].buf_idx].lines[0], "data") == 0,
                                                        "close: the other view keeps the file");
        jbe_free(&s);
    }

    /* ----- File→Open dialog: delete a file (with confirm) ------------- */
    {
        jbe_state_t s; jbe_init(&s);
        jbe_new(&s);                                    /* give the buffer a line */
        CHECK(make_fixture("A:_del.bas", "x\n"),       "del: seed file");

        jbe_handle_key(&s, JAPI_KEY_CTRL('O'));         /* open the dialog on A: */
        CHECK(s.open_active,                            "del: Open dialog active");

        /* Find and select our file in the listing. */
        int idx = -1;
        for (int i = 0; i < s.open_dlg.n_entries; i++)
            if (strcmp(s.open_dlg.entries[i].name, "_del.bas") == 0) idx = i;
        CHECK(idx >= 0,                                 "del: file appears in the dialog");
        s.open_dlg.sel = idx;

        /* Delete asks first; a non-Y key cancels and keeps the file. */
        jbe_handle_key(&s, JAPI_KEY_DELETE);
        CHECK(s.open_dlg.confirm_delete,                "del: Delete asks to confirm");
        jbe_handle_key(&s, 'n');
        CHECK(!s.open_dlg.confirm_delete,               "del: 'n' cancels");
        japi_file_t f;
        CHECK(japi_fopen(&f, "A:_del.bas", JAPI_READ),  "del: file still there after cancel");
        japi_fclose(&f);

        /* Delete + Y removes it and refreshes the listing. */
        jbe_handle_key(&s, JAPI_KEY_DELETE);
        jbe_handle_key(&s, 'Y');
        CHECK(!s.open_dlg.confirm_delete,               "del: confirm cleared");
        CHECK(!japi_fopen(&f, "A:_del.bas", JAPI_READ), "del: file removed from disk");
        int still = 0;
        for (int i = 0; i < s.open_dlg.n_entries; i++)
            if (strcmp(s.open_dlg.entries[i].name, "_del.bas") == 0) still = 1;
        CHECK(!still,                                   "del: gone from the refreshed listing");
        CHECK(s.open_active,                            "del: dialog stays open after delete");

        jbe_free(&s);
    }

    /* ----- step 21: F1 context-sensitive help ----------------------------- */
    {
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:jbe_h.txt", "one\n"), "write help fixture");
        CHECK(jbe_load(&e, "A:jbe_h.txt"), "load help fixture");

        /* F1 while editing opens the help at the top; Esc closes it. */
        jbe_handle_key(&e, JAPI_KEY_F1);
        CHECK(e.help_active && e.help_top == 0, "F1 (editing) opens help at top");
        jbe_handle_key(&e, JAPI_KEY_ESCAPE);
        CHECK(!e.help_active, "Esc closes help");

        /* F1 with the Edit menu open, highlighted on Cut (item 0), jumps to Cut. */
        jbe_handle_key(&e, JAPI_KEY_ALT('E'));
        CHECK(e.menu_active && e.menu_idx == 1, "Alt+E opens Edit menu");
        jbe_handle_key(&e, JAPI_KEY_F1);
        CHECK(e.help_active && e.help_top == help_anchor("item:Edit/Cut"),
              "F1 on Cut -> Cut help");
        jbe_handle_key(&e, JAPI_KEY_ESCAPE);

        /* Move down to Select All (Cut,Copy,Paste,Select All = index 3) -> its help. */
        jbe_handle_key(&e, JAPI_KEY_ALT('E'));
        jbe_handle_key(&e, JAPI_KEY_DOWN);
        jbe_handle_key(&e, JAPI_KEY_DOWN);
        jbe_handle_key(&e, JAPI_KEY_DOWN);
        jbe_handle_key(&e, JAPI_KEY_F1);
        CHECK(e.help_active && e.help_top == help_anchor("item:Edit/Select All"),
              "F1 on Select All -> its help");
        jbe_handle_key(&e, JAPI_KEY_ESCAPE);

        /* Scroll: PgDn moves down, Home returns to the top. */
        jbe_handle_key(&e, JAPI_KEY_F1);
        jbe_handle_key(&e, JAPI_KEY_PGDN);
        CHECK(e.help_active && e.help_top > 0, "PgDn scrolls help down");
        jbe_handle_key(&e, JAPI_KEY_HOME);
        CHECK(e.help_top == 0, "Home returns to top");
        jbe_handle_key(&e, JAPI_KEY_ESCAPE);

        /* The Help menu makes it discoverable: Alt+H, then Editor Help. */
        jbe_handle_key(&e, JAPI_KEY_ALT('H'));
        CHECK(e.menu_active && e.menu_idx == 7, "Alt+H opens Help menu");
        jbe_handle_key(&e, JAPI_KEY_ENTER);
        CHECK(e.help_active && e.help_top == 0, "Help -> Editor Help opens help");
        jbe_handle_key(&e, JAPI_KEY_ESCAPE);
    }

    /* ----- step 22: Japi Commander file ops (mkdir / copy / delete) -------- */
    {
        jbe_state_t e;
        jbe_init(&e);
        CHECK(make_fixture("A:cz.txt", "hello"), "write commander fixture");
        CHECK(jbe_load(&e, "A:cz.txt"), "load commander fixture");

        jbe_handle_key(&e, JAPI_KEY_CTRL('J'));
        CHECK(e.commander_active, "Ctrl+J opens the Commander");

        /* Ctrl+N = new folder: prompt, type a name, Enter creates it on A:. */
        jbe_handle_key(&e, JAPI_KEY_CTRL('N'));
        CHECK(e.commander_input_active && e.commander_input_kind == 0,
              "Ctrl+N opens the new-folder prompt");
        const char *nm = "tdir";
        for (int i = 0; nm[i]; i++) jbe_handle_key(&e, (uint16_t)nm[i]);
        CHECK(e.commander_input_len == 4, "typed the folder name");
        jbe_handle_key(&e, JAPI_KEY_ENTER);
        CHECK(!e.commander_input_active, "Enter closes the prompt");
        { japi_dir_t d; CHECK(japi_opendir(&d, "A:tdir"), "Ctrl+N created the folder"); }

        /* The name field edits like a text box: type "tdr", move the caret left
           one (onto 'r'), insert 'i' -> "tdir", with the caret tracking along. */
        jbe_handle_key(&e, JAPI_KEY_CTRL('N'));
        jbe_handle_key(&e, 't'); jbe_handle_key(&e, 'd'); jbe_handle_key(&e, 'r');
        CHECK(e.commander_input_len == 3 && e.commander_input_cur == 3 &&
              strcmp(e.commander_input, "tdr") == 0, "typed tdr, caret at end");
        jbe_handle_key(&e, JAPI_KEY_LEFT);
        CHECK(e.commander_input_cur == 2, "Left moves the caret back one");
        jbe_handle_key(&e, 'i');
        CHECK(strcmp(e.commander_input, "tdir") == 0 && e.commander_input_cur == 3,
              "typing inserts at the caret, not at the end");
        /* Home then Delete removes the leading 't' -> "dir". */
        jbe_handle_key(&e, JAPI_KEY_HOME);
        CHECK(e.commander_input_cur == 0, "Home jumps to the start");
        jbe_handle_key(&e, JAPI_KEY_DELETE);
        CHECK(strcmp(e.commander_input, "dir") == 0 && e.commander_input_cur == 0,
              "Delete removes the char at the caret");
        jbe_handle_key(&e, JAPI_KEY_ESCAPE);
        CHECK(!e.commander_input_active, "Esc cancels the edited prompt");

        /* Delete an (empty) folder: put the cursor on tdir, Delete, confirm with
           Y. japi_remove drops empty dirs, so the folder must be gone after. */
        {
            ui_filelist_t *ap = &e.commander_list[0];
            for (int g = 0; g < ap->n_entries &&
                 strcmp(ap->entries[ap->sel].name, "tdir") != 0; g++)
                jbe_handle_key(&e, JAPI_KEY_DOWN);
            CHECK(strcmp(ap->entries[ap->sel].name, "tdir") == 0 &&
                  ap->entries[ap->sel].is_dir, "cursor on the tdir folder");
            jbe_handle_key(&e, JAPI_KEY_DELETE);
            CHECK(e.commander_confirm_delete, "Delete on a folder asks to confirm");
            jbe_handle_key(&e, 'Y');
            { japi_dir_t d; CHECK(!japi_opendir(&d, "A:tdir"), "Y removed the empty folder"); }
        }

        /* Case-only rename (rn.txt -> RN.TXT). The host disk is case-sensitive,
           so this exercises the temp-name hop in commander_do_rename and proves
           it does not lose the file; the real FAT case-insensitive failure it
           guards against is hardware-only. */
        {
            CHECK(make_fixture("A:rn.txt", "x"), "write rename fixture");
            /* Reopen the Commander so the pane reloads and shows the new file. */
            jbe_handle_key(&e, JAPI_KEY_CTRL('J'));    /* close */
            jbe_handle_key(&e, JAPI_KEY_CTRL('J'));    /* open -> fresh listing */
            ui_filelist_t *ap = &e.commander_list[0];
            for (int g = 0; g < ap->n_entries &&
                 strcmp(ap->entries[ap->sel].name, "rn.txt") != 0; g++)
                jbe_handle_key(&e, JAPI_KEY_DOWN);
            CHECK(strcmp(ap->entries[ap->sel].name, "rn.txt") == 0, "cursor on rn.txt");
            jbe_handle_key(&e, JAPI_KEY_CTRL('R'));    /* prefilled with rn.txt */
            while (e.commander_input_len > 0) jbe_handle_key(&e, JAPI_KEY_BACKSPACE);
            const char *up = "RN.TXT";
            for (int i = 0; up[i]; i++) jbe_handle_key(&e, (uint16_t)up[i]);
            jbe_handle_key(&e, JAPI_KEY_ENTER);
            CHECK(strstr(e.commander_msg, "Renamed") != NULL, "case-only rename succeeds");
            {
                japi_file_t f;
                bool ok = japi_fopen(&f, "A:RN.TXT", JAPI_READ);
                if (ok) japi_fclose(&f);
                CHECK(ok, "RN.TXT exists after rename");
            }
            japi_remove("A:RN.TXT");
            japi_remove("A:rn.txt");
            jbe_handle_key(&e, JAPI_KEY_HOME);   /* leave the cursor at the top */
        }

        /* Windows-style copy: select cz.txt in A:, Ctrl+C, Tab to C:, Ctrl+V. */
        ui_filelist_t *a = &e.commander_list[0];
        for (int g = 0; g < a->n_entries &&
             strcmp(a->entries[a->sel].name, "cz.txt") != 0; g++)
            jbe_handle_key(&e, JAPI_KEY_DOWN);
        CHECK(strcmp(a->entries[a->sel].name, "cz.txt") == 0, "selected cz.txt in A:");
        japi_remove("C:cz.txt");
        jbe_handle_key(&e, JAPI_KEY_CTRL('C'));
        CHECK(e.clip_n == 1 && !e.clip_cut, "Ctrl+C puts one file on the clipboard");
        jbe_handle_key(&e, JAPI_KEY_TAB);                 /* to the C: pane */
        jbe_handle_key(&e, JAPI_KEY_CTRL('V'));           /* paste into C: */
        {
            japi_file_t f;
            bool ok = japi_fopen(&f, "C:cz.txt", JAPI_READ);
            if (ok) japi_fclose(&f);
            CHECK(ok, "Ctrl+V pasted cz.txt to C:");
        }

        /* Back to A:, Space tags the file, Delete asks to confirm, Y removes it. */
        jbe_handle_key(&e, JAPI_KEY_TAB);                 /* back to A: */
        for (int g = 0; g < a->n_entries &&
             strcmp(a->entries[a->sel].name, "cz.txt") != 0; g++)
            jbe_handle_key(&e, JAPI_KEY_DOWN);
        int czidx = a->sel;
        jbe_handle_key(&e, ' ');                          /* tag it, step down */
        CHECK(a->entries[czidx].tagged, "Space tags the current file");
        jbe_handle_key(&e, JAPI_KEY_DELETE);
        CHECK(e.commander_confirm_delete, "Delete asks to confirm");
        jbe_handle_key(&e, 'Y');
        {
            japi_file_t f;
            bool gone = !japi_fopen(&f, "A:cz.txt", JAPI_READ);
            if (!gone) japi_fclose(&f);
            CHECK(gone, "delete removed A:cz.txt");
        }
    }

    if (fails == 0) { printf("PASS: JBE MVP step 1..5c + 10 + 11 + 12 + 13 + 14 + 15 + 16 + 17 + 18 + 19 + 20 (shortcuts+macro-menu) + tab-indent + file(saveas/close/delete) + 21 (F1 help) + 22 (commander ops)\n"); return 0; }
    printf("%d check(s) failed\n", fails);
    return 1;
}
