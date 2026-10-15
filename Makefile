# The PC port, 32-bit, built with gcc.
#
#   make setup      checks the tools, makes the Python environment (.venv)
#   make extract    reads your disc image: the game's programs (disc/), its data (gamedata/), the split (asm/)
#                   ISO=<file> names the image; without it, the one .iso in this folder or the one above
#   make            builds port/build/bt3 (only what changed; -jN to use N cores)
#   make run        the game from its menus          make demo   the attract fight, in a window
#   make test       the attract fight without a window (what install.py's self-test does)
#   make clean      the build's output               make distclean   also asm/, disc/ and .venv (not gamedata/)
#
#   AUDIO=sdl (default) plays through SDL (port/src/gs/audio_sdl.c); AUDIO=none builds without sound
#   (port/src/null/audio_null.c, as iortcw's code/null: no device, the players stay silent).
#   NET=1 (default) builds in online play (port/src/gs/net*.c, state.c); NET=0 leaves it out
#   (port/src/null/net_null.c: never in a session, the online screen says it is not in the build).
#   TEXPACK=1 (default) builds in texture packs (port/src/gs/gs_texpack.c); TEXPACK=0 leaves them out (texpack_null.c).
#   PVR=1 (default) builds in the PowerVR renderer (BT3_GPU_API=pvr; port/src/gs/gs_pvr.c on gpu_pvr, a software
#   PowerVR). gpu_pvr's kos_pvr.cpp is GPL-2.0-only: a program built with it is not for release. PVR=0 leaves it out.
#
# The same commands as install.py --32 and port/tools/undefined.py (BT3_CC=gcc), as one rule per file.

ISO ?=
PVR ?= 1
AUDIO ?= sdl
NET ?= 1
TEXPACK ?= 1
BUILD  := port/build
OBJ    := $(BUILD)/obj
DATA   := $(BUILD)/obj_data
GEN_GS := $(BUILD)/gen/gs
EXE    := $(BUILD)/bt3
VENV   := .venv
PYTHON := $(VENV)/bin/python

CC  := gcc
CXX := g++

# --------------------------------------------------------------------------------------------------- the sources

# the main executable and the menu overlay (DBZP.BIN on the PS2; one program here). The VU0 files are replaced
# as a whole by port code (src/port/vu0_a.c, vu0_b.c; port/tools/portsrc.py).
REPLACED  := src/sys/vu0_a_c.c src/sys/vu0_a_c_b.c src/sys/vu0_a_c_c.c src/sys/vu0_b_c.c
GAME_SRC  := src/main.c $(filter-out $(REPLACED),$(sort $(wildcard src/sys/*.c src/battle/*.c src/menu/*.c)))
NL_SRC    := $(sort $(wildcard port/third_party/newlib_libm/*.c))
NET_SRC   := $(addprefix port/src/gs/,net.c net_chat.c net_match.c net_relay.c net_sign.c state.c)
GS_SRC    := $(filter-out $(if $(filter none,$(AUDIO)),port/src/gs/audio_sdl.c) $(if $(filter 0,$(NET)),$(NET_SRC)) \
               $(if $(filter 0,$(TEXPACK)),port/src/gs/gs_texpack.c),\
               $(sort $(wildcard port/src/gs/*.c)))
CXX_SRC   := $(sort $(wildcard port/third_party/imgui/*.cpp port/src/gs/*.cpp))
VU0_SRC   := src/port/vu0_a.c src/port/vu0_b.c
PC_SRC    := $(sort $(wildcard port/src/*.c))

# object names as undefined.py gives them (link.py and make_state.py go by them): src/sys/x.c -> sys_x.o, ...
GAME_OBJ  := $(foreach f,$(GAME_SRC),$(OBJ)/$(subst /,_,$(patsubst src/%.c,%,$(f))).o)
NL_OBJ    := $(patsubst port/third_party/newlib_libm/%.c,$(OBJ)/nl_%.o,$(NL_SRC))
GS_OBJ    := $(patsubst port/src/gs/%.c,$(OBJ)/gs_%.o,$(GS_SRC))
CXX_OBJ   := $(foreach f,$(CXX_SRC),$(OBJ)/cxx_$(basename $(notdir $(f))).o)
PC_OBJ    := $(patsubst src/port/%.c,$(OBJ)/pc_%.o,$(VU0_SRC)) $(patsubst port/src/%.c,$(OBJ)/pc_%.o,$(PC_SRC))
# gpu_pvr (port/third_party/gpu_pvr): named gs_ / cxx_ so that make_state.py counts them as the port's, not the game's
PVR_DIR     := port/third_party/gpu_pvr
PVR_CXX_SRC := $(PVR_DIR)/libpvr/pvr.cpp $(PVR_DIR)/libpvr/ta.cpp $(PVR_DIR)/libpvr/core.cpp $(PVR_DIR)/libpvr/core_gl.cpp \
               $(PVR_DIR)/libpvr/texture.cpp \
               $(PVR_DIR)/kos_pvr.cpp
PVR_C_SRC   := $(PVR_DIR)/kos_prim.c $(PVR_DIR)/pvr_host.c
PVR_CXX_OBJ := $(foreach f,$(PVR_CXX_SRC),$(OBJ)/cxx_pvrlib_$(basename $(notdir $(f))).o)
PVR_C_OBJ   := $(foreach f,$(PVR_C_SRC),$(OBJ)/gs_pvrlib_$(basename $(notdir $(f))).o)
PVR_OBJ     := $(if $(filter 1,$(PVR)),$(PVR_CXX_OBJ) $(PVR_C_OBJ))

# AUDIO=none: the sound output that has none (port/src/null)
NULL_OBJ  := $(if $(filter none,$(AUDIO)),$(OBJ)/pc_audio_null.o) $(if $(filter 0,$(NET)),$(OBJ)/pc_net_null.o) \
             $(if $(filter 0,$(TEXPACK)),$(OBJ)/pc_texpack_null.o)

OBJS      := $(GAME_OBJ) $(NL_OBJ) $(GS_OBJ) $(CXX_OBJ) $(PC_OBJ) $(PVR_OBJ) $(NULL_OBJ)

# ------------------------------------------------------------------------------------------------------ the flags

# the game: software float (port/src/softfloat_ps2.c), PS2 structure layout
# (no -w: the only warning the game code gives otherwise is -Wattributes; a call to a function not declared is an
#  error, as gnu89 would take it as int f(int ...) and pass a 64-bit argument as 32 bits)
GAME_CFLAGS := -m32 -std=gnu89 -c -O2 -g -fno-strict-aliasing -ffp-contract=off -fcommon -Wno-attributes -Werror=implicit-function-declaration -fno-pic -fno-stack-protector \
               -fno-builtin -msoft-float -mno-sse -mno-mmx -malign-double -include port_libm.h -Iinclude -Iport/include \
               -include port_compat.h
# the PS2's maths library, software float
NL_CFLAGS   := -m32 -std=gnu89 -c -O2 -g -fno-pic -msoft-float -mno-sse -mno-mmx -malign-double -fno-strict-aliasing \
               -ffp-contract=off -fno-builtin -w -Iport/include -Iport/third_party/newlib_libm -include newlib_shim.h
# the renderer and the settings window: ordinary host code, hardware float
GS_CFLAGS   := -m32 -std=gnu99 -c -O2 -g -fno-pic -msse2 -mfpmath=sse -fno-strict-aliasing -Wall -Wno-unused \
               -Wno-misleading-indentation -I$(GEN_GS) -MMD -MP
CXXFLAGS    := -m32 -std=c++17 -c -O2 -g -fno-pic -msse2 -mfpmath=sse -fno-exceptions -fno-rtti -w \
               -Iport/third_party/imgui -Iport/src/gs -I$(GEN_GS) -MMD -MP
# the port's own C: software float except the host maths (plat_libm.c, plat_fastvec.c) and the VU1 programs (vu1_native.c)
PC_CFLAGS   := -m32 -std=gnu99 -c -O2 -g -fno-pic -malign-double -fno-strict-aliasing -ffp-contract=off -fno-builtin -w \
               -Iinclude -Iport/include -Iport/src $(BT3_PORT_CFLAGS)
PC_SOFT     := -msoft-float -mno-sse -mno-mmx -DSF_FAST_CALLS -include math.h -include stdlib.h -include port_libm.h
PC_HARD     := plat_libm plat_fastvec vu1_native

# the PS2 reference is linked (port/src/gs/gs_core.c, gs_vu1.c): what records are compared against
# (BT3_RND=0); the Dreamcast build has the front end without it (gs_walk.c)
GAME_CFLAGS += -DPORT_GS_REF
GS_CFLAGS   += -DPORT_GS_REF
PC_CFLAGS   += -DPORT_GS_REF

CC_EE := $(PYTHON) port/tools/cc_ee.py

PVR_INC   := -I$(PVR_DIR) -I$(PVR_DIR)/kos
PVR_FLAGS := -m32 -O2 -g -msse2 -mfpmath=sse -w -MMD -MP
ifeq ($(PVR),1)
GS_CFLAGS += -DBT3_PVR
endif
# which PVR the objects were built with: a change rebuilds the two files that look at it (gs_draw.c, gs_pvr.c), and drops gpu_pvr's
# objects when it is off (link.py links every object in obj/)
PVR_FLAG := $(OBJ)/pvr.flag
$(shell mkdir -p $(OBJ); if [ "$$(cat $(PVR_FLAG) 2>/dev/null)" != "$(PVR)" ]; then echo $(PVR) > $(PVR_FLAG); \
        [ "$(PVR)" = 1 ] || rm -f $(OBJ)/cxx_pvrlib_* $(OBJ)/gs_pvrlib_*; fi)
# which sound output the objects were built with: link.py links every object there, so the other one's goes
AUDIO_FLAG := $(OBJ)/audio.flag
$(shell mkdir -p $(OBJ); if [ "$$(cat $(AUDIO_FLAG) 2>/dev/null)" != "$(AUDIO)" ]; then echo $(AUDIO) > $(AUDIO_FLAG); \
        rm -f $(OBJ)/gs_audio_sdl.o $(OBJ)/pc_audio_null.o; fi)
NET_FLAG := $(OBJ)/net.flag
$(shell mkdir -p $(OBJ); if [ "$$(cat $(NET_FLAG) 2>/dev/null)" != "$(NET)" ]; then echo $(NET) > $(NET_FLAG); \
        rm -f $(OBJ)/pc_net_null.o $(patsubst port/src/gs/%.c,$(OBJ)/gs_%.o,$(NET_SRC)); fi)
TEXPACK_FLAG := $(OBJ)/texpack.flag
$(shell mkdir -p $(OBJ); if [ "$$(cat $(TEXPACK_FLAG) 2>/dev/null)" != "$(TEXPACK)" ]; then echo $(TEXPACK) > $(TEXPACK_FLAG); \
        rm -f $(OBJ)/pc_texpack_null.o $(OBJ)/gs_gs_texpack.o; fi)

# ------------------------------------------------------------------------------------------------------ the rules

.PHONY: all setup extract run demo test pvr clean distclean
.DELETE_ON_ERROR:

all: $(EXE)

setup:
	python3 port/tools/mk.py setup

extract: | $(PYTHON)
	$(PYTHON) port/tools/mk.py extract "$(ISO)"

$(PYTHON):
	@echo "No Python environment yet: run 'make setup' first." >&2; false

asm/data:
	@echo "No extracted game yet: run 'make extract' first (ISO=<your disc image> if it is not in this folder)." >&2; false

$(OBJ) $(GEN_GS):
	mkdir -p $@

# the game's data tables, from the split executable (port/tools/gen_data.py assembles them into obj_data/)
$(DATA)/.stamp: port/tools/gen_data.py | asm/data $(PYTHON)
	$(PYTHON) port/tools/gen_data.py --asm asm
	touch $@

# the shaders and the side bars' pictures, embedded as C arrays (headers only rewritten when they change)
$(GEN_GS)/.stamp: port/tools/gen_shaders.py $(wildcard port/src/gs/shaders/*) $(wildcard port/assets/bars/*.png) | $(GEN_GS) $(PYTHON)
	$(PYTHON) port/tools/gen_shaders.py
	touch $@

$(OBJ)/main.o: src/main.c | $(OBJ) $(PYTHON)
	$(CC_EE) --game $@ $< -- $(CC) $(GAME_CFLAGS) -I$(<D)
$(OBJ)/sys_%.o: src/sys/%.c | $(OBJ) $(PYTHON)
	$(CC_EE) --game $@ $< -- $(CC) $(GAME_CFLAGS) -I$(<D)
$(OBJ)/battle_%.o: src/battle/%.c | $(OBJ) $(PYTHON)
	$(CC_EE) --game $@ $< -- $(CC) $(GAME_CFLAGS) -I$(<D)
$(OBJ)/menu_%.o: src/menu/%.c | $(OBJ) $(PYTHON)
	$(CC_EE) --game $@ $< -- $(CC) $(GAME_CFLAGS) -I$(<D)

$(OBJ)/nl_%.o: port/third_party/newlib_libm/%.c | $(OBJ) $(PYTHON)
	$(CC_EE) $@ $< -- $(CC) $(NL_CFLAGS)

$(OBJ)/gs_%.o: port/src/gs/%.c | $(OBJ) $(GEN_GS)/.stamp
	$(CC) $(GS_CFLAGS) $< -o $@
# the build key of a release (port/tools/buildkey.py): only this file sees it; none without BT3_BUILD_KEY
$(OBJ)/gs_net_sign.o: GS_CFLAGS += $(shell $(PYTHON) -c "import sys; sys.path.insert(0, 'port/tools'); import buildkey; print(' '.join(buildkey.flags()))")

$(OBJ)/gs_gs_draw.o $(OBJ)/gs_gs_pvr.o: $(PVR_FLAG)
$(OBJ)/gs_gs_pvr.o: GS_CFLAGS += $(PVR_INC)

# gpu_pvr itself: libpvr is the hot path of the PC build of this renderer, optimised as gpu_pvr.mk has it
$(filter-out %/cxx_pvrlib_kos_pvr.o,$(PVR_CXX_OBJ)): $(OBJ)/cxx_pvrlib_%.o: $(PVR_DIR)/libpvr/%.cpp | $(OBJ)
	$(CXX) $(PVR_FLAGS) -std=c++17 -I$(PVR_DIR) -c $< -o $@
$(OBJ)/cxx_pvrlib_kos_pvr.o: $(PVR_DIR)/kos_pvr.cpp | $(OBJ)
	$(CXX) $(PVR_FLAGS) -std=c++17 -I$(PVR_DIR) -c $< -o $@
$(PVR_C_OBJ): $(OBJ)/gs_pvrlib_%.o: $(PVR_DIR)/%.c | $(OBJ)
	$(CC) $(PVR_FLAGS) $(PVR_INC) -c $< -o $@

$(OBJ)/cxx_%.o: port/third_party/imgui/%.cpp | $(OBJ) $(GEN_GS)/.stamp
	$(CXX) $(CXXFLAGS) $< -o $@
$(OBJ)/cxx_%.o: port/src/gs/%.cpp | $(OBJ) $(GEN_GS)/.stamp
	$(CXX) $(CXXFLAGS) $< -o $@

# the vector-library references under the game's names: host-written, compiled as they are
$(OBJ)/pc_vu0_%.o: src/port/vu0_%.c | $(OBJ)
	$(CC) $(PC_CFLAGS) $(PC_SOFT) -include vu0_names.h -DREF_VU0_EXTERN_ARITH -MMD -MP $< -o $@
$(OBJ)/pc_audio_null.o: port/src/null/audio_null.c | $(OBJ)
	$(CC) $(PC_CFLAGS) -MMD -MP $< -o $@
$(OBJ)/pc_net_null.o: port/src/null/net_null.c | $(OBJ)
	$(CC) $(PC_CFLAGS) -MMD -MP $< -o $@
$(OBJ)/pc_texpack_null.o: port/src/null/texpack_null.c | $(OBJ)
	$(CC) $(PC_CFLAGS) -MMD -MP $< -o $@
$(OBJ)/pc_%.o: port/src/%.c | $(OBJ) $(PYTHON)
	$(if $(filter $*,$(PC_HARD)),$(CC) $(PC_CFLAGS) -MMD -MP $< -o $@,$(CC_EE) $@ $< -- $(CC) $(PC_CFLAGS) $(PC_SOFT))
# the VU1 programs as C: drawing only, the host's floats as the VU1 interpreter (gs_vu1.c) has them, so that the
# two give the same corners
$(OBJ)/pc_vu1_native.o: PC_CFLAGS += -msse2 -mfpmath=sse

# link.py: the link (with stubs for anything still missing), then the game-state map (make_state.py)
$(EXE): $(OBJS) $(DATA)/.stamp port/tools/link.py
	BT3_CC=gcc $(PYTHON) port/tools/link.py

run: $(EXE)
	port/run.sh menu

demo: $(EXE)
	port/run.sh demo

# the attract fight on the PowerVR renderer
pvr: $(EXE)
	BT3_GPU_API=pvr port/run.sh demo

test: $(EXE)
	@BT3_DEMO=1 BT3_SETTINGS= BT3_SAVES=$(BUILD)/selftest_saves BT3_GS=none timeout 300 ./$(EXE) > $(BUILD)/selftest.log 2>&1; \
	if grep -q "battle finished" $(BUILD)/selftest.log; then echo "self-test: the demo fight ran to its end"; \
	else echo "self-test FAILED (see $(BUILD)/selftest.log):"; tail -5 $(BUILD)/selftest.log; false; fi

clean:
	rm -rf $(OBJ) $(DATA) $(BUILD)/gen $(EXE) $(EXE).map $(EXE).mem $(BUILD)/stubs.txt $(BUILD)/undefined.txt

distclean: clean
	rm -rf asm disc $(VENV)

-include $(OBJS:.o=.d)
