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

#ifndef COMPAT_H
#define COMPAT_H 

#include <stdint.h>   /* C99 uintptr_t */
#include <sys/types.h>

#ifndef STDC_HEADERS
#define STDC_HEADERS 1
#endif
#define HAVE_STDLIB_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_MMAP 1
#define HAVE_SCHED_H 1
#define HAVE_DIRENT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_ALLOCA_H 1
#define HAVE_ALLOC 1
#define HAVE_LOCALE_H 1
#define HAVE_MCSTOMBS 1
#define HAVE_UMASK 1

#define HAVE_STRUCT_STAT_ST_BLKSIZE 1
#define HAVE_STRUCT_STAT_ST_BLOCKS 1
#ifdef __APPLE__
#define HAVE_STRUCT_STAT_ST_GEN 1
#endif

#define RETSIGTYPE void
#include <signal.h>
#ifdef SA_SIGACTION
#define HAVE_STRUCT_SIGACTION_SA_SIGACTION 1
#endif

#ifdef __sun__
#include <arpa/nameser_compat.h>
#define __BYTE_ORDER BYTE_ORDER
#define SCANDIR_ARG3 const struct dirent *
#endif
#ifdef __APPLE__
#include <sys/param.h>
#include <sys/mount.h>
#include <architecture/byte_order.h>
#define SCANDIR_ARG3 struct dirent *
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#ifdef __LITTLE_ENDIAN__
#define __BYTE_ORDER __LITTLE_ENDIAN
#else
#ifdef __BIG_ENDIAN__
#define __BYTE_ORDER __BIG_ENDIAN
#endif
#endif
#endif
#ifdef __FreeBSD__
#if __FreeBSD__ >= 2
#include <osreldate.h>
/* 800501 8.0-STABLE after change of the scandir(3) and alphasort(3)
   prototypes to conform to SUSv4. */
#if __FreeBSD_version >= 800501
#define SCANDIR_ARG3 const struct dirent *
#else
#define SCANDIR_ARG3 struct dirent *
#endif
#else
#define SCANDIR_ARG3 struct dirent *
#endif
#define __BYTE_ORDER _BYTE_ORDER
#define __LITTLE_ENDIAN _LITTLE_ENDIAN
#define __BIG_ENDIAN _BIG_ENDIAN
#endif
#ifdef __linux
#define SCANDIR_ARG3 const struct dirent *
#endif
#ifndef __BYTE_ORDER
#error __BYTE_ORDER not defined
#endif
#if __BYTE_ORDER == __BIG_ENDIAN
#define WORDS_BIGENDIAN
#endif

#ifdef HAS_GLIBC_CUSTOM_STREAMS_
#define HAVE_FMEMOPEN 1
#endif

#if defined ( __UCLIBC__ ) || !defined ( __linux ) || !defined ( __i386 )
#else
#define HAVE_EXECINFO_H 1
#define HAVE_UCONTEXT_H 1
#endif

#if defined ( __linux ) 
#define HAVE_SCHED_SETAFFINITY 1
#endif
#include <sched.h>
#if defined ( __cpu_set_t_defined )
#define HAVE_CPU_SET_T 1
#endif

/* MAC OS X version of gcc does not handle this properly!? */
#if defined ( __GNUC__ ) &&  defined ( __APPLE__ )
#define NO_UNUSED_RESULT
#else
#define NO_UNUSED_RESULT \
        void*ignore_result_ ## __LINE__ ## _;\
        (void)ignore_result_ ## __LINE__ ## _; /* avoid GCC unused-but-set-variable warning */ \
        ignore_result_ ## __LINE__ ## _=(void*)(uintptr_t)
#endif

#ifdef __sun__
#define HAVE_LOCKF 1
#else
#define HAVE_FLOCK 1
#endif

#endif
