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

#ifndef OPTDB_H_
#define OPTDB_H_

#define IS_IMG(s) \
        ((OPT_CNT(OPT_KEY_IMG_TYPE) && optdb_find(OPT_KEY_IMG_TYPE, s)) || \
        (OPT_CNT(OPT_KEY_FAKE_ISO) && optdb_find(OPT_KEY_FAKE_ISO, s)))

#define OPT_FILTER(path) \
        (OPT_CNT(OPT_KEY_EXCLUDE) && optdb_find(OPT_KEY_EXCLUDE, (char*)(path)))

#define OPT_BASE    (1000)
#define OPT_ADDR(o) ((o) + OPT_BASE)
#define OPT_ID(a)   ((a) - OPT_BASE)

enum {
        OPT_KEY_SRC = 0,
        OPT_KEY_DST,
        OPT_KEY_EXCLUDE,
        OPT_KEY_FAKE_ISO,
        OPT_KEY_IMG_TYPE,
        OPT_KEY_PREOPEN_IMG,
        OPT_KEY_SHOW_COMP_IMG,
        OPT_KEY_SEEK_LENGTH,
        OPT_KEY_SEEK_DEPTH,
        OPT_KEY_NO_PASSWD, /* Obsolete */
        OPT_KEY_NO_SMP,
        OPT_KEY_UNRAR_PATH, /* Obsolete */
        OPT_KEY_NO_LIB_CHECK,
        OPT_KEY_HIST_SIZE,
        OPT_KEY_BUF_SIZE,
        OPT_KEY_SAVE_EOF,
        OPT_KEY_NO_EXPAND_CBR,
        OPT_KEY_FLAT_ONLY,
        OPT_KEY_END, /* Must *always* be last key */
        OPT_KEY_LAST = (OPT_KEY_END - 1) 
};

struct opt_entry {
        union
        {
                long *v_arr_int;
                char **v_arr_str;
                void *p;
        } u;
        int is_set;
        int n_elem;
        int n_max;
        int read_from_file;
        int type;
};

#define OPT_CNT(o)     (opt_entry_p[(o)].n_elem)
#define OPT_STR(o, n)  (OPT_SET(o)?opt_entry_p[(o)].u.v_arr_str[(n)]:NULL)
#define OPT_STR2(o, n) (OPT_SET(o)?opt_entry_p[(o)].u.v_arr_str[(n)]:"")
#define OPT_INT(o, n)  (OPT_SET(o)?opt_entry_p[(o)].u.v_arr_int[(n)]:0)
#define OPT_SET(o)     (opt_entry_p[(o)].is_set)

extern struct opt_entry *opt_entry_p;

int optdb_save(int opt, const char*);
int optdb_find(int opt, char*);
void optdb_init();
void optdb_destroy();

#endif
