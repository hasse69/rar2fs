/*
    Copyright (C) 2009 Hans Beckerus (hans.beckerus#AT#gmail.com)

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

#ifndef COMMON_H_
#define COMMON_H_

#include <platform.h>

#define ABS_ROOT(s, path) \
        do { \
                size_t __root_len = strlen(OPT_STR2(OPT_KEY_SRC,0)); \
                size_t __path_len = strlen(path); \
                size_t __total = __root_len + __path_len + 1; \
                (s) = alloca(__total); \
                snprintf((s), __total, "%s%s", OPT_STR2(OPT_KEY_SRC,0), path); \
        } while (0)

#define ABS_MP_(s, path, file, __alloc) \
        do { \
                size_t __path_len = strlen(path); \
                size_t __file_len = strlen(file); \
                size_t __total = __path_len + __file_len + 2; \
                (s) = __alloc(__total); \
                if (__path_len && path[__path_len - 1] != '/') \
                        snprintf((s), __total, "%s/%s", path, file); \
                else \
                        snprintf((s), __total, "%s%s", path, file); \
        } while(0)

#define ABS_MP(s, path, file) ABS_MP_(s, path, file, alloca)
#define ABS_MP2(s, path, file) ABS_MP_(s, path, file, malloc)

#endif
