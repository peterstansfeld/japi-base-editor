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

static void run_session(jbe_state_t *ed) {
    jbe_init(ed);
    jbe_new(ed);   /* one empty scratch document in pane 0 */

    for (;;) {
        jbe_render(ed);
        vga_update();

        while (japi_has_char()) {
            uint16_t k = japi_get_char();
            jbe_handle_key(ed, k);

            /* File->New / Open / Close all act on the active pane's document. */
            if (ed->new_request)   { ed->new_request   = false; jbe_new(ed); }
            if (ed->open_request)  { ed->open_request  = false; jbe_load(ed, ed->open_path); }
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
