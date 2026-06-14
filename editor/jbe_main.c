/* Sim entry-point for JBE. The editor holds one document per pane (at most
   two), so a command line may name up to two files: the first opens in pane
   0, an optional second opens the split into pane 1. Further arguments are
   ignored. Window->Split / File->Close add or drop the second document at
   run time. On the Pico the equivalent main() seeds a single scratch file. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "japi_base.h"
#include "jbe.h"

static void show_open_error(const char *path) {
    vga_clear(VGA_WHITE, VGA_DARK_BLUE);
    vga_print(2, 2, "Could not open file:", VGA_YELLOW, VGA_DARK_BLUE);
    vga_print(3, 2, path,                   VGA_WHITE,  VGA_DARK_BLUE);
    vga_print(5, 2, "Press any key to exit.", VGA_CYAN, VGA_DARK_BLUE);
    vga_update();
    while (!japi_has_char()) vga_update();
}

int main(int argc, char **argv) {
    japi_init();

    jbe_state_t ed;
    jbe_init(&ed);

    /* First file into pane 0; with no args fall back to the scratch sandbox
       so real source can't be clobbered. */
    const char *first = (argc > 1) ? argv[1] : "A:scratch.txt";
    if (!jbe_load(&ed, first)) { show_open_error(first); return 2; }
    /* A second file opens the split into pane 1, then focus returns to the
       first file. Any further command-line files are ignored. */
    if (argc > 2) {
        if (!jbe_open_in_split(&ed, argv[2])) { show_open_error(argv[2]); return 2; }
        ed.active_pane = 0;
    }

    struct timespec ts = { 0, 10 * 1000 * 1000 };   /* ~10 ms idle */
    for (;;) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        ed.caret_on = (((now.tv_sec * 1000u + now.tv_nsec / 1000000u) / 500u) & 1u) != 0;
        jbe_render(&ed);
        vga_update();
        while (japi_has_char()) {
            uint16_t k = japi_get_char();
            jbe_handle_key(&ed, k);
            /* File->New / Open / Close all act on the active pane's document. */
            if (ed.new_request)   { ed.new_request   = false; jbe_new(&ed); }
            if (ed.open_request)  { ed.open_request  = false; jbe_load(&ed, ed.open_path); }
            if (ed.save_request)  { ed.save_request  = false; jbe_save(&ed); }
            if (ed.close_request) { ed.close_request = false; jbe_close_active(&ed); }
            if (ed.quit) {
                jbe_free(&ed);
                return 0;
            }
        }
        nanosleep(&ts, 0);
    }
}
