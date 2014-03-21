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

#ifndef PLATFORM_H_
#define PLATFORM_H_

#ifdef HAVE_CONFIG_H
# include <config.h>
#else
# include <compat.h>
#endif

#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif
#ifdef HAVE_UNISTD_H
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef HAVE_INTTYPES_H
#define __STDC_FORMAT_MACROS /* should only be needed for C++ */
# include <inttypes.h>
#endif
#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif

#ifdef HAVE_ALLOCA_H
# include <alloca.h>
#elif defined __GNUC__
/* Some systems, eg. FreeBSD, define this already in stdlib.h */
# ifndef alloca
#  define alloca __builtin_alloca
# endif
#elif defined _AIX
# define alloca __alloca
#elif defined _MSC_VER
# include <malloc.h>
# define alloca _alloca
#else
# ifndef HAVE_ALLOCA
#  ifdef  __cplusplus
    extern "C"
#  endif
    void *alloca (size_t);
# endif
#endif

#ifdef HAVE_DIRENT_H
# include <dirent.h>
# define NAMLEN(dirent) strlen ((dirent)->d_name)
#else
# define dirent direct
# define NAMLEN(dirent) ((dirent)->d_namlen)
# ifdef HAVE_SYS_NDIR_H
#  include <sys/ndir.h>
# endif
# ifdef HAVE_SYS_DIR_H
#  include <sys/dir.h>
# endif
# ifdef HAVE_NDIR_H
#  include <ndir.h>
# endif
#endif

#ifdef __GNUC__
#ifdef HAVE_SYNC_SYNCHRONIZE
#define MB() __sync_synchronize()
#else 
#define MB() do{ __asm__ __volatile__ ("" ::: "memory"); } while(0)
#endif
#else
#warning Check code for MB() on current platform
#define MB()
#endif

extern long page_size_;
#define P_ALIGN_(a) (((a)+page_size_)&~(page_size_-1))

#ifdef HAVE_STDBOOL_H
# include <stdbool.h>
#else
# ifndef HAVE__BOOL
#  ifdef __cplusplus
typedef bool _Bool;
#  else
#   define _Bool signed char
#  endif
# endif
# define bool _Bool
# define false 0
# define true 1
# define __bool_true_false_are_defined 1
#endif

#ifdef HAVE_FDATASYNC
#ifdef __APPLE__
# include <sys/syscall.h>
# define fdatasync(fd) syscall(SYS_fdatasync, (fd))
#endif
#else
# define fdatasync(fd) fsync(fd)
#endif

/* Not very likely, but just to be safe... */
#ifndef HAVE_MMAP
# undef MAP_FAILED
# define MAP_FAILED      ((void *) -1)
#endif

#endif


