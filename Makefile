# Needs devkitARM + libnds + libfat (devkitPro). Run:  make
ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM, e.g. export DEVKITARM=/opt/devkitpro/devkitARM)
endif
ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO, e.g. export DEVKITPRO=/opt/devkitpro)
endif

TARGET  := KC32x_DSi
CC      := $(DEVKITARM)/bin/arm-none-eabi-gcc
NDSTOOL := $(DEVKITPRO)/tools/bin/ndstool
LIBNDS  := $(DEVKITPRO)/libnds
ARCH    := -mthumb -mthumb-interwork
CFLAGS  := -g -Wall -O2 -march=armv5te -mtune=arm946e-s -fomit-frame-pointer $(ARCH) -I$(LIBNDS)/include -DARM9
LDFLAGS := -specs=ds_arm9.specs -g $(ARCH) -L$(LIBNDS)/lib
LIBS    := -lfat -lnds9

all: $(TARGET).nds

$(TARGET).nds: $(TARGET).elf
	$(NDSTOOL) -c $@ -9 $< -b $(LIBNDS)/icon.bmp "Chaotix DSi;Proof of concept;32xKC"

$(TARGET).elf: source/main.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -f $(TARGET).elf $(TARGET).nds
