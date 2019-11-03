AC_DEFUN([AC_PROG_GZIP],[
AC_CHECK_PROGS(gzip,[gzip],no)
export gzip;
AM_CONDITIONAL([HAVE_GZIP], [test x$gzip != xno])
AC_SUBST(gzip)
])
