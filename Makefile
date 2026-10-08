# Animal Forest PSP port: builds EBOOT.PBP from the recompiler output
# (work/recomp_out, see scripts/recompile.sh) and the runtime (runtime/).
# BUILDING.md has the whole procedure and the variables below.
#
#   ./build.sh   finds the dumps in roms/ and does everything, up to dist/AFPSP
#   gmake           -> build/psp/EBOOT.PBP
#   gmake install   -> copy the EBOOT, the ROM and the English text into PPSSPP's memory stick
#   gmake clean

PSPDEV ?= $(HOME)/pspdev
export PATH := $(PSPDEV)/bin:$(PATH)
PSPSDK := $(PSPDEV)/psp/sdk

BUILD ?= build/psp
GEN := work/recomp_out
TITLE := Animal Forest
TARGET := afpsp
# English dialogue built by scripts/make_text_en.sh from the user's own GameCube disc;
# packed into the EBOOT (see TEXT_PSAR below).
TEXT_EN ?= work/text/text_en.bin
NAMES_EN ?= work/text/names_en.bin
PPSSPP_GAME_DIR ?= $(HOME)/.config/ppsspp/PSP/GAME/AFPSP

CC := psp-gcc

GEN_SRCS := $(wildcard $(GEN)/funcs_*.c)
RT_DIRS := runtime/src runtime/src/audio runtime/src/english runtime/src/gfx
RT_SRCS := $(foreach d,$(RT_DIRS),$(wildcard $(d)/*.c))
RT_ASM_SRCS := $(foreach d,$(RT_DIRS),$(wildcard $(d)/*.S))

GEN_OBJS := $(patsubst $(GEN)/%.c,$(BUILD)/gen/%.o,$(GEN_SRCS))
RT_OBJS := $(patsubst runtime/src/%.c,$(BUILD)/rt/%.o,$(RT_SRCS)) $(patsubst runtime/src/%.S,$(BUILD)/rt/%.o,$(RT_ASM_SRCS))

INCLUDES := -Iruntime/include -Iruntime/src -I$(GEN) -I$(PSPDEV)/psp/include -I$(PSPSDK)/include
DEFINES := -D_PSP_FW_VERSION=600

COMMON_FLAGS := -G0 -mno-check-zero-division -fno-math-errno -fno-strict-aliasing -ffunction-sections -fdata-sections
GEN_OPT ?= -O2
GEN_CFLAGS := $(COMMON_FLAGS) $(GEN_OPT) -w $(INCLUDES) $(DEFINES) -DRECOMP_GENERATED_CODE
# gmake MEMCHECK=1 (in a BUILD directory of its own): the generated code checks
# every memory access against the plain address fold (MEM_PTR in recomp_psp.h)
# and logs the ones its fast form would treat differently. Slow; for verifying.
ifeq ($(MEMCHECK),1)
GEN_CFLAGS += -DRECOMP_MEM_CHECK
endif
RT_CFLAGS := $(COMMON_FLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-format $(INCLUDES) $(DEFINES)

LIBS := -lme-core -lpspsdk -lpspgu -lpsppower -lpsprtc -lpspaudio -lpspdisplay -lpspge -lpspctrl -lm
# --wrap=_sbrk: the game threads' stacks are reserved before the heap (runtime/src/sched.c).
LDFLAGS := -L$(PSPDEV)/psp/lib -L$(PSPSDK)/lib -specs=$(PSPSDK)/lib/prxspecs \
	-Wl,-q,-T$(PSPSDK)/lib/linkfile.prx -Wl,-zmax-page-size=128 -Wl,--gc-sections -Wl,--wrap=_sbrk

ELF := $(BUILD)/$(TARGET).elf
PRX := $(BUILD)/$(TARGET).prx
SFO := $(BUILD)/PARAM.SFO
EBOOT := $(BUILD)/EBOOT.PBP

.PHONY: all clean install gen-check

all: gen-check $(EBOOT)

gen-check:
	@test -f $(GEN)/funcs.h || { echo "Missing $(GEN): run scripts/recompile.sh first"; exit 1; }

$(BUILD)/gen/%.o: $(GEN)/%.c runtime/include/recomp_psp.h
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(GEN_CFLAGS) -c $< -o $@

# Header dependencies come from -MMD (the .d files included at the end).
$(BUILD)/rt/%.o: runtime/src/%.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(RT_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/rt/%.o: runtime/src/%.S
	@mkdir -p $(dir $@)
	@echo "AS  $<"
	@$(CC) -G0 $(INCLUDES) -c $< -o $@

# sections.c includes the generated section table.
$(BUILD)/rt/sections.o: $(GEN)/recomp_overlays.inl $(GEN)/funcs.h

# Every link gets a new build id (runtime/src/capture.c: a capture can only be
# resumed by the build that took it); build_id.txt has it for the tools.
$(ELF): $(GEN_OBJS) $(RT_OBJS)
	@echo "LD  $@"
	@id=$$(od -An -N4 -tx4 /dev/urandom | tr -d ' \n'); \
	 echo "const unsigned int rt_build_id = 0x$$id;" > $(BUILD)/build_id.c; \
	 echo "$$id" | tr a-f A-F > $(BUILD)/build_id.txt
	@$(CC) $(RT_CFLAGS) -c $(BUILD)/build_id.c -o $(BUILD)/build_id.o
	@$(CC) $(LDFLAGS) $(PSPSDK)/lib/prxexports.o $(RT_OBJS) $(BUILD)/build_id.o $(GEN_OBJS) $(LIBS) -o $@
	@psp-fixup-imports $@

$(PRX): $(ELF)
	@psp-prxgen $< $@

$(SFO):
	@mkdir -p $(dir $@)
	@mksfoex -d MEMSIZE=1 '$(TITLE)' $@

# EBOOT artwork: ICON0 is the XMB icon, PIC1 the background shown behind it.
# Both are made from the port's own render of the game's title screen, so
# nothing outside this repo is needed to build.
ICON0 := assets/ICON0.PNG
PIC1 := assets/PIC1.PNG
# The ROM the runtime loads. The English fan translation is a data-only patch of
# the JP build (same dmadata, same code segment bar five .text bytes, which
# recomp/af.jp.toml replicates), so it needs no separate recompile -- only this
# file swapped. The generated code is for one of the two ROMs (scripts/recompile.sh
# leaves which in rom.txt); override to play the other: BASEROM=work/af/baseroms/jp/baserom.z64
BASEROM ?= work/af/baseroms/$(firstword $(shell cat $(GEN)/rom.txt 2>/dev/null) en)/baserom.z64

ICON0_ARG = $(if $(wildcard $(ICON0)),$(ICON0),NULL)
PIC1_ARG = $(if $(wildcard $(PIC1)),$(PIC1),NULL)

# The English text goes into the EBOOT as its DATA.PSAR section, where the
# runtime reads it (runtime/src/data.c): an installed EBOOT needs no loose text
# files. Without them the game shows only what the ROM has.
TEXT_FILES := $(wildcard $(TEXT_EN) $(NAMES_EN))
TEXT_PSAR := $(BUILD)/text.psar
PSAR_ARG = $(if $(TEXT_FILES),$(TEXT_PSAR),NULL)

$(TEXT_PSAR): $(TEXT_FILES) tools/make_psar.py
	@mkdir -p $(dir $@)
	@python3 tools/make_psar.py $@ $(TEXT_FILES)

$(EBOOT): $(PRX) $(SFO) $(wildcard $(ICON0)) $(wildcard $(PIC1)) $(if $(TEXT_FILES),$(TEXT_PSAR))
	@pack-pbp $@ $(SFO) $(ICON0_ARG) NULL NULL $(PIC1_ARG) NULL $(PRX) $(PSAR_ARG) >/dev/null
	@echo "Built $@$(if $(filter 2,$(words $(TEXT_FILES))), with the English text,)"
	@$(if $(filter 2,$(words $(TEXT_FILES))),,echo "WARNING: $(TEXT_EN) and $(NAMES_EN) are not both there, so this EBOOT has no GameCube English text: run scripts/make_text_en.sh <disc image> and build again")

install: all
	@mkdir -p "$(PPSSPP_GAME_DIR)"
	@cp $(EBOOT) "$(PPSSPP_GAME_DIR)/EBOOT.PBP"
	@if [ -f $(BASEROM) ] && ! cmp -s $(BASEROM) "$(PPSSPP_GAME_DIR)/baserom.z64"; then \
		cp $(BASEROM) "$(PPSSPP_GAME_DIR)/baserom.z64"; \
		echo "Installed ROM $(BASEROM)"; \
	fi
	@if [ -f $(TEXT_EN) ] && ! cmp -s $(TEXT_EN) "$(PPSSPP_GAME_DIR)/text_en.bin"; then \
		cp $(TEXT_EN) "$(PPSSPP_GAME_DIR)/text_en.bin"; \
		echo "Installed English dialogue $(TEXT_EN)"; \
	fi
	@if [ -f $(NAMES_EN) ] && ! cmp -s $(NAMES_EN) "$(PPSSPP_GAME_DIR)/names_en.bin"; then \
		cp $(NAMES_EN) "$(PPSSPP_GAME_DIR)/names_en.bin"; \
		echo "Installed English names $(NAMES_EN)"; \
	fi
	@echo "Installed to $(PPSSPP_GAME_DIR)"

clean:
	rm -rf $(BUILD)

-include $(RT_OBJS:.o=.d)
