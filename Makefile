TARGET = vshbgm-mod
OBJS = vshbgm.o utils/utils.o external/systemctrl_stubs.o

USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

INCDIR = . utils external
CFLAGS = -Os -G0 -Wall -fno-builtin-printf
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
PRX_EXPORTS = exports.exp
PSP_FW_VERSION = 500

LIBS = -lpspaudio -lpspaudiocodec -lpspctrl -lpsputility -lpspkernel

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

release: all
	psp-packer $(TARGET).prx
