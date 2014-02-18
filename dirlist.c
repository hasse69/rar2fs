/*
    Copyright (C) 2009-2014 Hans Beckerus (hans.beckerus#AT#gmail.com)

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
#include <string.h>
#include "dirlist.h"

#define DIR_LIST_HEAD_ ((void*)-1)

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline int swap(struct dir_entry_list *A, struct dir_entry_list *B)
{
        int swap = strcmp(A->entry.name, B->entry.name);
        swap = !swap ? A->entry.type > B->entry.type : swap;
        if (swap > 0) {
                const struct dir_entry TMP = B->entry;
                B->entry = A->entry;
                A->entry = TMP;
                return 1;
        }
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void dir_list_open(struct dir_entry_list *root)
{
        root->next = NULL;
        root->entry.name = NULL;
        root->entry.head_flag = DIR_LIST_HEAD_;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void dir_list_close(struct dir_entry_list *root)
{
        /* Simple bubble sort of directory entries in alphabetical order */
        if (root && root->next) {
                int n;
                struct dir_entry_list *next;
                do {
                        n = 0;
                        next = root->next;
                        while (next->next) {
                                n += swap(next, next->next);
                                next = next->next;
                        }
                } while (n != 0);       /* while swaps performed */

                /* Make sure entries are unique. Duplicates will be removed. */
                next = root->next;
                while (next->next) {
                        if ((next->entry.type == DIR_E_NRM || /* no hash */
                                    next->entry.hash == next->next->entry.hash) &&
                                    !strcmp(next->entry.name, next->next->entry.name)) {
                                /* 
                                 * A duplicate. Rare but possible.
                                 * Make sure the current entry is kept marked
                                 * as valid since regular fs entries should
                                 * always have priority.
                                 */
                                next->next->entry.valid = 0;
                        } 
                        next = next->next;
                }
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void dir_list_free(struct dir_entry_list *root)
{
        struct dir_entry_list *next = root->next;
        while (next) {
                struct dir_entry_list *tmp = next;
                next = next->next;
                free(tmp->entry.name);
                free(tmp);
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
struct dir_entry_list *dir_entry_add_hash(struct dir_entry_list *l,
                const char *key, struct stat *st, uint32_t hash, int type)
{
        int dir_entry_add_skip_ = 0;

        if (l->entry.head_flag != DIR_LIST_HEAD_) {
                if (hash == l->entry.hash)
                        if (!strcmp(key, l->entry.name))
                                dir_entry_add_skip_ = 1;
        };
        l->next = !dir_entry_add_skip_
                ? malloc(sizeof(struct dir_entry_list))
                : NULL;
        if (l->next) {
                l=l->next;
                l->entry.head_flag = 0;
                l->entry.name = strdup(key);
                l->entry.hash = hash;
                l->entry.st = st;
                l->entry.type = type;
                l->entry.valid = 1; /* assume entry is valid */
                l->next = NULL;
        }
        return l;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
struct dir_entry_list *dir_entry_add(struct dir_entry_list *l, const char *key,
                struct stat *st, int type)
{
        l->next = malloc(sizeof(struct dir_entry_list));
        if (l->next) {
                l=l->next;
                l->entry.name = strdup(key);
                l->entry.hash = 0;
                l->entry.st = st;
                l->entry.type = type;
                l->entry.valid = 1; /* assume entry is valid */
                l->next = NULL;
        }
        return l;
}

