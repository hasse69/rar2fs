/*
    Copyright (C) 2009 Hans Beckerus (hans.beckerus@gmail.com)

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

#ifndef RARCONFIG_H_
#define RARCONFIG_H_

#include <platform.h>

#define RAR_SEEK_LENGTH_PROP 0x01
#define RAR_SAVE_EOF_PROP 0x02
#define RAR_PASSWORD_PROP 0x04

void rarconfig_init(const char *source, const char *cfg);
void rarconfig_destroy();

#define rarconfig_getprop_(type, path, prop) \
        rarconfig_getprop_##type(path, prop)
#define rarconfig_getprop(type, path, prop) \
        rarconfig_getprop_(type, path, prop)
int rarconfig_getprop_int(const char *path, int prop);
const char *rarconfig_getprop_char(const char *path, int prop);
const wchar_t *rarconfig_getprop_wchar(const char *path, int prop);
const char *rarconfig_getalias(const char *path, const char *file);
void rarconfig_setalias(const char *path, const char *file, const char *alias);

#endif

