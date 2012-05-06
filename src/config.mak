SRCPATH=.
prefix=/usr/local
exec_prefix=${prefix}
bindir=${exec_prefix}/bin
libdir=${exec_prefix}/lib
includedir=${prefix}/include
ARCH=ARM
SYS=WINDOWS
CC=arm-mingw32ce-gcc
CFLAGS=-Wshadow -O1 -g  -Wall -I. -I$(SRCPATH) -std=gnu99 -fno-tree-vectorize -DDR400 -DWINCE
DEPMM=-MM -g0
DEPMT=-MT
LD=arm-mingw32ce-gcc -o 
LDFLAGS=
LIBX264=libx264.a
AR=arm-mingw32ce-ar rc 
RANLIB=arm-mingw32ce-ranlib
STRIP=arm-mingw32ce-strip
AS=
ASFLAGS= -DBIT_DEPTH=8
EXE=.exe
HAVE_GETOPT_LONG=1
DEVNULL=NUL
PROF_GEN_CC=-fprofile-generate
PROF_GEN_LD=-fprofile-generate
PROF_USE_CC=-fprofile-use
PROF_USE_LD=-fprofile-use
default: cli
install: install-cli
default: lib-static
install: install-lib-static
LDFLAGSCLI = 
CLI_LIBX264 = $(LIBX264)
