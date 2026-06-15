/* Manual screenshot generator -- HOST TOOL, not part of the firmware.
 *
 * Renders the editor's VGA text buffer to a PPM image using the *real* 8x12
 * font (font_8x12.h) and the *real* 6-bit VGA palette, so a screenshot in the
 * manual shows exactly what the hardware puts on the monitor -- not a terminal
 * approximation. A wrapper script converts the PPM to PNG with ffmpeg.
 *
 * It builds a scene programmatically (load a sample file; optionally open a
 * menu or make a selection), calls jbe_render() to fill vga_text_buffer, and
 * writes the pixels. Usage:
 *     screenshot <scene> <out.ppm>
 *   scenes: overview | select | filemenu | editmenu | showcase
 *
 * Note: we deliberately do NOT call japi_init() -- that would switch the
 * terminal to its alt-screen. The VGA buffer and the file API both work
 * standalone, and we never call vga_update() (which is what paints a terminal).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "japi_base.h"
#include "jbe.h"
#include "font_8x12.h"          /* defines font_8x12[256][12] (single TU) */

#define CELL_W 8
#define CELL_H 12
#define IMG_W  (VGA_COLS * CELL_W)   /* 127 * 8  = 1016 px */
#define IMG_H  (VGA_ROWS * CELL_H)   /*  64 * 12 =  768 px */

/* 6-bit RRGGBB -> 24-bit RGB. Each 2-bit channel (0..3) scales by 85. */
static void pal(uint8_t c, unsigned char *r, unsigned char *g, unsigned char *b) {
    *r = (unsigned char)(((c >> 4) & 3) * 85);
    *g = (unsigned char)(((c >> 2) & 3) * 85);
    *b = (unsigned char)(( c       & 3) * 85);
}

/* Write the current vga_text_buffer to a binary PPM (P6). */
static int write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", IMG_W, IMG_H);
    static unsigned char row[IMG_W * 3];
    for (int sr = 0; sr < VGA_ROWS; sr++) {
        for (int gy = 0; gy < CELL_H; gy++) {
            int x = 0;
            for (int sc = 0; sc < VGA_COLS; sc++) {
                vga_char_t cell = vga_text_buffer[sr][sc];
                unsigned char fr, fg, fb, br, bg, bb;
                pal(cell.fg, &fr, &fg, &fb);
                pal(cell.bg, &br, &bg, &bb);
                uint8_t bits = font_8x12[cell.code][gy];
                for (int gx = 0; gx < CELL_W; gx++) {
                    int on = bits & (0x80 >> gx);
                    row[x * 3 + 0] = on ? fr : br;
                    row[x * 3 + 1] = on ? fg : bg;
                    row[x * 3 + 2] = on ? fb : bb;
                    x++;
                }
            }
            fwrite(row, 1, IMG_W * 3, f);
        }
    }
    fclose(f);
    return 1;
}

static void make_file(const char *path, const char *content) {
    japi_file_t fo;
    if (japi_fopen(&fo, path, JAPI_WRITE)) {
        japi_fwrite(&fo, content, (int)strlen(content));
        japi_fclose(&fo);
    }
}

int main(int argc, char **argv) {
    const char *scene = argc > 1 ? argv[1] : "overview";
    const char *out   = argc > 2 ? argv[2] : "screenshot.ppm";

    static jbe_state_t ed;
    jbe_init(&ed);

    /* Two sample programs. The logger is a realistic data-logging sketch used
       by the manual scenes; the rose is a colourful graphics demo for the
       "showcase" scene (e.g. a forum screenshot). Both exercise the syntax
       colouriser: comments, keywords, strings, numbers. */
    static const char *logger =
        "' Temperature logger -- reads the sensor every second\n"
        "DIM sample AS FLOAT, count AS INTEGER\n"
        "DIM total = 0 AS FLOAT\n"
        "FOR count = 1 TO 60\n"
        "    sample = TEMPR(0)\n"
        "    total = total + sample\n"
        "    PRINT \"Reading \"; count; \" = \"; sample; \" degrees\"\n"
        "    PAUSE 1000\n"
        "NEXT count\n"
        "PRINT \"Average over a minute: \"; total / 60\n"
        "END\n";
    /* A screen-filling, meaningful program for the "editmenu" hero shot: a
       little scene drawn with the graphics commands, so the colouriser has
       plenty of keywords, comments, strings and numbers to show off. */
    static const char *scene_bas =
        "' =====================================================================\n"
        "'  A QUIET AFTERNOON\n"
        "'  A little scene drawn with the Japi Base graphics commands:\n"
        "'  a sky gradient, the sun, rolling hills, a path and a small house.\n"
        "' =====================================================================\n"
        "\n"
        "DIM x, y AS INTEGER\n"
        "\n"
        "GRAPHICS OPEN 0, 0, 126, 63, 1            ' a full-screen 504x384 bitmap\n"
        "\n"
        "' --- sky: a smooth vertical gradient -------------------------------\n"
        "FOR y = 0 TO 250\n"
        "    LINE 0, y, 503, y, RGB(60, 110, 140 + y / 5)\n"
        "NEXT y\n"
        "\n"
        "' --- a scatter of stars still fading in the morning sky ------------\n"
        "FOR x = 1 TO 40\n"
        "    PIXEL 12 * x, 30 + 2 * x, RGB(255, 255, 220)\n"
        "NEXT x\n"
        "\n"
        "' --- the sun, with two soft haloes ---------------------------------\n"
        "CIRCLE 410, 70, 60, RGB(255, 240, 120), 1\n"
        "CIRCLE 410, 70, 72, RGB(255, 225, 80)\n"
        "CIRCLE 410, 70, 86, RGB(255, 205, 50)\n"
        "\n"
        "' --- rolling hills as a row of tall green columns ------------------\n"
        "FOR x = 0 TO 503\n"
        "    y = 250 + 40 * SIN(x * 0.012)\n"
        "    LINE x, y, x, 383, RGB(40, 150, 60)\n"
        "NEXT x\n"
        "\n"
        "' --- a winding path drawn dot by dot -------------------------------\n"
        "FOR x = 0 TO 200\n"
        "    PIXEL 250 + 60 * SIN(x * 0.05), 383 - x, RGB(210, 200, 150)\n"
        "NEXT x\n"
        "\n"
        "' --- a small house: walls, roof, door and a window -----------------\n"
        "BOX  120, 250, 90, 70, RGB(200, 180, 150), 1    ' walls\n"
        "LINE 120, 250, 165, 210, RGB(150, 60, 40)       ' left roof\n"
        "LINE 210, 250, 165, 210, RGB(150, 60, 40)       ' right roof\n"
        "BOX  150, 285, 25, 35, RGB(90, 50, 30), 1       ' door\n"
        "BOX  180, 262, 22, 22, RGB(120, 200, 230), 1    ' window\n"
        "\n"
        "PRINT \"A quiet afternoon, in 504 by 384 pixels.\"\n"
        "END\n";
    static const char *rose =
        "' --- Maurer rose -------------------------------------------\n"
        "' a rose walked in big angular steps -> an intricate web.\n"
        "\n"
        "GRAPHICS OPEN 32, 16, 62, 31, 1\n"
        "\n"
        "DIM i, t, r, x, y, px, py, cr, cg, cb AS FLOAT\n"
        "\n"
        "FOR i = 0 TO 360\n"
        "    t = i * 71 * PI / 180\n"
        "    r = 175 * COS(10 * t)\n"
        "    x = 248 + r * COS(t)\n"
        "    y = 186 + r * SIN(t)\n"
        "    cr = 128 + 127 * COS(i * 0.01745)\n"
        "    cg = 128 + 127 * COS(i * 0.01745 - 2.094)\n"
        "    cb = 128 + 127 * COS(i * 0.01745 - 4.188)\n"
        "    IF i > 0 THEN LINE px, py, x, y, RGB(cr, cg, cb)\n"
        "    px = x : py = y\n"
        "NEXT i\n"
        "\n"
        "PRINT \"Maurer rose done.\"\n";

    bool showcase = (strcmp(scene, "showcase") == 0);
    bool editmenu = (strcmp(scene, "editmenu") == 0);
    bool help     = (strcmp(scene, "help") == 0);
    bool helpmenu = (strcmp(scene, "helpmenu") == 0);
    const char *fname, *content;
    if      (showcase)                     { fname = "A:rose.bas";  content = rose; }
    else if (editmenu || help || helpmenu) { fname = "A:scene.bas"; content = scene_bas; }
    else                                   { fname = "A:logger.bas"; content = logger; }
    make_file(fname, content);
    jbe_load(&ed, fname);
    ed.panes[0].cur_row = 0; ed.panes[0].cur_col = 0;

    if (strcmp(scene, "select") == 0) {
        /* A block (rectangular) selection across the loop body. */
        ed.panes[0].sel_active = true;
        ed.panes[0].sel_block  = true;
        ed.panes[0].sel_row = 4; ed.panes[0].sel_col = 4;
        ed.panes[0].cur_row = 7; ed.panes[0].cur_col = 16;
    } else if (strcmp(scene, "filemenu") == 0) {
        jbe_handle_key(&ed, JAPI_KEY_ALT('F'));   /* open the File menu */
    } else if (editmenu) {
        jbe_handle_key(&ed, JAPI_KEY_ALT('E'));   /* open the Edit menu */
    } else if (help) {
        ed.help_active = true; ed.help_top = 0;   /* F1 help window overlay */
    } else if (helpmenu) {
        jbe_handle_key(&ed, JAPI_KEY_ALT('H'));    /* open the Help menu */
    } else if (strcmp(scene, "commander") == 0) {
        make_file("A:hello.bas",     "PRINT \"hi\"\n");
        make_file("A:notes.txt",     "notes\n");
        make_file("A:AZERTY_FR.kbd", "kbd\n");
        make_file("C:readme.txt",    "readme\n");
        jbe_handle_key(&ed, JAPI_KEY_CTRL('J'));    /* open the Japi Commander */
    }

    jbe_render(&ed);
    if (!write_ppm(out)) { fprintf(stderr, "screenshot: cannot write %s\n", out); return 1; }
    return 0;
}
