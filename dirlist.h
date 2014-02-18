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

#ifndef DIRLIST_H_
#define DIRLIST_H_

#include <platform.h>

/* Directory entry types */
#define DIR_E_NRM 0
#define DIR_E_RAR 1

typedef struct dir_entry_list dir_entry_list;

__extension__
struct dir_entry_list {
        struct dir_entry {
                char *name;
                uint32_t hash;
                union {
                        struct stat *st;
                        void *head_flag;
                };
                int type;
                int valid;
        } entry;
        struct dir_entry_list *next;
};

void dir_list_open(struct dir_entry_list *root);

void dir_list_close(struct dir_entry_list *root);

void dir_list_free(struct dir_entry_list *root);

struct dir_entry_list *dir_entry_add_hash(struct dir_entry_list *l,
        const char *key, struct stat *st, uint32_t hash, int type);

struct dir_entry_list *dir_entry_add(struct dir_entry_list *l,
        const char *key, struct stat *st, int type);

#endif
