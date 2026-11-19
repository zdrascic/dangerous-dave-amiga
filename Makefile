# to generate assembler listing with LTO, add to LDFLAGS: -Wa,-adhln=$@.listing,--listing-rhs-width=200
# for better annotations add -dA -dP
# to generate assembler source with LTO, add to LDFLAGS: -save-temps=cwd

forward-to-backward = $(subst /,\,$1)

subdirs := $(wildcard */)
VPATH = $(subdirs)
cpp_sources := $(wildcard *.cpp) $(wildcard $(addsuffix *.cpp,$(subdirs)))
cpp_objects := $(addprefix obj/,$(patsubst %.cpp,%.o,$(notdir $(cpp_sources))))
# The game's sources live in src/ (picked up as a subdirectory below).
# tests/*.c are host-side unit tests (make test), never linked into the game.
c_sources := $(filter-out tests/%.c,$(wildcard *.c) $(wildcard $(addsuffix *.c,$(subdirs))))
PROG := main
c_objects := $(addprefix obj/,$(patsubst %.c,%.o,$(notdir $(c_sources))))
s_sources := support/gcc8_a_support.s support/depacker_doynax.s
s_objects := $(addprefix obj/,$(patsubst %.s,%.o,$(notdir $(s_sources))))
vasm_sources := libs/ptplayer/ptplayer.asm
vasm_objects := $(addprefix obj/,$(patsubst %.asm,%.o,$(notdir $(vasm_sources))))
iff_sources := $(wildcard *.iff)
iff_objects := $(addprefix obj/,$(patsubst %.iff,%_iff.o,$(iff_sources)))
objects := $(cpp_objects) $(c_objects) $(s_objects) $(vasm_objects) $(iff_objects)

# https://stackoverflow.com/questions/4036191/sources-from-subdirectories-in-makefile/4038459
# http://www.microhowto.info/howto/automatically_generate_makefile_dependencies.html

OUT = dave

# The game's asset packages: every file src/main.c loads, in the order it
# loads them (a floppy then reads the package front to back). ZIP-compatible,
# stored, 4-byte aligned (libs/sade/sade_package.h). dave.ocs holds the 5-plane graphics of
# assets/*/ocs, dave.aga the 8-plane ones of assets/*/aga; the rest is the
# same in both. src/main.c loads dave.aga on an AGA machine, else dave.ocs,
# and falls back to the loose OCS files when neither is there.
PACKAGE       = dave.ocs
PACKAGE_AGA   = dave.aga
PACKAGE_TOOL  = tools/make_package.py
PACKAGE_FILES_OF = assets/title/$(1)/dave_logo.iff assets/fonts/font8.iff \
                assets/game/$(1)/tiles.iff assets/game/$(1)/dave.iff assets/game/$(1)/hud.iff \
                assets/game/$(1)/monsters.iff assets/game/sounds.spk assets/game/demos.bin assets/game/dscores.dav \
                $(sort $(wildcard assets/game/levels/level*.map))
PACKAGE_FILES     = $(call PACKAGE_FILES_OF,ocs)
PACKAGE_AGA_FILES = $(call PACKAGE_FILES_OF,aga)

ifeq ($(OS),Windows_NT)
# py.exe lives in C:\Windows, which the VS Code compile task's minimal PATH
# keeps; a plain "python" (e.g. C:\Python312) is not on that PATH.
PYTHON = py -3
CC = C:/amiga-gcc/bin/m68k-amiga-elf-gcc
ELF2HUNK = elf2hunk
OBJDUMP = m68k-amiga-elf-objdump
OBJCOPY = m68k-amiga-elf-objcopy
VASM = vasmm68k_mot
MKDIR_OBJ = mkdir obj 2>nul || ver>nul
CLEAN_CMD = del /q obj\*.o obj\*.d obj\.stamp_* $(OUT).elf $(OUT).exe $(OUT).map $(OUT).s $(PACKAGE) $(PACKAGE_AGA) 2>nul || ver>nul
STAMP_CMD = (del /q obj\.stamp_* $(OUT).elf $(OUT).exe 2>nul || ver>nul) & echo.>obj\.stamp_$(PROG)
TEST_RUN = obj\clock_test.exe
PACKAGE_TEST_RUN = obj\package_test.exe
else
PYTHON = python3
AMIGA_EXT := $(lastword $(sort $(wildcard $(HOME)/.vscode/extensions/bartmanabyss.amiga-debug-*/bin/linux)))
CC = $(AMIGA_EXT)/opt/bin/m68k-amiga-elf-gcc
ELF2HUNK = $(AMIGA_EXT)/elf2hunk
OBJDUMP = $(AMIGA_EXT)/opt/bin/m68k-amiga-elf-objdump
OBJCOPY = $(AMIGA_EXT)/opt/m68k-amiga-elf/bin/objcopy
VASM = $(AMIGA_EXT)/vasmm68k_mot
MKDIR_OBJ = mkdir -p obj
CLEAN_CMD = rm -f obj/*.o obj/*.d obj/.stamp_* $(OUT).elf $(OUT).exe $(OUT).map $(OUT).s $(PACKAGE) $(PACKAGE_AGA)
STAMP_CMD = rm -f obj/.stamp_* $(OUT).elf $(OUT).exe && touch obj/.stamp_$(PROG)
TEST_RUN = obj/clock_test
PACKAGE_TEST_RUN = obj/package_test
# Objects left by a Windows build of this folder have C:/... paths in their
# .d files, which this make can't parse ("target pattern contains no '%'" —
# even for `make clean`), and LTO bytecode from a different GCC. Drop them
# so they rebuild here.
foreign_deps := $(shell grep -lE '[A-Za-z]:/' obj/*.d 2>/dev/null)
ifneq ($(foreign_deps),)
$(info Removing Windows-built objects: $(notdir $(foreign_deps:.d=.o)))
$(shell rm -f $(foreign_deps) $(foreign_deps:.d=.o))
endif
endif

# -flto-partition=one: one LTO unit, as for a small program — split into
# partitions, LTO marks symbols hidden, which this target warns about
CCFLAGS = -g -MP -MMD -m68000 -Ofast -nostdlib -Wextra -Wno-unused-function -Wno-volatile-register-var -fomit-frame-pointer -fno-tree-loop-distribution -flto -fwhole-program -fno-exceptions -flto-partition=one
CPPFLAGS= $(CCFLAGS) -fno-rtti -fcoroutines -fno-use-cxa-atexit
ASFLAGS = -Wa,-g,--register-prefix-optional
LDFLAGS = -Wl,--emit-relocs,-Ttext=0,-Map=$(OUT).map

all: $(OUT).exe $(PACKAGE) $(PACKAGE_AGA)

$(PACKAGE): $(PACKAGE_FILES) $(PACKAGE_TOOL)
	$(info Packing $@)
	@$(PYTHON) -I $(PACKAGE_TOOL) -o $@ $(PACKAGE_FILES)

$(PACKAGE_AGA): $(PACKAGE_AGA_FILES) $(PACKAGE_TOOL)
	$(info Packing $@)
	@$(PYTHON) -I $(PACKAGE_TOOL) -o $@ $(PACKAGE_AGA_FILES)

package: $(PACKAGE) $(PACKAGE_AGA)

obj:
	@$(MKDIR_OBJ)

$(OUT).exe: $(OUT).elf
	$(info Elf2Hunk $(OUT).exe)
	@$(ELF2HUNK) $(OUT).elf $(OUT).exe -s

# the stamp forces a relink after a build of another program in this folder
obj/.stamp_$(PROG): | obj
	@$(STAMP_CMD)

$(OUT).elf: $(objects) obj/.stamp_$(PROG)
	$(info Linking $(OUT).elf)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(objects) -o $@
	@$(OBJDUMP) --disassemble --no-show-raw-ins --visualize-jumps -S $@ >$(OUT).s

clean:
	$(info Cleaning...)
	@$(CLEAN_CMD)

# Run in FS-UAE (tools/run_fsuae.sh): an A500 with OCS, or an A4000 with AGA.
# Kickstart ROMs are not included: ROMS=dir, default ../roms (see README.md).
run: all
	@tools/run_fsuae.sh ocs

run-aga: all
	@tools/run_fsuae.sh aga

# host-side unit tests (the native compiler, not the Amiga one): tick
# timing must match on PAL (50 Hz) and NTSC (60 Hz); the package loader's
# ZIP parsing must read our packages and other writers' stored ZIPs
HOSTCC ?= gcc
TEST_ZIPS = obj/testzips
test: | obj
	$(info Building tests/clock_test.c (host))
	@$(HOSTCC) -std=c99 -Wall -Wextra -O2 tests/clock_test.c -o obj/clock_test
	@$(TEST_RUN)
	$(info Building tests/package_test.c (host))
	@$(PYTHON) -I tests/make_test_zips.py $(TEST_ZIPS) $(PACKAGE_FILES)
	@$(HOSTCC) -std=c99 -Wall -Wextra -O2 tests/package_test.c -o obj/package_test
	@$(PACKAGE_TEST_RUN) ok:$(TEST_ZIPS)/ok_sade.ocs ok:$(TEST_ZIPS)/ok_pyzip.zip \
		ok:$(TEST_ZIPS)/ok_comment.zip opt:$(TEST_ZIPS)/ok_infozip.zip \
		bad:$(TEST_ZIPS)/bad_deflate.zip bad:$(TEST_ZIPS)/bad_truncated.ocs

.PHONY: all clean test package run run-aga

-include $(objects:.o=.d)

$(cpp_objects) : obj/%.o : %.cpp | obj
	$(info Compiling $<)
	@$(CC) $(CPPFLAGS) -c -o $@ "$(CURDIR)/$<"

$(c_objects) : obj/%.o : %.c | obj
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ "$(CURDIR)/$<"

$(s_objects): obj/%.o : %.s | obj
	$(info Assembling $<)
	@$(CC) $(CCFLAGS) $(ASFLAGS) -c -o $@ "$(CURDIR)/$<"

$(vasm_objects): obj/%.o : libs/ptplayer/%.asm | obj
	$(info Assembling $< (vasm))
	@$(VASM) -m68000 -Felf -quiet -x -o $@ "$(CURDIR)/$<"

$(iff_objects): obj/%_iff.o : %.iff | obj
	$(info Embedding $<)
	@$(OBJCOPY) -I binary -O elf32-m68k -B m68k \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		--redefine-sym _binary_$*_iff_start=$*_iff_start \
		--redefine-sym _binary_$*_iff_end=$*_iff_end \
		--redefine-sym _binary_$*_iff_size=$*_iff_size \
		$< $@
