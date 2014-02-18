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

    This is a C/C++ wrapper for the extension of the freeware Unrar C++
    library (libunrar). It is part of the extension itself. The wrapper
    can be used in source code written in C in order to access and 
    include the C++ library API.

    Unrar source may be used in any software to handle RAR archives
    without limitations free of charge, but cannot be used to re-create
    the RAR compression algorithm, which is proprietary. Distribution
    of modified Unrar source in separate form or as a part of other
    software is permitted, provided that it is clearly stated in
    the documentation and source comments that the code may not be used
    to develop a RAR (WinRAR) compatible archiver.
*/

#ifndef DLLWRAPPER_H_
#define DLLWRAPPER_H_

#ifndef __cplusplus
#include <wchar.h>
#endif

#include "dllext.hpp"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RARHeaderData RARHeaderData;
typedef struct RARHeaderDataEx RARHeaderDataEx;
typedef struct RAROpenArchiveData RAROpenArchiveData;
typedef struct RAROpenArchiveDataEx RAROpenArchiveDataEx;

#ifdef __cplusplus
}
#endif

#endif
