AUTOMAKE_OPTIONS = foreign
GIT_VERSION := $(shell git describe --abbrev=8 --dirty --always --tags)

VERSION_MAJOR = 3
VERSION_MINOR = 0
VERSION_PATCH = 4

BLACKMAGIC_SDK_PATH = $(top_srcdir)/include/decklink-sdk

# This will be ignored of libklmonitoring is not installed in the system.
#HISTOGRAMS = -DPRINT_HISTOGRAMS=1

CFLAGS += -g -Wall -DVERSION=\"$(VERSION)\" -DPROG="\"$(PACKAGE)\"" -D_FILE_OFFSET_BITS=64 -O3 \
	-D_BSD_SOURCE $(HISTOGRAMS) \
	-I$(BLACKMAGIC_SDK_PATH) \
	-I$(top_srcdir) \
	-DKL_USERSPACE -D__STDC_FORMAT_MACROS \
	-DGIT_VERSION=\"$(GIT_VERSION)\" \
	-DHAVE_DECKLINK \
	-DDEV_ABR=0 -DDEV_DOWN_UP=0 \
	-DVERSION_MAJOR=$(VERSION_MAJOR) \
	-DVERSION_MINOR=$(VERSION_MINOR) \
	-DVERSION_PATCH=$(VERSION_PATCH)

LDFLAGS += -lpthread -lz -lx264 -lyuv
LDFLAGS += -lavformat -lavcodec -ldl -lasound -lfdk-aac -lbz2 -lz -lswscale -lavutil -lm -lswresample
LDFLAGS += -lklscte35 -lklvanc -llzma
LDFLAGS += -ltwolame -lmpegts -lstdc++ -ldl -lzvbi -lreadline -lcurses -lltntstools
if LIBX265
LDFLAGS += -lx265
endif
if LIBVA
LDFLAGS += -L../../vaapi/libva-utils/common/.libs
LDFLAGS += -lva -lva-display -llzma
CFLAGS += -I../../vaapi/libva-utils/common
endif
if BLUEFISH444
BLUEFISH444_SDK=/home/stoth/BLUEFISH/EpochLinuxDriver_V5_11_0_27/release/driver
LDFLAGS += -L$(BLUEFISH444_SDK)/lib
LDFLAGS += -lBlueVelvetC64 -lBlueVelvet64 -lrt
CFLAGS += -I$(BLUEFISH444_SDK)/inc
endif
if WEBSOCKETS
LDFLAGS += -lwebsockets -ljson-c
endif
CXXFLAGS = $(CFLAGS) -std=c++11

bin_PROGRAMS  = obecli

x86_sdi.o:
	yasm -f elf -m amd64 -DARCH_X86_64=1 -DHAVE_CPUNOP=1 -I../common/x86/ -o x86_sdi.o ../input/sdi/x86/x86_sdi.asm

vfilter.o:
	yasm -f elf -m amd64 -DARCH_X86_64=1 -DHAVE_CPUNOP=1 -I../common/x86/ -o vfilter.o ../filters/video/x86/vfilter.asm

obecli_SOURCES  = obe.c
obecli_SOURCES += obecli.c
obecli_SOURCES += stream_formats.c
obecli_SOURCES += ../mux/smoothing.c
obecli_SOURCES += ../mux/ts/ts.c
obecli_SOURCES += ../output/ip/ip.c
obecli_SOURCES += ../output/file/file.c
obecli_SOURCES += ../input/sdi/ancillary.c
obecli_SOURCES += ../input/sdi/sdi.c
obecli_SOURCES += ../input/sdi/vbi.c
obecli_SOURCES += ../input/sdi/v210.c
obecli_SOURCES += ../input/sdi/smpte337_detector.c
obecli_SOURCES += ../input/sdi/smpte337_detector2.c
obecli_SOURCES += ../input/sdi/decklink/decklink.cpp
obecli_SOURCES += ../input/sdi/linsys/linsys.c
obecli_SOURCES += ../input/sdi/v4l2/v4l2.cpp
obecli_SOURCES += ../input/sdi/v210/v210fileinput.cpp
if BLUEFISH444
obecli_SOURCES += ../input/sdi/bluefish/bluefish.cpp
endif
if NDILIB
obecli_SOURCES += ../input/ndi/ndi.cpp
endif
obecli_SOURCES += ../filters/audio/audio.c
obecli_SOURCES += ../filters/audio/337m/337m.c
obecli_SOURCES += ../filters/video/cc.c
obecli_SOURCES += ../filters/video/video.c
obecli_SOURCES += ../filters/video/convert_jpeg.c
obecli_SOURCES += ../filters/video/analyze_fp.cpp
obecli_SOURCES += ../encoders/encoder_smoothing.c
obecli_SOURCES += ../encoders/audio/lavc/lavc.c
obecli_SOURCES += ../encoders/audio/mp2/twolame.c
obecli_SOURCES += ../encoders/audio/ac3bitstream/ac3bitstream.c
obecli_SOURCES += ../encoders/audio/ac3bitstream/klringbuffer.c
obecli_SOURCES += ../encoders/video/avc/x264.c
if LIBX265
obecli_SOURCES += ../encoders/video/hevc/x265.c
endif
if LIBVA
obecli_SOURCES += ../encoders/video/avc/avcodec.c
obecli_SOURCES += ../encoders/video/avc/vaapi.c
obecli_SOURCES += ../encoders/video/hevc/vaapi-hevc.c
endif
obecli_SOURCES += ../encoders/video/sei-timestamp.c
obecli_SOURCES += ../common/network/udp/udp.c
obecli_SOURCES += ../common/linsys/util.c
obecli_SOURCES += ../common/x86/x86inc.asm
obecli_SOURCES += ../common/x86/x86util.asm
obecli_SOURCES += ../common/common_lavc.c
obecli_SOURCES += ../common/queue.c
obecli_SOURCES += ltn_ws.c
obecli_SOURCES += osd.c
obecli_SOURCES += x86_sdi.o
obecli_SOURCES += vfilter.o

obecli_DEPENDENCIES  = x86_sdi.o
obecli_DEPENDENCIES += vfilter.o

obecli_LDFLAGS = vfilter.o x86_sdi.o

libklvanc_noinst_includedir = $(includedir)

noinst_HEADERS  = $(top_srcdir)/mux/mux.h
