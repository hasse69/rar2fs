

AC_DEFUN([AX_PROTOTYPE_SCANDIR],[
AC_LANG_PUSH([C])
AX_PROTOTYPE(scandir,
 [
  #include <dirent.h>
 ],
 [
  const char * dir = 0;
  struct dirent ***namelist = 0;
  int(*select)(ARG3) = 0;
  scandir(dir, namelist, select, alphasort);
 ],
 ARG3, [const struct dirent *, struct dirent *])
 AC_LANG_POP([C])
])
