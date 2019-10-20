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

#ifndef INDEX_H_
#define INDEX_H_

#include <stdint.h>
#include <arpa/inet.h>

#define R2I_MAGIC     (htonl(0x72326900))   /* 'r2i ' */

/* This is the old broken header which was used for version 0.
 * It is obsolete and no longer supported due to the lack of
 * interoperability between 32- and 64-bit platforms, also with
 * respect to different byte order (endian).
 * This version of the header is kept here for reference only and
 ' also in the rather unlikely case some safe conversion can be
 ' made to version 1 or later. */
struct idx_head_broken {
        uint32_t magic;
        uint16_t version;
        uint16_t spare;
        off_t offset;
        size_t size;
};

/* Header fields are in network byte order from version 1 and later */
struct idx_head {
        uint32_t magic;
        uint16_t version;
        uint16_t spare;
        uint64_t offset;
        uint64_t size;
};

struct idx_data {
        struct idx_head head;
        char bytes[1]; /* start of data bytes */
};

#endif
