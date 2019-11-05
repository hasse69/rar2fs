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

#include <platform.h>
#include <libgen.h>
#include <string.h>

#ifndef HAVE_MEMRCHR
/*!
 *****************************************************************************
 * Fallback function if memrchr(3) GNU extension is not available.
 * Somewhat less optimized but will do the job pretty well.
 ****************************************************************************/
static inline void *memrchr(const void *s, int c_in, size_t n)
{
        const unsigned char *p;
        unsigned char c;

        c = (unsigned char)c_in;
        p = (const unsigned char *)s + n;
        while (n-- > 0)
                if (*--p == c)
                        return (void *)p;
	return 0;
}
#endif

/*!
 *****************************************************************************
 * dirname - return directory part of PATH.
 * Copyright (C) 1996-2019 Free Software Foundation, Inc.
 *
 * Contributed by Ulrich Drepper <drepper@cygnus.com>, 1996.
 * The GNU C Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * The GNU C Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * You should have received a copy of the GNU Lesser General Public
 * License along with the GNU C Library; if not, see
 * <http://www.gnu.org/licenses/>.
 *
 * This is more or less a direct copy of the function from the GNU C Library.
 * It has been renamed to not clash with the existing library function and
 * also re-formatted to align with the rest of the design.
 '
 * The purpose of this function is to avoid the dilemma with dirname(3)'s
 * many different implementations. Some are thread safe some are not.
 * Some modify its argument, some do not. If we are going to use dirname(3)
 * we have to treat it as being both non thread safe and input modifying.
 * At which point it is much easier to use something else that is known to
 * implement the same semantics across all platforms.
 *
 ****************************************************************************/
char *__gnu_dirname(char *path)
{
        static const char dot[] = ".";
        char *last_slash;

        /* Find last '/'. */
        last_slash = path != NULL ? strrchr(path, '/') : NULL;
        if (last_slash != NULL && last_slash != path && last_slash[1] == '\0') {
                /* Determine whether all remaining characters are slashes. */
                char *runp;
                for (runp = last_slash; runp != path; --runp)
                        if (runp[-1] != '/')
                                break;
                /* The '/' is the last character, we have to look further. */
                if (runp != path)
                        last_slash = memrchr(path, '/', runp - path);
        }
        if (last_slash != NULL) {
                /* Determine whether all remaining characters are slashes. */
                char *runp;
                for (runp = last_slash; runp != path; --runp)
                        if (runp[-1] != '/')
                                break;
                /* Terminate the path.  */
                if (runp == path) {
                        /* The last slash is the first character in the string.
                           We have to return "/".  As a special case we have to
                           return "//" if there are exactly two slashes at the
                           beginning of the string. See XBD 4.10 Path Name
                           Resolution for more information. */
                        if (last_slash == path + 1)
                                ++last_slash;
                        else
                                last_slash = path + 1;
                } else {
                        last_slash = runp;
                }
                last_slash[0] = '\0';
        } else {
                /* This assignment is ill-designed but the XPG specs require to
                   return a string containing "." in any case no directory part is
                   found and so a static and constant string is required. */
                path = (char *)dot;
        }

        return path;
}
