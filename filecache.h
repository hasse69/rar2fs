/*
    Copyright (C) 2009 Hans Beckerus (hans.beckerus@gmail.com)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    This program take use of the freeware "Unrar C++ Library" (libunrar)
    by Alexander Roshal and some extensions to it.

    Unrar source may be used in any software to handle RAR archives
    without limitations free of charge, but cannot be used to re-create
    the RAR compression algorithm, which is proprietary. Distribution
    of modified Unrar source in separate form or as a part of other
    software is permitted, provided that it is clearly stated in
    the documentation and source comments that the code may not be used
    to develop a RAR (WinRAR) compatible archiver.
*/

#ifndef FILECACHE_H_
#define FILECACHE_H_

#include <platform.h>
#include <sys/stat.h>
#include <pthread.h>

__extension__
struct filecache_entry {
        char *rar_p;
        char *file_p;
        char *link_target_p;
        short method;                /* for getxattr() */
        struct stat stat;
        off_t offset;                /* >0: offset in rar file (raw read) */
        off_t vsize_first;           /* >0: volume file size (raw read) */
        off_t vsize_real_first;
        off_t vsize_real_next;
        off_t vsize_next;
        short vno_base;
        short vno_first;
        short vlen;
        short vpos;
        short vtype;
        union {
                struct {
#ifndef WORDS_BIGENDIAN
                        unsigned int raw:1;
                        unsigned int multipart:1;
                        unsigned int image:1;
                        unsigned int fake_iso:1;
                        unsigned int force_dir:1;
                        unsigned int vsize_fixup_needed:1;
                        unsigned int encrypted:1;
                        unsigned int vsize_resolved:1;
                        unsigned int :19;
                        unsigned int unresolved:1;
                        unsigned int xdir:1;
                        unsigned int check_atime:1;
                        unsigned int direct_io:1;
                        unsigned int avi_tested:1;
                        unsigned int save_eof:1;
#else
                        unsigned int save_eof:1;
                        unsigned int avi_tested:1;
                        unsigned int direct_io:1;
                        unsigned int check_atime:1;
                        unsigned int xdir:1;
                        unsigned int unresolved:1;
                        unsigned int :19;
                        unsigned int vsize_resolved:1;
                        unsigned int encrypted:1;
                        unsigned int vsize_fixup_needed:1;
                        unsigned int force_dir:1;
                        unsigned int fake_iso:1;
                        unsigned int image:1;
                        unsigned int multipart:1;
                        unsigned int raw:1;
#endif
                } flags;
                uint32_t flags_uint32;
        };
};

#define LOCAL_FS_ENTRY ((void*)-1)
#define LOOP_FS_ENTRY ((void*)-2)

#define ABS_ROOT(s, path) \
        do { \
                (s) = alloca(strlen(path) + strlen(OPT_STR2(OPT_KEY_SRC,0)) + 1); \
                strcpy((s), OPT_STR2(OPT_KEY_SRC,0)); \
                strcat((s), path); \
        } while (0)

#define ABS_MP(s, path, file) \
        do { \
                int l = strlen(path); \
                /* add +2 in case of fake .iso */ \
                (s) = alloca(l + strlen(file) + 3 + 2); \
                strcpy((s), path); \
                if (l && path[l - 1] != '/') \
                        strcat((s), "/"); \
                strcat((s), file); \
        } while(0)

extern pthread_mutex_t file_access_mutex;

struct filecache_entry *
filecache_alloc(const char *path);

struct filecache_entry *
filecache_get(const char *path);

void
filecache_invalidate(const char *path);

struct filecache_entry *
filecache_clone(const struct filecache_entry *src);

void
filecache_copy(const struct filecache_entry *src, struct filecache_entry *dest);

void
filecache_freeclone(struct filecache_entry *dest);

void
filecache_init();

void
filecache_destroy();

#endif
