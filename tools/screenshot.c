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
 *   scenes: overview | select | filemenu
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
    const char *fname = showcase ? "A:rose.bas" : "A:logger.bas";
    make_file(fname, showcase ? rose : logger);
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
    }

    jbe_render(&ed);
    if (!write_ppm(out)) { fprintf(stderr, "screenshot: cannot write %s\n", out); return 1; }
    return 0;
}
