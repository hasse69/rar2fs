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

    This is an extension of the freeware Unrar C++ library (libunrar).
    It requires the complete unrar source package in order to compile.

    Unrar source may be used in any software to handle RAR archives
    without limitations free of charge, but cannot be used to re-create
    the RAR compression algorithm, which is proprietary. Distribution
    of modified Unrar source in separate form or as a part of other
    software is permitted, provided that it is clearly stated in
    the documentation and source comments that the code may not be used
    to develop a RAR (WinRAR) compatible archiver.
*/

#ifndef _UNRAR_DLLEXT_
#define _UNRAR_DLLEXT_

#include <platform.h>

#ifndef _UNIX
#if defined ( __unix ) || defined ( __unix__ ) || defined ( unix ) || defined ( __APPLE__ )
#define _UNIX
#endif
#endif
#include "dll.hpp"
#include "version.hpp"
#if RARVER_MAJOR > 4
#include "headers5.hpp"
#endif

#define  LINK_T_UNICODE     0x00000001U
#define  LINK_T_FILECOPY    0x00000002U

/* Later versions of UnRAR source/dll should define these.
 * Assume that if one of these are not defined, they all need to
 * be defined here instead for backwards compatibility. */
#ifndef ROADF_VOLUME
#define ROADF_VOLUME       0x0001
#define ROADF_COMMENT      0x0002
#define ROADF_LOCK         0x0004
#define ROADF_SOLID        0x0008
#define ROADF_NEWNUMBERING 0x0010
#define ROADF_SIGNED       0x0020
#define ROADF_RECOVERY     0x0040
#define ROADF_ENCHEADERS   0x0080
#define ROADF_FIRSTVOLUME  0x0100
#endif
#ifndef RHDF_SPLITBEFORE
#define RHDF_SPLITBEFORE 0x01
#define RHDF_SPLITAFTER  0x02
#define RHDF_ENCRYPTED   0x04
#define RHDF_SOLID       0x10
#define RHDF_DIRECTORY   0x20
#endif

#ifndef __cplusplus

/* Internal implementation, depends on archive format version. */
enum HOST_SYSTEM {
  /* RAR 5.0 host OS */
  HOST5_WINDOWS=0,HOST5_UNIX=1,

  /* RAR 3.0 host OS. */
  HOST_MSDOS=0,HOST_OS2=1,HOST_WIN32=2,HOST_UNIX=3,HOST_MACOS=4,
  HOST_BEOS=5,HOST_MAX
};

/* These are missing from unrar headers!? */
#define FHD_STORING         0x30U
#define FHD_FASTEST_COMP    0x31U
#define FHD_FAST_COMP       0x32U
#define FHD_NORMAL_COMP     0x33U
#define FHD_GOOD_COMP       0x34U
#define FHD_BEST_COMP       0x35U

#endif

#pragma pack(1)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RARArchiveList RARArchiveList;
typedef struct RARArchiveListEx RARArchiveListEx;

#ifdef __cplusplus
}
#endif

struct RARArchiveListEx
{
  struct RARHeaderDataEx hdr;
  __extension__
  union
  {
    char       LinkTarget[1024];
    wchar_t    LinkTargetW[1024];
  };
  unsigned int LinkTargetFlags;
  struct RawTime_
  {
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
  } RawTime;
  unsigned int HeadSize;
  off_t        Offset;
  off_t        FileDataEnd;
  RARArchiveListEx* next;
};

struct RARWcb
{
  unsigned int bytes;
  wchar_t data[8192]; // 8k should be enough?
};

#ifdef __cplusplus
extern "C" {
#endif

int          PASCAL RARListArchiveEx(HANDLE hArcData, RARArchiveListEx* fList, int *ResultCode);
void         PASCAL RARFreeListEx(RARArchiveListEx* fList);
void         PASCAL RARNextVolumeName(char*, bool);
void         PASCAL RARVolNameToFirstName(char*, bool);
void         PASCAL RARGetFileInfo(HANDLE hArcData, const char *FileName, struct RARWcb *wcb);

#ifdef __cplusplus
}
#endif

#pragma pack()

#undef FileHandle

#endif
