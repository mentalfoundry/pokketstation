# Build pk_timing_bench.mcs using a real GNU ARM cross-assembler - no
# Python, no scripting language dependency.
#
# Requires: the "GNU Arm Embedded Toolchain" (arm-none-eabi-as/ld/objcopy -
# https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads, or the
# equivalent xPack prebuilt release), plus any C compiler for pack.c.

AS      ?= arm-none-eabi-as
LD      ?= arm-none-eabi-ld
OBJCOPY ?= arm-none-eabi-objcopy
CC      ?= cc

ASFLAGS = -mcpu=arm7tdmi -I src

SRCS = src/header.s src/icon.s src/mirrors.s src/font.s src/thumb_loop.s \
       src/start.s src/helpers.s src/experiments.s src/ui.s
OBJS = $(SRCS:.s=.o)

all: pk_timing_bench.mcs

# Standard PS1 memory-card icon (palette+bitmap, body offset 0x60-0xFF) -
# separate from the PocketStation-specific browse icon in icon.s. Converted
# from a source BMP at build time so changing the icon later never needs
# manual re-embedding. See icon_convert.c and README.md.
icon_convert: icon_convert.c
	$(CC) -O2 -o icon_convert icon_convert.c

assets/card_icon.bin: assets/card_icon.bmp icon_convert
	./icon_convert assets/card_icon.bmp assets/card_icon.bin

src/header.o: src/header.s src/constants.inc assets/card_icon.bin
	$(AS) $(ASFLAGS) -o $@ src/header.s

src/%.o: src/%.s src/constants.inc
	$(AS) $(ASFLAGS) -o $@ $<

body.elf: $(OBJS) link.ld
	$(LD) -T link.ld -o body.elf $(OBJS)

body.bin: body.elf
	$(OBJCOPY) -O binary --pad-to=0x02002000 --gap-fill=0x00 body.elf body.bin

pack: pack.c
	$(CC) -O2 -o pack pack.c

pk_timing_bench.mcs: body.bin pack
	./pack body.bin pk_timing_bench.mcs

clean:
	rm -f $(OBJS) body.elf body.bin pack pack.exe icon_convert icon_convert.exe \
	      assets/card_icon.bin pk_timing_bench.mcs

.PHONY: all clean
