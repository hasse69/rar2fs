/*
    Copyright (C) 2009-2014 Hans Beckerus (hans.beckerus@gmail.com)

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
#include <hash.h>

typedef struct dir_elem dir_elem;
__extension__
struct dir_elem {
        char *name_p;
        char *rar_p;
        char *file_p;
        char *file2_p;
        char *link_target_p;
        struct stat stat;
        uint32_t dir_hash;
        off_t offset;                /* >0: offset in rar file (raw read) */
        union {
                off_t vsize_first;   /* >0: volume file size (raw read) */
                off_t msize;         /* >0: mmap size */
        };
        off_t vsize_real;
        off_t vsize_next;
        short vno_base;
        short vno_max;
        short vlen;
        short vpos;
        short vtype;
        short method;                /* for getxattr() */
        union {
                struct {
#ifndef WORDS_BIGENDIAN
                        unsigned int raw:1;
                        unsigned int multipart:1;
                        unsigned int image:1;
                        unsigned int fake_iso:1;
                        unsigned int mmap:2;
                        unsigned int force_dir:1;
                        unsigned int vno_in_header:1;
                        unsigned int encrypted:1;
                        unsigned int :20;
                        unsigned int direct_io:1;
                        unsigned int avi_tested:1;
                        unsigned int save_eof:1;
#else
                        unsigned int save_eof:1;
                        unsigned int avi_tested:1;
                        unsigned int direct_io:1;
                        unsigned int :20;
                        unsigned int encrypted:1;
                        unsigned int vno_in_header:1;
                        unsigned int force_dir:1;
                        unsigned int mmap:2;
                        unsigned int fake_iso:1;
                        unsigned int image:1;
                        unsigned int multipart:1;
                        unsigned int raw:1;
#endif
                } flags;
                uint32_t flags_uint32;
        };
        struct dir_elem *next_p;
};
typedef struct dir_elem dir_elem_t;

#define LOCAL_FS_ENTRY ((void*)-1)

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

dir_elem_t *
filecache_alloc(const char *path);

dir_elem_t *
filecache_get(const char *path);

void
filecache_invalidate(const char *path);

dir_elem_t *
filecache_clone(const dir_elem_t *src);

void
filecache_copy(const dir_elem_t *src, dir_elem_t *dest);

void
filecache_freeclone(dir_elem_t *dest);

void
filecache_init();

void
filecache_destroy();

#endif
