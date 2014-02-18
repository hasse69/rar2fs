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

#ifndef HASH_H_
#define HASH_H_

#include <platform.h>

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline uint32_t get_hash(const char *s, uint32_t mask)
{
        /* djb2 xor variant (favored by Bernstein) */

        uint32_t hash = 5381;
        int c;

        while((c = *s++)) {
                /* hash = hash * 33 ^ c */
                hash = ((hash << 5) + hash) ^ c;
        }
        return hash & (mask - 1);
}

#endif
