ACLOCAL_AMFLAGS = -I m4
AM_CFLAGS = -Wall 
AM_CXXFLAGS = $(UNRAR_CXXFLAGS) -Wall -Wno-reorder
AM_CPPFLAGS = $(UNRAR_CPPFLAGS) $(FUSE_CPPFLAGS)
bin_PROGRAMS = rar2fs mkr2i
dist_man_MANS = rar2fs.1

EXTRA_DIST = get-version.sh

rar2fs_SOURCES = 		dllext.cpp \
				optdb.c \
				filecache.c \
				iobuffer.c \
				sighandler.c \
				dirlist.c \
				rar2fs.c \
				compat.h \
				optdb.h \
				dirlist.h \
				hash.h \
				filecache.h \
				iobuffer.h \
				sighandler.h \
				debug.h \
				dllwrapper.h \
				index.h \
				platform.h \
				version.h \
				fileext.hpp \
				dllext.hpp

rar2fs_LDADD = $(PTHREAD_CFLAGS) $(LDFLAGS) $(UNRAR_LDFLAGS) $(FUSE_LDFLAGS) \
		$(PTHREAD_LIBS) $(FUSE_LIBS) $(UNRAR_LIBS) $(LIBICONV) -lstdc++
rar2fs_LINK = $(CC) -o $@
mkr2i_SOURCES = mkr2i.c
mkr2i_LDADD = $(LDFLAGS) $(UNRAR_LDFLAGS) $(FUSE_LDFLAGS)
mkr2i_LINK = $(CC) -o $@

if LINUX
install-exec-hook:
	$(MKDIR_P) $(DESTDIR)$(sbindir) && \
	rm -f $(DESTDIR)$(sbindir)/mount.rar2fs && \
	$(LN_S) $(bindir)/rar2fs $(DESTDIR)$(sbindir)/mount.rar2fs

uninstall-hook:
	rm -f $(DESTDIR)$(sbindir)/mount.rar2fs
endif

