/* Pico entry-point body for JBE. Parallel to jbe_main.c (POSIX), but
   without command-line args, nanosleep, or process-exit semantics.
   On the Pico, JBE starts with a single empty scratch buffer and
   re-initialises on quit so File->Exit hands you a fresh editor rather
   than a hung CPU.

   The platform main() in "JBE Pico 2/main.c" calls jbe_pico_run() after
   japi_init(). This function never returns. */

#include <string.h>
#include "pico/stdlib.h"
#include "japi_base.h"
#include "jbe.h"
#include "calc_engine.h"    /* the embedded programmer's calculator (NumLock toggles it) */
#include "calc_ui.h"
#include "calc_classic.h"   /* the classic floating-point calculator (Shift+Tab switches) */

static void run_session(jbe_state_t *ed) {
    jbe_init(ed);
    jbe_new(ed);   /* one empty scratch document in pane 0 */

    /* The calculator is a full-screen mode toggled with NumLock -- a peer of
       the editor at this level, so the editor module needn't know about it.
       Its state is static: the ~520-byte CalcEngine must not sit on the small
       Core 0 stack, and keeping it persistent means a calculation survives
       toggling away and back. Same wiring as jbb_app.c on the full computer. */
    static CalcEngine  calc;           /* programmer's calc (calc_mode 0) */
    static ClassicCalc cc;             /* classic floating-point calc (calc_mode 1) */
    static bool calc_active = false, calc_help = false;
    static int  calc_mode = 0;         /* persists = the last-used calc */
    calc_engine_init(&calc);
    calc_engine_set_base(&calc, 10);   /* the calculator opens in decimal */
    calc.width = CALC_W16;             /* 16-bit: the binary view stays on one line */
    classic_init(&cc);

    for (;;) {
        jbe_render(ed);
        if (calc_active) {                                   /* calc floats on top */
            if (calc_mode == 0) calc_ui_render(&calc, calc_help);
            else                classic_render(&cc);
        }
        vga_update();

        while (japi_has_char()) {
            uint16_t k = japi_get_char();
            if (k == JAPI_KEY_NUM_LOCK) { calc_active = !calc_active; break; }  /* toggle */
            if (calc_active) {
                if (k == JAPI_KEY_STAB) { calc_mode = !calc_mode; break; }      /* Shift+Tab */
                if (k == JAPI_KEY_CTRL('C')) {        /* copy calc value -> editor clipboard */
                    char vbuf[48];
                    if (calc_mode == 0) calc_ui_copy(&calc, vbuf, sizeof vbuf);
                    else                classic_copy(&cc, vbuf, sizeof vbuf);
                    jbe_clip_set(ed, vbuf);
                    continue;
                }
                if (k == JAPI_KEY_CTRL('V')) {        /* paste a number from the clipboard */
                    if (ed->clip && ed->clip_len > 0) {
                        if (calc_mode == 0) calc_ui_paste(&calc, ed->clip);
                        else                classic_paste(&cc, ed->clip);
                    }
                    continue;
                }
                if (calc_mode == 0) calc_ui_handle_key(&calc, &calc_help, k);
                else                classic_handle_key(&cc, k);
                continue;
            }

            jbe_handle_key(ed, k);

            /* File->New / Open / Save / Close all act on the active pane's
               document. Save is deferred here (not on the deep key-handling
               stack) for the same reason as in jbb_app.c: a flash write from
               inside jbe_handle_key overflows the ~2 KB Core 0 stack. */
            if (ed->new_request)   { ed->new_request   = false; jbe_new(ed); }
            if (ed->open_request)  { ed->open_request  = false; jbe_load(ed, ed->open_path); }
            if (ed->save_request)  { ed->save_request  = false; (void)jbe_save(ed); }
            if (ed->close_request) { ed->close_request = false; jbe_close_active(ed); }

            if (ed->quit) {
                jbe_free(ed);
                return;
            }
        }
    }
}

void jbe_pico_run(void) {
    static jbe_state_t ed;
    for (;;) run_session(&ed);   /* File->Exit hands you a fresh editor */
}
