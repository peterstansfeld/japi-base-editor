# Japi Base Editor — host build.
#
# Links the editor *component* with JapiBase's canonical host simulator. The
# platform headers and the simulator are the single source in JapiBase; only
# the editor lives in this repo. Dependency-free (gcc only).
CC       = gcc
JAPIBASE = ../../JapiBase
JAPIINC  = $(JAPIBASE)/Japi Base Pico 2
SIM      = $(JAPIBASE)/sim
CFLAGS   = -std=c11 -Wall -Wextra -O2 -DJAPI_SIM -I"$(JAPIINC)" -I"$(SIM)" -I"$(SIM)/shim"

# The canonical simulator source (kept in JapiBase).
SIMSRC = "$(SIM)/japi_sim.c"

sim_demo: sim_demo.c
	$(CC) $(CFLAGS) -o sim_demo $(SIMSRC) sim_demo.c

sim_test: sim_test.c
	$(CC) $(CFLAGS) -o sim_test $(SIMSRC) sim_test.c

# --- Japi Base Editor (JBE) ---------------------------------------------
# The same editor source builds for both backends; here we link the host sim.
EDITORINC = -I../editor
JBE_SRC   = ../editor/jbe.c ../editor/ui_filelist.c

jbe: ../editor/jbe_main.c ../editor/jbe.h ../editor/ui_filelist.h
	$(CC) $(CFLAGS) $(EDITORINC) -o jbe $(SIMSRC) $(JBE_SRC) ../editor/jbe_main.c

jbe_test: ../editor/jbe_test.c ../editor/jbe.h ../editor/ui_filelist.h
	$(CC) $(CFLAGS) $(EDITORINC) -I. -o jbe_test $(SIMSRC) $(JBE_SRC) ../editor/jbe_test.c

test: sim_test jbe_test
	./sim_test
	./jbe_test

# Quick interactive try-out: writes its own source into A:jbe.c then opens it.
demo: jbe
	mkdir -p simdisk_A
	cp ../editor/jbe.c simdisk_A/jbe.c
	./jbe A:jbe.c

clean:
	rm -f sim_demo sim_test seam_app jbe jbe_test

.PHONY: clean test
