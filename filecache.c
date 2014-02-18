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

#include <platform.h>
#include <memory.h>
#include <string.h>
#include <wchar.h>
#include <libgen.h>
#include <errno.h>
#include <assert.h>
#include "debug.h"
#include "filecache.h"
#include "optdb.h"
#include "hash.h"

extern char *src_path;
pthread_mutex_t file_access_mutex;

#define PATH_CACHE_SZ  (1024)
static dir_elem_t path_cache[PATH_CACHE_SZ];

#define FREE_CACHE_MEM(e)\
        do {\
                if ((e)->name_p)\
                        free ((e)->name_p);\
                if ((e)->rar_p)\
                        free ((e)->rar_p);\
                if ((e)->file_p)\
                        free ((e)->file_p);\
                if ((e)->file2_p)\
                        free ((e)->file2_p);\
                if ((e)->link_target_p)\
                        free ((e)->link_target_p);\
                (e)->name_p = NULL;\
                (e)->rar_p = NULL;\
                (e)->file_p = NULL;\
                (e)->file2_p = NULL;\
        } while(0)

/*!
 *****************************************************************************
 *
 ****************************************************************************/
dir_elem_t *filecache_alloc(const char *path)
{
        uint32_t hash = get_hash(path, 0);
        dir_elem_t *p = &path_cache[(hash & (PATH_CACHE_SZ - 1))];
        if (p->name_p) {
                if (!strcmp(path, p->name_p))
                        return p;
                while (p->next_p) {
                        p = p->next_p;
                        if (hash == p->dir_hash && !strcmp(path, p->name_p))
                                return p;
                }
                p->next_p = malloc(sizeof(dir_elem_t));
                p = p->next_p;
                memset(p, 0, sizeof(dir_elem_t));
        }
        p->dir_hash = hash;
        return p;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
dir_elem_t *filecache_get(const char *path)
{
        uint32_t hash = get_hash(path, 0);
        dir_elem_t *p = &path_cache[hash & (PATH_CACHE_SZ - 1)];
        if (p->name_p) {
                while (p) {
                        /*
                         * Checking the full hash here will inflict a small
                         * cache hit penalty for the bucket but will   
                         * instead improve speed when searching a collision
                         * chain due to less calls needed to strcmp().
                         */
                        if (hash == p->dir_hash && !strcmp(path, p->name_p))
                                return p;
                        p = p->next_p;
                }
        }
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void filecache_invalidate(const char *path)
{
        int i;
        if (path) {
                uint32_t hash = get_hash(path, PATH_CACHE_SZ);
                printd(3, "Invalidating cache path %s\n", path);
                dir_elem_t *e_p = &path_cache[hash];
                dir_elem_t *p = e_p;

                /* Search collision chain */
                while (p->next_p) {
                        dir_elem_t *prev_p = p;
                        p = p->next_p;
                        if (p->name_p && !strcmp(path, p->name_p)) {
                                FREE_CACHE_MEM(p);
                                prev_p->next_p = p->next_p;
                                free(p);
                                /* Entry purged. We can leave now. */
                                return;
                        }
                }

                /*
                 * Entry not found in collision chain.
                 * Most likely it is in the bucket, but double check.
                 */
                if (e_p->name_p && !strcmp(e_p->name_p, path)) {
                        /* Need to relink collision chain */
                        if (e_p->next_p) {
                                dir_elem_t *tmp = e_p->next_p;
                                FREE_CACHE_MEM(e_p);
                                memcpy(e_p, e_p->next_p, 
                                            sizeof(dir_elem_t));
                                free(tmp);
                        } else {
                                FREE_CACHE_MEM(e_p);
                                memset(e_p, 0, sizeof(dir_elem_t));
                        }
                }
        } else {
                printd(3, "Invalidating all cache entries\n");
                for (i = 0; i < PATH_CACHE_SZ;i++) {
                        dir_elem_t *e_p = &path_cache[i];
                        dir_elem_t *p_next = e_p->next_p;
 
                        /* Search collision chain */
                        while (p_next) {
                                dir_elem_t *p = p_next;
                                p_next = p->next_p;
                                FREE_CACHE_MEM(p);
                                free(p);
                        }
                        FREE_CACHE_MEM(e_p);
                        memset(e_p, 0, sizeof(dir_elem_t));
                }
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
dir_elem_t *filecache_clone(const dir_elem_t *src)
{
        dir_elem_t* dest = malloc(sizeof(dir_elem_t));
        if (dest != NULL) {
                memcpy(dest, src, sizeof(dir_elem_t));
                errno = 0;
                if (src->name_p)
                        dest->name_p = strdup(src->name_p);
                if (src->rar_p)
                        dest->rar_p = strdup(src->rar_p);
                if (src->file_p)
                        dest->file_p = strdup(src->file_p);
                if (src->file2_p)
                        dest->file2_p = strdup(src->file2_p);
                if (src->link_target_p)
                        dest->link_target_p = strdup(src->link_target_p);
                if (errno != 0) {
                        filecache_freeclone(dest);
                        dest = NULL;
                }
        } 
        return dest;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
#define CP_ENTRY_F(f) dest->f = src->f
void filecache_copy(const dir_elem_t *src, dir_elem_t *dest)
{
        if (dest != NULL && src != NULL) {
                if (dest->rar_p) 
                        free(dest->rar_p); 
                if (src->rar_p)
                        dest->rar_p = strdup(src->rar_p);
                else
                        dest->rar_p = NULL;
                if (dest->file_p)
                        free(dest->file_p); 
                if (src->file_p)
                        dest->file_p = strdup(src->file_p);
                else
                        dest->file_p = NULL;
                if (dest->file2_p)
                        free(dest->file2_p); 
                if (src->file2_p)
                        dest->file2_p = strdup(src->file2_p);
                else
                        dest->file2_p = NULL;
                if (dest->link_target_p)
                        free(dest->link_target_p); 
                if (src->link_target_p)
                        dest->link_target_p = strdup(src->link_target_p);
                else
                        dest->link_target_p = NULL;

                CP_ENTRY_F(stat);
                CP_ENTRY_F(offset);
                CP_ENTRY_F(msize);
                CP_ENTRY_F(vsize_real);
                CP_ENTRY_F(vsize_next);
                CP_ENTRY_F(vno_base);
                CP_ENTRY_F(vno_max);
                CP_ENTRY_F(vlen);
                CP_ENTRY_F(vpos);
                CP_ENTRY_F(vtype);
                CP_ENTRY_F(method);
                CP_ENTRY_F(flags_uint32);
        }
}
#undef CP_ENTRY_F

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void filecache_freeclone(dir_elem_t *dest)
{
        FREE_CACHE_MEM(dest);
        free(dest);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void filecache_init()
{
        memset(path_cache, 0, sizeof(dir_elem_t)*PATH_CACHE_SZ);
        pthread_mutex_init(&file_access_mutex, NULL);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void filecache_destroy()
{
        filecache_invalidate(NULL);
        pthread_mutex_destroy(&file_access_mutex);
}

