# Change below to match current configuration
##########################

##########################
# This is the path (absolute or relative) to the folder containing
# the full portable "Unrar C++ library" (libunrar) sources 
UNRAR_SRC=

##########################
# This is the path (absolute or relative) to the folder containing
# the compiled libunrar.so.
# This can be left blank if the system already points out the location
# of a compatible unrarlib.so, eg. /lib.
UNRAR_LIB=

##########################
# This is the path (absolute or relative) to the folder containing
# the FUSE development header files.
# This can be left blank if the header files are already placed in
# some default location such as /usr/include.
FUSE_SRC=

##########################
# This is the path (absolute or relative) to the folder containing
# the FUSE library files.
# This can be left blank if the system already points out the location
# of a compatible FUSE library, eg. /lib.
# When using Fuse4x the path _must_ be set here or else auto-detection
# of Fuse4x library will fail.
FUSE_LIB=

##########################
# Does the host support glibc custom streams?
# If unsure try 'y' here. If linker fails to find e.g. fmemopen()
# your answer was most likely incorrect.
HAS_GLIBC_CUSTOM_STREAMS=y

##########################
# For Mac OS X, choose if 64-bit inodes (file serial number) should
# be supported or not. The default is _not_ to support it. But for
# version >= 10.6 (Snow Leopard) this is enabled by default in the
# Darwin kernel so it would sort of make sense saying 'y' here too.
# If using Fuse4x, leave it as 'n' or else auto-detection of Fuse4x
# library will fail!
# This option has no effect on any other OS so just leave it as is.
USE_OSX_64_BIT_INODES=n


##########################
# Change here to match your local platform or toolchain
ifndef CROSS
# Host/target platform using GCC
CC=gcc
CXX=g++
CFLAGS+=-g -fno-omit-frame-pointer
CXXFLAGS+=-g -fno-omit-frame-pointer
DEFINES=-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
STRIP=strip
LDFLAGS=
else
# Cross-compile using GCC
CC=$(CROSS)-gcc
CXX=$(CROSS)-g++
CFLAGS+=-g
CXXFLAGS+=-g
DEFINES=-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
STRIP=$(CROSS)-strip
LDFLAGS=
endif

