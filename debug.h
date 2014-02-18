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

#ifndef DEBUG_H_
#define DEBUG_H_

#include <stdio.h>

#if defined ( DEBUG_ ) && DEBUG_ > 5
#undef DEBUG_
#define DEBUG_ 5
#endif

#if defined ( DEBUG_ )
#if DEBUG_ > 2
#define ELVL_ 2
#else
#define ELVL_ DEBUG_
#endif
#define ENTER_(...)         ENTER__(ELVL_, __VA_ARGS__)
#define ENTER__(l, ...)     ENTERx_(l, __VA_ARGS__)
#define ENTERx_(l, ...)     ENTER##l##_(__VA_ARGS__)
#define ENTER1_(fmt, ...)   printd(1, "%s()\n", __func__)
#define ENTER2_(fmt, ...)   printd(2, "%s()   " fmt "\n", __func__, ##__VA_ARGS__)
#else
#define ENTER_(...)
#endif

#ifdef DEBUG_
#define printd(l, fmt, ...) \
        do{ \
                if (l <= DEBUG_) \
                        fprintf(stderr, fmt, ##__VA_ARGS__); \
        }while(0)
#else
#define printd(...)
#endif

#endif
