
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
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hashtable.h"
#include "dirlist.h"
#include "dircache.h"
#include "optdb.h"
#include "common.h"

#define DIRCACHE_SZ 1024

/* Hash table handle */
static void *ht = NULL;

pthread_rwlock_t dir_access_mutex;
static struct dircache_cb user_cb;

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *__alloc()
{
        struct dircache_entry *e;
        e = malloc(sizeof(struct dircache_entry));
        if (e)
                dir_list_open(&e->dir_entry_list);
        return e;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __free(void *data)
{
        struct dircache_entry *e = data;
        if (e)
                dir_list_free(&e->dir_entry_list);
        free(e);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void dircache_init(struct dircache_cb *cb)
{
        struct hash_table_ops ops = {
                .alloc = __alloc,
                .free = __free,
        };

        ht = hashtable_init(DIRCACHE_SZ, &ops);
        pthread_rwlock_init(&dir_access_mutex, NULL);
        if (cb)
                user_cb = *cb;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void dircache_destroy()
{
        pthread_rwlock_destroy(&dir_access_mutex);
        hashtable_destroy(ht);
        ht = NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void dircache_invalidate(const char *path)
{
        hashtable_entry_delete(ht, path);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
struct dircache_entry *dircache_alloc(const char *path)
{
        struct hash_table_entry *hte;
        struct dircache_entry *e;
        char *root;
        struct stat st;

        hte = hashtable_entry_alloc(ht, path);
        if (hte) {
                e = hte->user_data;
                ABS_ROOT(root, path);
                if (!stat(root, &st)) {
#ifdef HAVE_STRUCT_STAT_ST_MTIM
                        e->mtim = st.st_mtim;
#else
                        e->mtim.tv_sec = st.st_mtime;
#endif
                        e->ts_valid = 1;
                } else {
                        e->ts_valid = 0;
                }
                return e;
        }
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
struct dircache_entry *dircache_get(const char *path)
{
        struct hash_table_entry *hte;
        struct dircache_entry *e;
        char *root;
        struct stat st;
        int ret;

        hte = hashtable_entry_get(ht, path);
        if (hte) {
                e = hte->user_data;
                if (e->ts_valid) {
                        ABS_ROOT(root, path);
                        ret = stat(root, &st);
#ifdef HAVE_STRUCT_STAT_ST_MTIM
                        if (ret || (st.st_mtim.tv_nsec != e->mtim.tv_nsec) ||
                            (st.st_mtim.tv_sec != e->mtim.tv_sec)) {
#else
                        if (ret || st.st_mtime != e->mtim.tv_sec) {
#endif
                                if (user_cb.stale)
                                        user_cb.stale(path, &e->dir_entry_list);
                                dircache_invalidate(path);
                                return NULL;
                        }
                }
                return e;
        }
        return NULL;
}

