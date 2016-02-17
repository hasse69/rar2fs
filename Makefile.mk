
include config.mk

ifdef DEBUG
CFLAGS+=-O0 -DDEBUG_=$(DEBUG)
CXXFLAGS+=-O0 -DDEBUG_=$(DEBUG)
else
CFLAGS+=-O2
CXXFLAGS+=-O2
endif

GITHEAD=.git/$(shell cat .git/HEAD 2>/dev/null | cut -d" " -f2)
GITREV=$(shell cut -c1-7 $(GITHEAD) 2>/dev/null)
ifneq ($(GITREV),)
CFLAGS+=-DGITREV_=0x$(GITREV)
endif

UNAME := $(shell uname)
MAKE := $(shell which gmake)

LIBS=-lstdc++ -lunrar -lpthread

# Try some default if path is not specified
ifeq ($(FUSE_SRC),)
FUSE_SRC=/usr/include/fuse
endif

ifeq ($(UNAME), Darwin)
# Is _DARWIN_C_SOURCE really needed ?
DEFINES+=-D_DARWIN_C_SOURCE
ifeq ($(USE_OSX_64_BIT_INODES), n)
ifeq "$(wildcard $(FUSE_LIB)/libfuse4x.dylib)" ""
ifeq "$(wildcard $(FUSE_LIB)/libosxfuse.dylib)" ""
LIBS+=-lfuse
else
LIBS+=-losxfuse
endif
else
LIBS+=-lfuse4x
endif
DEFINES+=-D_DARWIN_NO_64_BIT_INODE
else
ifeq "$(wildcard $(FUSE_LIB)/libfuse4x.dylib)" ""
ifeq "$(wildcard $(FUSE_LIB)/libosxfuse.dylib)" ""
LIBS+=-lfuse_ino64
else
LIBS+=-losxfuse
endif
else
LIBS+=-lfuse4x
endif
DEFINES+=-D_DARWIN_USE_64_BIT_INODE
endif
else
ifeq ($(UNAME), Linux)
CFLAGS+=-rdynamic
CXXFLAGS+=-rdynamic
endif
ifeq ($(UNAME), SunOS)
DEFINES+=-D_REENTRANT -D__EXTENSIONS__ 
endif
LIBS+=-lfuse 
endif

ifeq ("$(HAS_GLIBC_CUSTOM_STREAMS)", "y")
CONF+=-DHAS_GLIBC_CUSTOM_STREAMS_
endif
ifneq ("$(UCLIBC_STUBS)", "")
LIBS+=-lfmemopen
endif

ifeq ("$(MAKE)", "")
MAKE := make
endif

ifneq ("$(FUSE_SRC)", "")
INCLUDES+=-I$(FUSE_SRC)
endif
DEFINES+=-D_GNU_SOURCE
CFLAGS+=-std=gnu99 -Wall

C_COMPILE=$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(DEFINES) $(CONF) -DRARDLL -DFUSE_USE_VERSION=26 $(INCLUDES)
CXX_COMPILE=$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) $(DEFINES) -DRARDLL 
LINK=$(CC)
ifneq ("$(UNRAR_LIB)", "")
LIB_DIR=-L$(UNRAR_LIB)
endif
ifneq ("$(FUSE_LIB)", "")
LIB_DIR+=-L$(FUSE_LIB)
endif

OBJECTS=dllext.o optdb.o filecache.o iobuffer.o sighandler.o dirlist.o rar2fs.o
DEPS=.deps
UNRAR_SMPTEST=__unrar_smptest.cpp
UNRAR_SMPDEFS=-DRAR_SMP

all:	rar2fs mkr2i

$(UNRAR_SMPTEST):
	@echo "#define RAR_SMP" > $@
	@echo "#include <rar.hpp>" >> $@
	@echo "int main(){(void)GetNumberOfThreads();return 0;}" >> $@

clean:
	(cd stubs;$(MAKE) clean)
	rm -rf *.o *~ $(DEPS)
	@rm -rf $(UNRAR_SMPTEST)

clobber:
	(cd stubs;$(MAKE) clobber)
	rm -rf *.o *.P *.d rar2fs mkr2i *~ $(DEPS)

ifneq ("$(UCLIBC_STUBS)", "")
rar2fs:	$(UNRAR_SMPTEST) $(OBJECTS) 
	(cd stubs;$(MAKE) CROSS=$(CROSS))
else
rar2fs:	$(UNRAR_SMPTEST) $(OBJECTS)
endif
	$(LINK) -o rar2fs $(LDFLAGS) $(OBJECTS) $(LIB_DIR) $(LIBS)	

ifneq ("$(STRIP)", "")
	$(STRIP) rar2fs
endif

mkr2i:	mkr2i.o 
	$(LINK) -o mkr2i $(LDFLAGS) mkr2i.o
ifneq ("$(STRIP)", "")
	$(STRIP) mkr2i
endif

%.o : %.c
	@mkdir -p .deps
	$(C_COMPILE) -MD -I. -I$(UNRAR_SRC) -I$(FUSE_SRC) -c $<
	@cp $*.d $*.P; \
	sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
	-e '/^$$/ d' -e 's/$$/ :/' < $*.d >> $*.P; \
	mv $*.P $(DEPS); \
	rm -f $*.d

%.o : %.cpp
	$(eval CXX_COMPILE += $(shell $(CXX) -I$(UNRAR_SRC) -L$(UNRAR_LIB) $(UNRAR_SMPTEST) $(LIBS) 2>/dev/null && echo $(UNRAR_SMPDEFS)))
	@mkdir -p .deps
	$(CXX_COMPILE) -MD -I. -I$(UNRAR_SRC) -c $<
	@cp $*.d $*.P; \
	sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
	-e '/^$$/ d' -e 's/$$/ :/' < $*.d >> $*.P; \
	mv $*.P $(DEPS); \
	rm -f $*.d

-include $(OBJECTS:%.o=$(DEPS)/%.P)
