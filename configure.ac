#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.68])
AC_INIT([obe], [1.5], [stoth@kernellabs.com])
AM_INIT_AUTOMAKE
AC_CONFIG_MACRO_DIR([m4])
AC_CANONICAL_HOST
AM_SILENT_RULES([yes])

# Checks for programs.
AC_CHECK_PROGS(YASM, yasm)
AC_PROG_OBJC
AC_PROG_CC
AC_PROG_CC_C99
AC_PROG_CXX
AM_PROG_AS
AX_PROG_YASM()
#CCAS=yasm
#CCASFLAGS=

#AC_SUBST(CCAS)
#AC_SUBST(CCASFLAGS)

# We always build with -fPIC in case our static libraries end up
# being linked into a consumer's shared library
AC_MSG_CHECKING(whether fPIC compiler option is accepted)
SAVED_CFLAGS="$CFLAGS -DKL_USERSPACE"
CFLAGS="$CFLAGS -fPIC -Werror "
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [return 0;])],
    [AC_MSG_RESULT(yes)
     CFLAGS="$SAVED_CFLAGS -fPIC"],
    [AC_MSG_RESULT(no)
     CFLAGS="$SAVED_CFLAGS"])

AC_PROG_RANLIB
AC_PROG_LIBTOOL

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h fcntl.h netinet/in.h stdint.h stdlib.h string.h sys/ioctl.h sys/socket.h sys/time.h unistd.h])
AC_CHECK_HEADERS([fdk-aac/aacdecoder_lib.h],
                 [break],
                 [AC_MSG_ERROR([libfdk-aac headers not found or not usable])])
AC_CHECK_HEADERS([libavutil/avutil.h],
                 [break],
                 [AC_MSG_ERROR([libavutil headers not found or not usable])])
AC_CHECK_HEADERS([libyuv.h],
                 [break],
                 [AC_MSG_ERROR([libyuv headers not found or not usable])])
AC_CHECK_HEADERS([libklvanc/vanc.h],
                 [break],
                 [AC_MSG_ERROR([libklvanc headers not found or not usable])])
AC_CHECK_HEADERS([libklscte35/scte35.h],
                 [break],
                 [AC_MSG_ERROR([libklscte35 headers not found or not usable])])
AC_CHECK_HEADERS([x264.h],
                 [break],
                 [AC_MSG_ERROR([libx264 headers not found or not usable])])
AC_CHECK_HEADERS([readline/readline.h],
                 [break],
                 [AC_MSG_ERROR([libreadline headers not found or not usable])])
AC_CHECK_HEADERS([libmpegts.h],
                 [break],
                 [AC_MSG_ERROR([libmpegts headers not found or not usable])])
AC_CHECK_HEADERS([libzvbi.h],
                 [break],
                 [AC_MSG_ERROR([libzvbi headers not found or not usable])])
AC_CHECK_HEADERS([twolame.h],
                 [break],
                 [AC_MSG_ERROR([libtwolame headers not found or not usable])])
AC_CHECK_HEADERS([alsa/asoundlib.h], HEADER_ASOUND="yes")
if test "x$HEADER_ASOUND" == "xyes" ; then
    LDFLAGS+=" -lasound "
fi

AC_LANG_PUSH([C++])
AC_CHECK_HEADERS([DTAPI.h], HEADER_DEKTECLIB="yes")
AC_LANG_POP([C++])
if test "x$HEADER_DEKTECLIB" == "xyes" ; then
    LDFLAGS+=" "
fi
AM_CONDITIONAL(DEKTECLIB, test x"$HEADER_DEKTECLIB" == x"yes")

AC_LANG_PUSH([Objective C++])
AC_CHECK_HEADERS([AVFoundation/AVFoundation.h], HEADER_AVFOUNDATION="yes")
AC_LANG_POP([Objective C++])
if test "x$HEADER_AVFOUNDATION" == "xyes" ; then
    LDFLAGS+=" -framework AudioToolbox -lfdk-aac -lx264 -pthread -lm -pthread -framework VideoToolbox "
    LDFLAGS+=" -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework CoreServices "
    LDFLAGS+=" -framework Security -framework AVFoundation -framework Foundation "
fi
AM_CONDITIONAL(AVFOUNDATION, test x"$HEADER_AVFOUNDATION" == x"yes")

AC_CHECK_HEADERS([Processing.NDI.Lib.h], HEADER_NDILIB="yes")
if test "x$HEADER_NDILIB" == "xyes" ; then
#    LDFLAGS+=" -lndi "
    LDFLAGS+=" "
fi
AM_CONDITIONAL(NDILIB, test x"$HEADER_NDILIB" == x"yes")

# VEGA 3301 support
AC_CHECK_HEADERS([VEGA3301_cap_types.h], HEADER_VEGA3301LIB="yes")
if test "x$HEADER_VEGA3301LIB" == "xyes" ; then
    LDFLAGS+=" -lvega3301_capture -lvega3301_fpga_lib -lvega330x_venc -lm31_common -lrt "
fi
AM_CONDITIONAL(VEGA3301LIB, test x"$HEADER_VEGA3301LIB" == x"yes")

# VEGA 3311
AC_CHECK_HEADERS([VEGA3311_cap_types.h], HEADER_VEGA3311LIB="yes")
if test "x$HEADER_VEGA3311LIB" == "xyes" ; then
    LDFLAGS+=" -lvega3311_capture -lvega_bqb_venc -lvega3311_fpga_drv -lm30_common -lrt "
fi
AM_CONDITIONAL(VEGA3311LIB, test x"$HEADER_VEGA3311LIB" == x"yes")

AC_CHECK_HEADERS([BlueDriver_p.h], HEADER_BLUEFISH444="yes")
#if test "x$HEADER_LIBX265" == "xyes" ; then
#    LDFLAGS+=" -lx265 "
#fi
AM_CONDITIONAL(BLUEFISH444, test x"$HEADER_BLUEFISH444" == x"yes")

# Check for x265 (optional)
AC_CHECK_HEADERS([x265.h], HEADER_LIBX265="yes")
if test "x$HEADER_LIBX265" == "xyes" ; then
    LDFLAGS+=" -lx265 "
fi
AM_CONDITIONAL(LIBX265, test x"$HEADER_LIBX265" == x"yes")

## Check for libklsmpte2064 (optional)
AC_CHECK_HEADERS([libklsmpte2064/klsmpte2064.h], HEADER_LIBKLSMPTE2064="yes")
AM_CONDITIONAL(LIBKLSMPTE2064, test x"$HEADER_LIBKLSMPTE2064" == x"yes")

## Check for VAAPI (optional)
AC_CHECK_HEADERS([va/va.h], HEADER_LIBVA="yes")
AM_CONDITIONAL(LIBVA, test x"$HEADER_LIBVA" == x"yes")

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_UINT16_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_CHECK_FUNCS([memset strrchr])

# Add debug support
AC_ARG_ENABLE(debug,
  AS_HELP_STRING(
    [--enable-debug],
    [enable debugging, default: no]),
    [case "${enableval}" in
      yes) debug=true ;;
      no)  debug=false ;;
      *)   AC_MSG_ERROR([bad value ${enableval} for --enable-debug]) ;;
    esac],
    [debug=false])
AM_CONDITIONAL(DEBUG, test x"$debug" = x"true")
AM_COND_IF(DEBUG,
    AC_DEFINE(DEBUG, 1, [Define to 0 if this is a release build]),
    AC_DEFINE(DEBUG, 0, [Define to 1 or higher if this is a debug build]))

AC_ARG_ENABLE(websockets,
  AS_HELP_STRING(
    [--enable-websockets],
    [enable websockets, default: no]),
    [case "${enableval}" in
      yes) websockets=true ;;
      no)  websockets=false ;;
      *)   AC_MSG_ERROR([bad value ${enableval} for --enable-websockets]) ;;
    esac],
    [websockets=false])
AM_CONDITIONAL(WEBSOCKETS, test x"$websockets" = x"true")

AC_CONFIG_FILES([Makefile obe/Makefile])
AC_OUTPUT
