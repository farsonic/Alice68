# Alice68-Network: the MUI network settings window. Built as Riptide's own
# RiptidePrefs is (its GCC build: m68k-linux-gnu-gcc, link.ld, elf2hunk.py),
# from its MUI layer in rt/ (see rt/README) and Alice68's netgui*.c.
# SPDX-License-Identifier: GPL-2.0-only
CROSS  ?= m68k-linux-gnu-
CC     := $(CROSS)gcc
LD     := $(CROSS)ld
PYTHON ?= python3
OUT    ?= ../out
B      := $(OUT)/netgui
GCCINC := $(shell $(CC) -print-file-name=include)
LIBGCC := $(shell $(CC) -m68020 -print-libgcc-file-name)
CFLAGS := -m68020 -mtune=68020-60 -O2 -std=gnu99 -ffreestanding -fno-builtin -fno-pic -fno-pie \
          -fomit-frame-pointer -msoft-float -nostdinc -isystem $(GCCINC) \
          -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector \
          -ffunction-sections -fdata-sections -Wall -Wno-unused-parameter \
          -Irt/include -Irt -Irt/os -I$(B) -I. -I.. -I../wifi -I../../third_party/Emu68/src/alice
LDFLAGS := -T rt/link.ld --emit-relocs --gc-sections --no-warn-rwx-segments

LIBS   := exec dos timer locale battclock intuition gadtools graphics icon
GEN_H  := $(foreach l,$(LIBS),$(B)/gen/$(l)_calls.h) $(B)/gen/muimaster_calls.h $(B)/gen/expansion_calls.h
RT_SRC := rt/libc.c rt/printf.c rt/prefs/mui_util.c rt/prefs/mui_theme.c rt/prefs/mui_classes.c rt/prefs/mui_lang.c \
          rt/prefs/mui_widgets.c rt/prefs/stat_util.c
SRC    := netgui.c
OBJ    := $(B)/startup.o $(patsubst %.c,$(B)/%.o,$(notdir $(RT_SRC)) $(SRC)) $(B)/mui_thunks.o

all: $(OUT)/Alice68-Network

$(B)/gen/%_calls.h: rt/os/os_lvos.txt rt/tools/gen_lvos.py
	@mkdir -p $(dir $@)
	$(PYTHON) rt/tools/gen_lvos.py call gcc rt/os/os_lvos.txt $* $@
$(B)/gen/expansion_calls.h: a68_lvos.txt rt/tools/gen_lvos.py
	@mkdir -p $(dir $@)
	$(PYTHON) rt/tools/gen_lvos.py call gcc a68_lvos.txt expansion $@
$(B)/gen/muimaster_calls.h: rt/prefs/mui_lvos.txt rt/tools/gen_lvos.py
	@mkdir -p $(dir $@)
	$(PYTHON) rt/tools/gen_lvos.py call gcc rt/prefs/mui_lvos.txt muimaster $@

vpath %.c rt rt/prefs .
$(B)/%.o: %.c $(GEN_H) $(wildcard *.h rt/prefs/*.h rt/os/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/startup.o: rt/startup.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/mui_thunks.o: rt/prefs/mui_thunks.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/Alice68-Network.elf: $(OBJ) rt/link.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJ) $(LIBGCC)
$(OUT)/Alice68-Network: $(B)/Alice68-Network.elf rt/tools/elf2hunk.py
	$(PYTHON) rt/tools/elf2hunk.py -v $< $@

clean:
	rm -rf $(B) $(OUT)/Alice68-Network
.PHONY: all clean
