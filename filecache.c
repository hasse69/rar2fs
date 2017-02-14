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

#include "platform.h"
#include <memory.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include "hashtable.h"
#include "filecache.h"

#define FILECACHE_SZ  (1024)

/* Hash table handle */
static void *ht = NULL;

pthread_mutex_t file_access_mutex;

#define FREE_CACHE_MEM(e)\
        do {\
                free((e)->rar_p);\
                free((e)->file_p);\
                free((e)->file2_p);\
                if ((e)->link_target_p) {\
                        free((e)->link_target_p);\
                        (e)->link_target_p = NULL;\
                }\
                (e)->rar_p = NULL;\
                (e)->file_p = NULL;\
                (e)->file2_p = NULL;\
        } while(0)

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *__alloc()
{
        struct filecache_entry *e;
        e = malloc(sizeof(struct filecache_entry));
        if (e)
                memset(e, 0, sizeof(struct filecache_entry));
        return e;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __free(void *data)
{
        struct filecache_entry *e = data;
        if (e) 
                FREE_CACHE_MEM(e);
        free(e);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
struct filecache_entry *filecache_alloc(const char *path)
{
        struct hash_table_entry *hte;
        hte = hashtable_entry_alloc(ht, path);
        if (hte)
                return hte->user_data;
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
struct filecache_entry *filecache_get(const char *path)
{
        struct hash_table_entry *hte;
        hte = hashtable_entry_get(ht, path);
        if (hte)
                return hte->user_data;
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void filecache_invalidate(const char *path)
{
        hashtable_entry_delete(ht, path);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
struct filecache_entry *filecache_clone(const struct filecache_entry *src)
{
        struct filecache_entry* dest = malloc(sizeof(struct filecache_entry));
        if (dest != NULL) {
                memcpy(dest, src, sizeof(struct filecache_entry));
                errno = 0;
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
void filecache_copy(const struct filecache_entry *src,
                    struct filecache_entry *dest)
{
        if (dest != NULL && src != NULL) {
                free(dest->rar_p);
                if (src->rar_p)
                        dest->rar_p = strdup(src->rar_p);
                else
                        dest->rar_p = NULL;
                free(dest->file_p);
                if (src->file_p)
                        dest->file_p = strdup(src->file_p);
                else
                        dest->file_p = NULL;
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
                CP_ENTRY_F(vsize_real_first);
                CP_ENTRY_F(vsize_real_next);
                CP_ENTRY_F(vsize_next);
                CP_ENTRY_F(vno_base);
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
void filecache_freeclone(struct filecache_entry *dest)
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
        struct hash_table_ops ops = {
                .alloc = __alloc,
                .free = __free,
        };

        ht = hashtable_init(FILECACHE_SZ, &ops);
        pthread_mutex_init(&file_access_mutex, NULL);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void filecache_destroy()
{
        pthread_mutex_destroy(&file_access_mutex);
        hashtable_destroy(ht);
        ht = NULL;
}

