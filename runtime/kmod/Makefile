# hwcread.prx: the counter-reading kernel module (hwcread.c). Debug only.
TARGET = hwcread
OBJS = hwcread.o
BUILD_PRX = 1
PRX_EXPORTS = exports.exp
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1
CFLAGS = -O2 -G0 -Wall
LDFLAGS = -nostartfiles
LIBS =
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
