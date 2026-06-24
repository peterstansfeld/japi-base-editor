# Japi Base Editor — host build.
#
# Links the editor *component* with JapiBase's canonical host simulator. The
# platform headers and the simulator are the single source in JapiBase; only
# the editor lives in this repo. Dependency-free (gcc only).
CC       = gcc
JAPIBASE = ../../JapiBase
JAPIINC  = $(JAPIBASE)/Japi Base Pico 2
SIM      = $(JAPIBASE)/sim
CALC     = ../JapiBaseCalc/src
CFLAGS   = -std=c11 -Wall -Wextra -O2 -DJAPI_SIM -I"$(JAPIINC)" -I"$(SIM)" -I"$(SIM)/shim" -I"$(CALC)"

# The canonical simulator source (kept in JapiBase). The simulator's own demos
# and tests live with it in JapiBase; this repo builds only the editor.
SIMSRC = "$(SIM)/japi_sim.c"

# --- Japi Base Editor (JBE) ---------------------------------------------
# The same editor source builds for both backends; here we link the host sim.
EDITORINC = -I../editor
JBE_SRC   = ../editor/jbe.c ../editor/ui_filelist.c

# The embedded calculator (NumLock), built from the JapiBaseCalc submodule.
CALC_SRC  = $(CALC)/calc.c $(CALC)/calc_engine.c $(CALC)/calc_ui.c \
            $(CALC)/calc_eval.c $(CALC)/calc_classic.c

jbe: ../editor/jbe_main.c ../editor/jbe.h ../editor/ui_filelist.h
	$(CC) $(CFLAGS) $(EDITORINC) -o jbe $(SIMSRC) $(JBE_SRC) $(CALC_SRC) ../editor/jbe_main.c

jbe_test: ../editor/jbe_test.c ../editor/jbe.h ../editor/ui_filelist.h
	$(CC) $(CFLAGS) $(EDITORINC) -I. -o jbe_test $(SIMSRC) $(JBE_SRC) ../editor/jbe_test.c

test: jbe_test
	./jbe_test

# Quick interactive try-out: writes its own source into A:jbe.c then opens it.
demo: jbe
	mkdir -p simdisk_A
	cp ../editor/jbe.c simdisk_A/jbe.c
	./jbe A:jbe.c

clean:
	rm -f jbe jbe_test

.PHONY: clean test
