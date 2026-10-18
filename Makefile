# Mosaintosh: a Mac Plus on the ESP-Mosaico, host side.
#
# The emulator is umac (external/umac, with Musashi as its 68000). Everything
# here builds and runs on the Mac, so the machine can be booted, measured and
# debugged before any of it goes on the board.
#
# ROM and disk images are not part of this repository. Put your own in
# assets/ (ignored by git):
#   assets/MacPlus-v3.rom   128 KB Mac Plus v3 ROM (checksum 4D1F8172)
#   assets/System.img       raw, bootable HFS volume

CC ?= cc

# The machine. 4 MB is a fully expanded Plus and what System 7.5 needs. The
# screen is the largest square clear of the panel's rounded corners, framed
# (main/mac_display.h); umac needs the width a multiple of 32 and
# width*height/8 < 64 KB. Keep in step with projects/mosaintosh's main/CMakeLists.txt.
MEMSIZE     ?= 4096
DISP_WIDTH  ?= 416
DISP_HEIGHT ?= 416

ROM  ?= assets/MacPlus-v3.rom
DISK ?= assets/System.img

UMAC    := external/umac
MUSASHI := $(UMAC)/external/Musashi

UMAC_SRC    := $(UMAC)/src/disc.c $(UMAC)/src/main.c $(UMAC)/src/rom.c $(UMAC)/src/scc.c \
               $(UMAC)/src/via.c
MUSASHI_SRC := $(MUSASHI)/m68kcpu.c $(MUSASHI)/m68kdasm.c $(MUSASHI)/m68kops.c \
               $(MUSASHI)/softfloat/softfloat.c

UMAC_FLAGS := -I$(UMAC)/include -I$(MUSASHI) -DMUSASHI_CNF=\"../include/m68kconf.h\" \
              -DENABLE_DASM=1 -DUMAC_MEMSIZE=$(MEMSIZE) \
              -DDISP_WIDTH=$(DISP_WIDTH) -DDISP_HEIGHT=$(DISP_HEIGHT)
CFLAGS ?= -O2 -g

.PHONY: all boot board board-window disk clean

all: build/headless build/board_check

# Musashi generates its opcode tables; umac's own Makefile knows how.
$(MUSASHI)/m68kops.c:
	$(MAKE) -C $(UMAC) prepare

build/headless: host/headless.c $(UMAC_SRC) $(MUSASHI_SRC) $(MUSASHI)/m68kops.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(UMAC_FLAGS) -o $@ host/headless.c $(UMAC_SRC) $(MUSASHI_SRC) -lm

# Boot for 60 emulated seconds, screenshot every 5, convert to PNG.
boot: build/headless
	@mkdir -p build/boot
	@rm -f build/boot/*
	./build/headless -r $(ROM) -d $(DISK) -t 60 -s 5 -o build/boot/shot
	@python3 tools/pbm2png.py build/boot/*.pbm > /dev/null

# --- the firmware on the simulated board -----------------------------------
#
# main/ is the firmware. Here it is compiled unchanged against the bundled
# Mosaico board simulator (fake ESP-IDF, panel, touch and flash partitions),
# with the same tick rate and main-task stack as the firmware build.

BOARD           := sim/board
FREERTOS_HZ     ?= 1000
MAIN_TASK_STACK ?= 20480

# What goes in the macdisk partition: System 7.5.3 trimmed to 8 MB (see
# tools/trim_variants.sh; variant B keeps the 7.5.1 and 7.5.3 fixes).
BOARD_DISK ?= build/trim/B.img

FW_SRC  := $(wildcard main/*.c)
FW_HDR  := $(wildcard main/*.h)
BOARD_HDR := $(wildcard $(BOARD)/*.h $(BOARD)/include/*.h $(BOARD)/include/*/*.h)

BOARD_FLAGS := $(CFLAGS) -pthread $(UMAC_FLAGS) -Imain -I$(BOARD) -I$(BOARD)/include \
               -DCONFIG_FREERTOS_HZ=$(FREERTOS_HZ) \
               -DCONFIG_ESP_MAIN_TASK_STACK_SIZE=$(MAIN_TASK_STACK)
WARN := -Wall -Wextra -Werror

# umac and Musashi are third-party: built once, without our warning flags.
EMU_OBJ := $(patsubst %.c,build/emu/%.o,$(UMAC_SRC) $(MUSASHI_SRC))
build/emu/%.o: %.c $(MUSASHI)/m68kops.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(UMAC_FLAGS) -c -o $@ $<

FW_OBJ := $(patsubst %.c,build/obj/%.o,$(FW_SRC)) build/obj/board/board.o
build/obj/main/%.o: main/%.c $(FW_HDR) $(BOARD_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(BOARD_FLAGS) $(WARN) -c -o $@ $<
build/obj/board/board.o: $(BOARD)/board.c $(BOARD_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(BOARD_FLAGS) $(WARN) -c -o $@ $<

build/board_check: host/board_check.c $(FW_OBJ) $(EMU_OBJ)
	$(CC) $(BOARD_FLAGS) $(WARN) -o $@ $^ -lm

build/obj/board/board_window.o: $(BOARD)/board_window.m $(BOARD)/board.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -fobjc-arc -I$(BOARD) -c -o $@ $<

build/board_window: host/board_window.c build/obj/board/board_window.o $(FW_OBJ) $(EMU_OBJ)
	$(CC) $(BOARD_FLAGS) $(WARN) -framework Cocoa -o $@ $^ -lm

$(BOARD_DISK):
	bash tools/trim_variants.sh

disk: $(BOARD_DISK)

# Boot to the Finder, open About This Macintosh by touch; screenshots in
# build/check/.
board: build/board_check $(BOARD_DISK)
	@mkdir -p build/check
	@rm -f build/check/*
	./build/board_check $(ROM) $(BOARD_DISK); status=$$?; \
	    python3 tools/pbm2png.py build/check/*.ppm > /dev/null; exit $$status

board-window: build/board_window $(BOARD_DISK)
	./build/board_window $(ROM) $(BOARD_DISK)

clean:
	rm -rf build/headless* build/board_* build/obj build/emu build/boot build/check
