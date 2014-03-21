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
#include <memory.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <libgen.h>
#include "debug.h"
#include "optdb.h"

static struct opt_entry opt_entry_[] = {
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 1, 0},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 1},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 1},
        {{NULL,}, 0, 0, 0, 0, 1},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 0},
        {{NULL,}, 0, 0, 0, 0, 0}
};

struct opt_entry *opt_entry_p  = &opt_entry_[0];

#define OPT_(o)                 (opt_entry_p+(o))
#define OPT_STR_                u.v_arr_str,char*
#define OPT_INT_                u.v_arr_int,long
#define ADD_OPT_(o, s1, mt)     ADD_OPT__(o, s1, mt)

#define ADD_OPT__(o, s1, m, t) \
        do { \
                OPT_(o)->is_set = 1; \
                if ((OPT_(o))->n_elem == (OPT_(o))->n_max) { \
                        OPT_(o)->n_max += 16; \
                        OPT_(o)->m = (t*)realloc((t*)OPT_(o)->m, \
                                OPT_(o)->n_max * sizeof(t*)); \
                } \
                if (IS_STR_(o)) \
                        OPT_(o)->m[OPT_(o)->n_elem++] = (t)strdup(s1); \
                else \
                        OPT_(o)->m[OPT_(o)->n_elem++] = (t)strtoul(s1, NULL, 10); \
        } while (0)

#define CLR_OPT_(o) \
        do { \
                OPT_(o)->is_set = 0; \
                if ((OPT_(o))->n_elem && IS_STR_(o)) { \
                        int i = (OPT_(o))->n_elem; \
                        while (i--) { \
                                free((void*)OPT_(o)->u.v_arr_str[i]);\
                        } \
                }\
                OPT_(o)->n_elem = 0; \
        } while (0)

#define IS_INT_(o) (OPT_(o)->type)
#define IS_STR_(o) (!OPT_(o)->type)

/*!
 *****************************************************************************
 *
 ****************************************************************************/
int optdb_save(int opt, const char *s)
{
        char *s1 = NULL;
        char *endptr = NULL;

        if (opt < 0 || opt > OPT_KEY_LAST)
                return 1;

        OPT_(opt)->is_set = 1;

        if (OPT_(opt)->read_from_file && s && *s == '/') {
                FILE *fp = fopen(s, "r");
                if (fp) {
                        struct stat st;
                        (void)fstat(fileno(fp), &st);
                        s1 = malloc(st.st_size * 2);
                        if (s1) {
                                char *s2 = s1;
                                NO_UNUSED_RESULT fread(s1, 1, st.st_size, fp);
                                while(*s1) {
                                        if (*s1=='\n') *s1=';';
                                                s1++;
                                }
                                s1 = s2;
                        }
                        fclose(fp);
                }
        } else {
                if (s)
                        s1 = strdup(s);
        }
        if (!s1)
                return 0;

        switch (opt)
        {
        case OPT_KEY_SEEK_DEPTH:
                break;
        case OPT_KEY_SEEK_LENGTH:
        case OPT_KEY_HIST_SIZE:
        {
                NO_UNUSED_RESULT strtoul(s1, &endptr, 10);
                if (*endptr)
                        return 1;
                CLR_OPT_(opt);
                ADD_OPT_(opt, s1, OPT_INT_);
                break;
        }
        case OPT_KEY_SRC:
        case OPT_KEY_DST:
                CLR_OPT_(opt);
                ADD_OPT_(opt, s1, OPT_STR_);
                break;
        default:
                {
                        /*
                         * One could easily have used strsep() here but I
                         * choose not to:
                         * "This function suffers from the same problems as
                         * strtok(). In particular, it modifies the original
                         * string. Avoid it."
                         */
                        char *s2 = s1;
                        if (strlen(s1)) {
                                while ((s2 = strchr(s2, ';'))) {
                                        *s2++ = 0;
                                        if (strlen(s1) > 1)
                                                ADD_OPT_(opt, s1, OPT_STR_);
                                        s1 = s2;
                                }
                                if (*s1)
                                        ADD_OPT_(opt, s1, OPT_STR_);
                        }
                }
                break;
        }
        if (s1)
                free(s1);

#ifdef DEBUG_
        {
                int i;
                printd(5, "option %d : ", opt);
                for(i = 0; i < OPT_(opt)->n_elem; i++)
                        if (opt != OPT_KEY_SEEK_LENGTH)
                                printd(5, "\"%s\" ", OPT_(opt)->u.v_arr_str[i]);
                        else
                                printd(5, "\"%ld\" ", OPT_(opt)->u.v_arr_int[i]);
                printd(5, "\n");
        }
#endif

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void reset_opt(int opt, int init)
{
        if (opt < 0 || opt > OPT_KEY_LAST)
                return;

        CLR_OPT_(opt);
        if (init) {
                switch (opt) {
                case OPT_KEY_IMG_TYPE:
                        ADD_OPT_(OPT_KEY_IMG_TYPE, ".iso", OPT_STR_);
                        ADD_OPT_(OPT_KEY_IMG_TYPE, ".img", OPT_STR_);
                        ADD_OPT_(OPT_KEY_IMG_TYPE, ".nrg", OPT_STR_);
                        break;
                default:
                        break;
                }
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void optdb_init()
{
        int i = OPT_KEY_END;
        while (i--)
                reset_opt(i, 1);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void optdb_destroy()
{
        int i = OPT_KEY_END;
        while (i--)
                reset_opt(i, 0);
}

#undef ADD_OPT_
#undef OPT_
#undef OPT_INT_
#undef OPT_STR_

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline int get_ext_len(char *s)
{
        char *s1 = s + strlen(s);
        while (s1 != s && *s1 != '.')
                --s1;
        return s1 == s ? 0 : strlen(s1);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
int optdb_find(int opt, char *path)
{
        int i = 0;
        if (opt == OPT_KEY_EXCLUDE) {
                while (i != OPT_CNT(opt)) {
                        char *tmp = OPT_STR(OPT_KEY_EXCLUDE, i);
                        char *safe_path = strdup(path);
                        if (!strcmp(basename(safe_path), tmp ? tmp : "")) {
                                free(safe_path);
                                return 1;
                        }
                        free(safe_path);
                        ++i;
                }
        } else if (opt == OPT_KEY_FAKE_ISO || opt == OPT_KEY_IMG_TYPE) {
                int l = get_ext_len(path);
                if (l <= 1)
                        return 0;
                while (i != OPT_CNT(opt)) {
                        char *tmp =  OPT_STR(opt, i);
                        if (!strcmp((path) + (strlen(path) - l), tmp ? tmp : ""))
                                return l - 1;
                        ++i;
                }
        }
        return 0;
}

