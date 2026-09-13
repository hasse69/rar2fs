#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.
AC_INIT([rar2fs],
        [m4_esyscmd(./get-version.sh src/version.h)],
        [https://hasse69.github.io/rar2fs])
AC_PREREQ([2.61])
AC_CONFIG_AUX_DIR(config)
AC_CANONICAL_TARGET
AM_INIT_AUTOMAKE
AC_CONFIG_MACRO_DIR(m4)
AC_CONFIG_HEADERS([src/config.h])

AM_CONDITIONAL([LINUX],
               [case $host_os in *linux*) true;; *) false;; esac])

# Save CFLAGS (if defined by user; othwerwise NONE)
USER_CFLAGS=${CFLAGS-NONE} 

BUILDDIR=`cd $srcdir;pwd`

# Checks for programs.
AC_PROG_CC_STDC
if test x"$GCC" != x"yes"; then
    AC_MSG_ERROR([
        Sorry, but $PACKAGE currently supports only \
the GNU C/C++ compiler!]);
fi

AM_PROG_CC_C_O
AC_PROG_CXX

AC_USE_SYSTEM_EXTENSIONS
AX_C_BIGENDIAN_CROSS

# Checks for libraries.

# Checks for header files.
AC_FUNC_ALLOCA
AC_CHECK_HEADERS([errno.h arpa/inet.h fcntl.h limits.h locale.h malloc.h stddef.h sys/file.h sys/mount.h sys/param.h sys/statvfs.h syslog.h wchar.h])
AC_CHECK_HEADERS([execinfo.h ucontext.h sched.h])
AC_CHECK_HEADERS([sys/sysmacros.h])
AC_CHECK_HEADERS([sys/xattr.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_DIRENT
# Use C++ context when checking for <stdbool.h>.
# Some platforms seems to fail in detecting stdbool.h properly
# due to the IBM "xlc bug" when gcc is used!?
AC_LANG_PUSH([C++])
AC_HEADER_STDBOOL
AC_LANG_POP
AC_HEADER_ASSERT
AC_TYPE_UID_T
AC_C_INLINE
AC_TYPE_MODE_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_CHECK_MEMBERS([struct stat.st_blksize])
AC_CHECK_MEMBERS([struct stat.st_gen])
AC_CHECK_MEMBERS([struct stat.st_atim])
AC_CHECK_MEMBERS([struct stat.st_ctim])
AC_CHECK_MEMBERS([struct stat.st_mtim])
AC_STRUCT_ST_BLOCKS
AC_CHECK_MEMBERS([struct stat.st_rdev])
AC_TYPE_UINT64_T

need_priu64=no
AC_MSG_CHECKING([for a working PRIu64])
AC_LANG_PUSH([C])
AC_RUN_IFELSE(
	[AC_LANG_PROGRAM([[
		#include <stdio.h>
		#ifdef HAVE_INTTYPES_H
                #define __STDC_FORMAT_MACROS
		#include <inttypes.h>
		#endif
		]],
		[[
		char *x;
		#if !defined(PRIu64)
			return -1;
		#else
		x = PRIu64;
		if( x[0] == '%' ) {
			return -1;
		} else {
			return 0;
		}
		#endif
		]])],
	[
		# program worked correctly
		AC_MSG_RESULT([yes])
	],
	[
		# program failed
		AC_MSG_RESULT([no])
		AC_DEFINE([NEED_PRI_MACRO], [1], [Defined when PRIu64 is missing or broken])
		need_priu64=yes
	],
	[
		# we are cross compiling
		AC_MSG_RESULT([skipped])
		AC_DEFINE([NEED_PRI_MACRO], [1], [Defined when PRIu64 is missing or broken])
		need_priu64=yes
	])
AC_LANG_POP([C])
AC_TYPE_SIGNAL
AC_CHECK_MEMBERS([struct sigaction.sa_sigaction],,,[[#include <signal.h>]])

# Checks for library functions.
AC_FUNC_CHOWN
AC_FUNC_FORK
AC_SYS_LARGEFILE
AC_FUNC_FSEEKO
AC_FUNC_LSTAT_FOLLOWS_SLASHED_SYMLINK
AC_CHECK_FUNCS([mktime atexit dup3 fs_stat_dev ftruncate getcwd getpass lchown memchr memmove memset mkdir realpath rmdir select setlocale strchr strdup strerror strpbrk strrchr strstr strtol strtoul utimensat fdatasync wcstombs umask memrchr])

########################################################
# Check for extended attribute support
AC_SEARCH_LIBS(getxattr, [attr])
AC_CHECK_FUNCS(getxattr,[
        AC_CHECK_FUNCS(lgetxattr fgetxattr listxattr llistxattr flistxattr removexattr lremovexattr fremovexattr setxattr lsetxattr fsetxattr)
        ])
########################################################
# Do xattr functions take additional options like on Darwin?
if test x"$ac_cv_func_getxattr" = x"yes" ; then
        AC_CACHE_CHECK([whether xattr interface takes additional options], attr_cv_xattr_add_opt, [
                old_LIBS=$LIBS
                LIBS="$LIBS $ACL_LIBS"
                AC_TRY_COMPILE([
                        #include <sys/types.h>
                        #if HAVE_ATTR_XATTR_H
                        #include <attr/xattr.h>
                        #elif HAVE_SYS_XATTR_H
                        #include <sys/xattr.h>
                        #endif
                ],[
                        getxattr(0, 0, 0, 0, 0, 0);
                ],
                [attr_cv_xattr_add_opt=yes],
                [attr_cv_xattr_add_opt=no;LIBS=$old_LIBS])
        ])
        if test x"$attr_cv_xattr_add_opt" = x"yes"; then
                AC_DEFINE(XATTR_ADD_OPT, [1], [Define to 1 if xattr functions takes additional options])
        fi
fi

# Check for GNU library extension fmemopen()
#
AC_CHECK_FUNCS(fmemopen)

# Check for lockf() and/or flock()
# On eg. OpenSolaris there is only a POSIX lockf() function.
# 
AC_CHECK_FUNCS(lockf flock)

# Check for mmap() system call
#
if test x"$cross_compiling" = x"yes"; then
   AC_CHECK_FUNCS([mmap munmap])
   if test x"$ac_cv_func_mmap" = x"yes"; then
       ac_cv_func_mmap_fixed_mapped=yes
       AC_DEFINE([HAVE_MMAP], [1], [Define to 1 if you have a working `mmap' system call.])
   fi
else
   AC_FUNC_MMAP
   AC_CHECK_FUNCS(munmap)
fi

########################################################
# Check if backtrace functions needs a lib
AC_SEARCH_LIBS([backtrace], [execinfo])

withval=
AC_ARG_WITH([fuse],
   [AS_HELP_STRING([--with-fuse=PATH],
               [search PATH to fuse header files]
   )],
   [], [])
if test x"$withval" != x; then
    if test x"$withval" != x"no"; then
       if test x"$withval" != x"yes"; then
           FUSE_CPPFLAGS="-I"$withval
       fi
    fi
fi

withval=
AC_ARG_WITH([fuse-lib],
   [AS_HELP_STRING([--with-fuse-lib=PATH],
               [search PATH to fuse library]
   )],
   [], [])
if test x"$withval" != x; then
    if test x"$withval" != x"no"; then
       if test x"$withval" != x"yes"; then
           FUSE_LDFLAGS="-L"$withval
       fi
    fi
fi

static_unrar=fallback
enableval=
AC_ARG_ENABLE([static-unrar],
   [AS_HELP_STRING([--enable-static-unrar],
               [enable static linking of libunrar [default]])],
   [], [])
if test x"$enableval" != x"yes"; then
    if test x"$enableval" = x"no"; then
        static_unrar=no
    fi;
else
    static_unrar=yes
fi

withval=
AC_ARG_WITH([unrar],
   [AS_HELP_STRING([--with-unrar=PATH],
               [search PATH to unrar source [PATH=./unrar]]
   )],
   [], [])
if test x"$withval" != x; then
    if test x"$withval" != x"no"; then
        if test x"$withval" != x"yes"; then
           with_unrar=`cd $with_unrar;pwd`
           UNRAR_CPPFLAGS="-I"`cd $with_unrar;pwd`
        fi
    fi
else
    with_unrar="$BUILDDIR/unrar"
    UNRAR_CPPFLAGS="-I$BUILDDIR/unrar"
fi

withval=
AC_ARG_WITH([unrar-lib],
   [AS_HELP_STRING([--with-unrar-lib=PATH],
               [search PATH to unrar library]
   )],
   [], [])
if test x"$withval" != x; then
    if test x"$withval" != x"no"; then
       if test x"$withval" != x"yes"; then
           if test x"$static_unrar" != x"yes"; then
               UNRAR_LDFLAGS="-L"`cd $withval;pwd`
               with_unrar_lib=$withval
               export LD_LIBRARY_PATH="$LD_LIBRARY_PATH\:/"`cd $withval;pwd`
               export DYLD_LIBRARY_PATH="$LD_LIBRARY_PATH\:/"`cd $withval;pwd`
           fi
       fi
    fi
fi

FUSE_CPPFLAGS="$FUSE_CPPFLAGS -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26"
UNRAR_CXXFLAGS="-DRARDLL"
AC_SUBST(FUSE_CPPFLAGS)
AC_SUBST(UNRAR_CPPFLAGS)
AC_SUBST(UNRAR_CXXFLAGS)
AC_SUBST(FUSE_LDFLAGS)
AC_SUBST(UNRAR_LDFLAGS)

enableval=
AC_ARG_ENABLE([debug],
   [AS_HELP_STRING([--enable-debug[[=LEVEL]]],
               [enable debug mode [LEVEL=4]])],
   [], [])
if test x"$enableval" != x"no"; then
   if test x"$enableval" = x"yes"; then
       debug_level=4
   else
       debug_level=$enableval
   fi
fi
if test "$debug_level" != ""; then
    AC_DEFINE_UNQUOTED(DEBUG_, $debug_level, Define run-time debug level)
    if test "$USER_CFLAGS" = "NONE"; then
        CXXFLAGS="-g -O0"
        CFLAGS="-g -O0"
    fi
else
    CPPFLAGS="$CPPFLAGS -DNDEBUG" 
fi

AX_PTHREAD
AC_SUBST(PTHREAD_LIBS)
AC_SUBST(PTHREAD_CFLAGS)

# FUSE might need these later if eg. -static is set in LDFLAGS. Use a generic 
# function like main() here since all we need is to verify if the library
# exists or not and then append it to LIBS.
AC_CHECK_LIB([rt], [main])
AC_CHECK_LIB([dl], [main])

save_CPPFLAGS="$CPPFLAGS"
CPPFLAGS="$CPPFLAGS $FUSE_CPPFLAGS"
AC_CHECK_HEADER(fuse.h,,
    [AC_MSG_ERROR([
        Cannot find fuse.h - add alternative include search path using
        --with-fuse and rerun configure.])])

save_LDFLAGS="$LDFLAGS"
save_LIBS="$LIBS"
LDFLAGS="$LDFLAGS $FUSE_LDFLAGS $PTHREAD_CFLAGS"
LIBS="$PTHREAD_LIBS $LIBS"
AC_CHECK_LIB(osxfuse_i64,fuse_new, [FUSE_LIBS="-losxfuse_i64"],
    [ AC_CHECK_LIB(osxfuse,fuse_new, [FUSE_LIBS="-losxfuse"],
        [ AC_CHECK_LIB(fuse_ino64,fuse_new, [FUSE_LIBS="-lfuse_ino64"],
            [ AC_CHECK_LIB(fuse4x,fuse_new, [FUSE_LIBS="-lfuse4x"],
                [ AC_CHECK_LIB(fuse,fuse_new, [FUSE_LIBS="-lfuse"],
                    [ AC_CHECK_LIB(dokanfuse2.dll,fuse_new, [FUSE_LIBS="-ldokanfuse2.dll"],
			   [ AC_CHECK_LIB(dokanfuse1.dll,fuse_new, [FUSE_LIBS="-ldokanfuse1.dll"],
				  [AC_MSG_ERROR([
				           Cannot find fuse library - add alternative library search path using
					   --with-fuse-lib and rerun configure. ])
			   ],)
                    ],)
                ],)
            ],)
        ],)
    ],)
],)
LIBS="$FUSE_LIBS $LIBS"
# fuse_version() became available for the first time in fuse 2.7
AC_CHECK_FUNCS(fuse_version)
LIBS="$save_LIBS"
LDFLAGS="$save_LDFLAGS"
AC_SUBST(FUSE_LIBS)

########################################################
# Check for a supported FUSE_MAJOR_VERSION.
AC_MSG_CHECKING([for supported FUSE API version])
if test x"$cross_compiling" = x"no"; then
    AC_RUN_IFELSE([
        AC_LANG_PROGRAM([[
             #include <fuse.h>]],
    [[
        if(FUSE_MAJOR_VERSION == 2 && FUSE_MINOR_VERSION >= 6)
            return 0;
        return -1;
    ]])],
        [AC_MSG_RESULT([yes])],
        [AC_MSG_RESULT([no])
        AC_MSG_FAILURE([
    $PACKAGE requires FUSE 2.6 or newer. Please check config.log for errors.
    If you cannot determine the problem, mail the author of the package
    and include the config.log file])
        ]
    )
else
    AC_MSG_RESULT([skipped])
fi
########################################################

########################################################
# Check for cygfuse
AC_MSG_CHECKING([if we need to set -DCYGFUSE])
AC_TRY_COMPILE(
    [#include <fuse.h>
    ],
    [#ifndef FSP_FUSE_API
         #error need -DCYGFUSE
     #endif
    ],
    [AC_MSG_RESULT([yes])
        FUSE_CPPFLAGS="$FUSE_CPPFLAGS -DCYGFUSE"
    ],
    [AC_MSG_RESULT([no])
    ]
)
########################################################
CPPFLAGS="$save_CPPFLAGS"

save_CPPFLAGS="$CPPFLAGS"
CPPFLAGS="$CPPFLAGS $UNRAR_CPPFLAGS"
AC_LANG_PUSH([C++])
AC_CHECK_HEADER([rar.hpp],,
     AC_MSG_ERROR([
            Cannot find unrar source - add alternative source search path using
            --with-unrar and rerun configure.])
)
AC_LANG_POP
CPPFLAGS="$save_CPPFLAGS"

AC_MSG_CHECKING([for static linking of UnRAR library])
AC_MSG_RESULT([$static_unrar])
if test x"$static_unrar" != x"no"; then
    test -f $with_unrar/libunrar.a && link_fallback=no || link_fallback=test
    if test x"$link_fallback" = x"test"; then
        if test x"$static_unrar" = x"fallback"; then
            static_unrar=no
            AC_MSG_WARN([Failed to locate static UnRAR library; falling back to dynamic linking])
        else
            AC_MSG_ERROR([]
                   [Cannot find static unrar library - add alternative source search path using]
                   [--with-unrar and/or make sure a library has been built (only supported from]
                   [version 5.4.6 and later) in the source tree pointed to as $with_unrar and]
                   [rerun configure.])
        fi
    fi
fi

if test x"$static_unrar" = x"no"; then
    save_LDFLAGS="$LDFLAGS"
    save_LIBS="$LIBS"
    save_CXXFLAGS="$CXXFLAGS"
    LDFLAGS="$LDFLAGS $UNRAR_LDFLAGS"
    LIBS="$PTHREAD_LIBS $LIBS"
    CXXFLAGS="$CXXFLAGS $PTHREAD_CFLAGS"
    AC_LANG_PUSH([C++])
    AC_CHECK_LIB(unrar, RARGetDllVersion, [UNRAR_LIBS="-lunrar"],
         AC_MSG_ERROR([]
                [Cannot find unrar library - add alternative library search path using]
                [--with-unrar-lib and rerun configure. If the intention was to build]
                [towards a static version of the library please make sure such has been]
                [built (only supported from v5.4.6 and later) in the source tree pointed]
                [to as $with_unrar.])
    )
    AC_LANG_POP
    LDFLAGS="$save_LDFLAGS"
    LIBS="$save_LIBS"
    CXXFLAGS="$save_CXXFLAGS"

    ########################################################
    # Check for matching unrar library- vs. source version
    save_CPPFLAGS="$CPPFLAGS"
    save_LDFLAGS="$LDFLAGS"
    save_LIBS="$LIBS"
    save_CXXFLAGS="$CXXFLAGS"
    LDFLAGS="$LDFLAGS $UNRAR_LDFLAGS"
    CPPFLAGS="$CPPFLAGS $UNRAR_CPPFLAGS"
    LIBS="$UNRAR_LIBS $PTHREAD_LIBS $LIBS"
    CXXFLAGS="$CXXFLAGS $PTHREAD_CFLAGS"
    AC_LANG_PUSH([C++])
    AC_MSG_CHECKING([for matching UnRAR library and source version])
    if test x"$cross_compiling" = x"no"; then
        AC_RUN_IFELSE([
            AC_LANG_PROGRAM([[
                 #define _UNIX
                 #include <dll.hpp>]],
        [[
            if (RARGetDllVersion() != RAR_DLL_VERSION)
                return -1;
        ]])],
            [AC_MSG_RESULT([yes])],
            [AC_MSG_RESULT([no])
            AC_MSG_FAILURE([
        The version of UnRAR source pointed to by --with-unrar=$with_unrar
        and the current shared library picked up at run time are incompatible.
        Make sure the shared library is properly installed on the system or add 
        an alternative library search path using --with-unrar-lib and/or add
        an alternative source search path using --with-unrar and rerun configure.])
            ]
        )
    else
        AC_MSG_RESULT([skipped])
    fi
    AC_LANG_POP
    CPPFLAGS="$save_CPPFLAGS"
    LDFLAGS="$save_LDFLAGS"
    LIBS="$save_LIBS"
    CXXFLAGS="$save_CXXFLAGS"
    ########################################################
else
    UNRAR_LIBS="$with_unrar/libunrar.a"
    AC_DEFINE(HAVE_STATIC_UNRAR, [1], [Define to 1 if UnRAR library is linked statically])
fi

AC_SUBST(UNRAR_LIBS)

########################################################
# Check if we need to set -DRAR_SMP
if test x"$UNRAR_LIBS" != x; then
    save_CPPFLAGS="$CPPFLAGS"
    CPPFLAGS="$CPPFLAGS $UNRAR_CPPFLAGS"
    save_LDFLAGS=$LDFLAGS
    LDFLAGS="$LDFLAGS $UNRAR_LDFLAGS"
    save_LIBS=$LIBS
    LIBS="$UNRAR_LIBS $LIBS"
    save_CXXFLAGS="$CXXFLAGS"
    CXXFLAGS="$CXXFLAGS $PTHREAD_CFLAGS"
    AC_LANG_PUSH([C++])
    AC_MSG_CHECKING([if unrar library needs -DRAR_SMP])
    AC_LINK_IFELSE(
        [AC_LANG_PROGRAM(
            [
                #define RAR_SMP
                #include "rar.hpp"
            ],
            [
                GetNumberOfThreads();
            ])],
        [AC_MSG_RESULT([yes])
            UNRAR_CXXFLAGS="$UNRAR_CXXFLAGS -DRAR_SMP"
        ],
        [AC_MSG_RESULT([no])
        ]
    )
    AC_LANG_POP
    CPPFLAGS="$save_CPPFLAGS"
    LDFLAGS="$save_LDFLAGS"
    LIBS="$save_LIBS"
    CXXFLAGS="$save_CXXFLAGS"
fi
########################################################

AM_ICONV
AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION([0.14.4])

AC_MSG_CHECKING(for commit hash in git repo)
GITHEAD=$BUILDDIR/.git/$(cat $BUILDDIR/.git/HEAD 2>/dev/null | cut -d" " -f2)
GITREV=$(cut -c1-7 $GITHEAD 2>/dev/null)
if test x"$GITREV" != x; then
    AC_SUBST([CONFIG_STATUS_DEPENDENCIES], [$GITHEAD])
    CFLAGS="$CFLAGS -DGITREV_=0x$GITREV"
    echo $GITREV
else
    echo no
fi

# Check for the Linux functions for controlling processor affinity.
# LINUX: sched_setaffinity
AC_CHECK_FUNCS(sched_setaffinity)
if test "$ac_cv_func_sched_setaffinity" = "yes" ; then
    # Test for the cpu process set type
    AC_CACHE_CHECK([whether cpu_set_t available],pac_cv_have_cpu_set_t,[
    AC_TRY_COMPILE( [
#include <sched.h>],[ cpu_set_t t; ],pac_cv_have_cpu_set_t=yes,pac_cv_have_cpu_set_t=no)])
    if test "$pac_cv_have_cpu_set_t" = yes ; then
        AC_DEFINE(HAVE_CPU_SET_T,1,[Define to 1 if cpu_set_t is defined in sched.h])

        AC_CACHE_CHECK([whether the CPU_SET and CPU_ZERO macros are defined],
        pac_cv_cpu_set_defined,[
        AC_TRY_LINK( [
#include <sched.h>],[ cpu_set_t t; CPU_ZERO(&t); CPU_SET(1,&t); ],
        pac_cv_cpu_set_defined=yes,pac_cv_cpu_set_defined=no)])
        if test "$pac_cv_cpu_set_defined" = "yes" ; then
            AC_DEFINE(HAVE_CPU_SET_MACROS,1,[Define to 1 if CPU_SET and CPU_ZERO macros are defined])
        fi
        # FIXME: Some versions of sched_setaffinity return ENOSYS (!),
        # so we should test for the unfriendly and useless behavior
    fi
fi

AH_TEMPLATE([SCANDIR_ARG3], [Define the type of arg#3 to scandir()])
AX_PROTOTYPE_SCANDIR

# Check how linker handles -fno-omit-frame-pointer
# Note that AC_LANG_WERROR is not used here since it is more or less useless
# until its effect can be undone. Instead $ac_c_werror_flag is manipulated
# directly. Dirty but effective!
#
AC_LANG_PUSH(C)
ac_c_werror_flag=yes
save_CFLAGS=$CFLAGS
CFLAGS="$CFLAGS -fno-omit-frame-pointer"
AC_MSG_CHECKING([if compiler supports -fno-omit-frame-pointer])
AC_TRY_COMPILE([],[],
    [AC_MSG_RESULT([yes])
    ],
    [AC_MSG_RESULT([no])
        CFLAGS="$save_CFLAGS"
    ]
)
ac_c_werror_flag=
AC_LANG_POP

# Check how compiler handles -Wextra
# Note that AC_LANG_WERROR is not used here since it is more or less useless
# until its effect can be undone. Instead $ac_c_werror_flag is manipulated
# directly. Dirty but effective!
AC_LANG_PUSH(C)
ac_c_werror_flag=yes
save_CFLAGS=$CFLAGS
CFLAGS="$CFLAGS -Wextra"
AC_MSG_CHECKING([if compiler supports -Wextra])
AC_TRY_COMPILE([#include<stdio.h>],[
    ],
    [AC_MSG_RESULT([yes])
    ],
    [AC_MSG_RESULT([no])
        CFLAGS="$save_CFLAGS -W"
    ]
)
ac_c_werror_flag=
AC_LANG_POP

# Check how compiler handles -Wno-unused-result
# If HAVE_NO_UNUSED_RESULT is defined to 1 there is no need for
# a run-time workaround.
# The functions declared with attribute warn_unused_result can 
# then be called like this:
#    NO_UNUSED_RESULT fscanf(...)

# Note that AC_LANG_WERROR is not used here since it is more or less useless
# until its effect can be undone. Instead $ac_c_werror_flag is manipulated
# directly. Dirty but effective!
# Also, we check using -Wunused-result since GCC has a special treatment of
# the '-Wno-' form. If there is a -Wunused-result it is here assumed that
# there is also a -Wno-unused-result and vice versa.
#
AH_TEMPLATE([HAVE_NO_UNUSED_RESULT],
            [Define to 1 if compiler can disable warning for functions declared
             with attribute warn_unused_result])
AH_VERBATIM([HAVE_NO_UNUSED_RESULT_],[
/* Set return type for functions declared with attribute warn_unused_result
   to avoid warning if it can not be switch off (bug!?). */
#ifndef HAVE_NO_UNUSED_RESULT
/* MAC OS X version of gcc does not handle this properly!? */
#if defined ( __GNUC__ ) && defined ( __APPLE__ )
#define NO_UNUSED_RESULT
#else
#define NO_UNUSED_RESULT \
         void*ignore_result_ ## __LINE__ ## _;\
         (void)ignore_result_ ## __LINE__ ## _; /* avoid GCC unused-but-set-variable warning */ \
         ignore_result_ ## __LINE__ ## _=(void*)(uintptr_t)
#endif
#else
#define NO_UNUSED_RESULT
#endif
])
AC_LANG_PUSH(C)
ac_c_werror_flag=yes
save_CFLAGS=$CFLAGS
CFLAGS="$CFLAGS -Wunused-result"
AC_MSG_CHECKING([if compiler supports -Wno-unused-result])
AC_TRY_COMPILE([#include<stdio.h>],[
    ],
    [AC_MSG_RESULT([yes])
        AC_DEFINE([HAVE_NO_UNUSED_RESULT],[1])
        CFLAGS="$save_CFLAGS -Wno-unused-result"
    ],
    [AC_MSG_RESULT([no])
        CFLAGS=$save_CFLAGS
    ]
)
ac_c_werror_flag=
AC_LANG_POP

AC_LANG_PUSH(C)
ac_c_werror_flag=yes
save_CFLAGS=$CFLAGS
CFLAGS="$CFLAGS -Werror"
AC_MSG_CHECKING([if compiler supports visibility attribute])
AC_TRY_COMPILE([__attribute__((visibility("hidden")))],[],
    [AC_MSG_RESULT([yes])
         has_hidden_attrib=yes
    ],
    [AC_MSG_RESULT([no])
         has_hidden_attrib=no
    ]
)
CFLAGS="$save_CFLAGS"
ac_c_werror_flag=
AC_LANG_POP
if test x"$has_hidden_attrib" = x"yes"; then
    AC_DEFINE(HAVE_VISIBILITY_ATTRIB, [1], [Define to 1 if compiler supports __attribute__((visibility("hidden")))])
fi


# Check how linker handles -rdynamic
# Note that AC_LANG_WERROR is not used here since it is more or less useless
# until its effect can be undone. Instead $ac_c_werror_flag is manipulated
# directly. Dirty but effective!
#
AC_LANG_PUSH(C)
ac_c_werror_flag=yes
save_LDFLAGS=$LDFLAGS
LDFLAGS="$LDFLAGS -rdynamic"
AC_MSG_CHECKING([if linker supports -rdynamic])
AC_TRY_LINK([],[return 0;],
    [AC_MSG_RESULT([yes])
    ],
    [AC_MSG_RESULT([no])
        LDFLAGS="$save_LDFLAGS"
    ]
)
ac_c_werror_flag=
AC_LANG_POP

AC_LANG_PUSH(C)
ac_c_werror_flag=yes
AC_MSG_CHECKING([for __sync_synchronize])
AC_TRY_COMPILE([], [__sync_synchronize();],
    [AC_MSG_RESULT([yes])
         has_synchronize=yes
    ],
    [AC_MSG_RESULT([no])
         has_synchronize=no
    ]
)
ac_c_werror_flag=
AC_LANG_POP
if test x"$has_synchronize" = x"yes"; then
    AC_DEFINE(HAVE_SYNC_SYNCHRONIZE, [1], [Define to 1 if __sync_synchronize() is available])
fi

# Check for GNU style dirname (modifies input argument, thread safe).
# If we are cross-compiling we cannot determine this so use the fallback.
# The latter should not perform much worse than the actual GNU C Library
# version of dirname(3). The fallback will be used on platforms such as
# Mac OS X, which lacks a thread safe GNU style dirname(3) implementation.
AC_LANG_PUSH(C)
AC_MSG_CHECKING([for GNU style dirname])
if test x"$cross_compiling" = x"no"; then
    AC_RUN_IFELSE([
        AC_LANG_PROGRAM([[
             #include <libgen.h>
             #include <string.h>]],
    [[
        char *path = strdup("/a/b/c");
        char *tmp = path;
        if (dirname(path) == tmp)
            return 0;
        return -1;
    ]])],
        [AC_MSG_RESULT([yes])
         AC_DEFINE(HAVE_GNU_DIRNAME, [1], [Define to 1 if you have GNU style dirname])
        ],
        [AC_MSG_RESULT([no])
        ]
    )
else
    AC_MSG_RESULT([skipped])
fi
AC_LANG_POP

# Check if we have errno ENOATTR, typically not on Linux
AC_LANG_PUSH(C)
ac_c_werror_flag=yes
AC_MSG_CHECKING([for errno ENOATTR])
AC_TRY_COMPILE([#include <errno.h>], [return ENOATTR;],
    [AC_MSG_RESULT([yes])
        AC_DEFINE(HAVE_ENOATTR, [1], [Define to 1 if errno ENOATTR is available])
    ],
    [AC_MSG_RESULT([no])
    ]
)
ac_c_werror_flag=
AC_LANG_POP

AC_PROG_LN_S
AC_PROG_MKDIR_P

# Do some final preparations before we finish
#
CC="$PTHREAD_CC"
CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
CXXFLAGS="$CXXFLAGS $PTHREAD_CFLAGS"

AC_CONFIG_FILES([Makefile])
AC_CONFIG_FILES([src/Makefile])
AC_CONFIG_FILES([man/Makefile])
AC_OUTPUT


