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

#ifdef _UNIX
#define FileHandle FILE*
#else
#define FileHandle HANDLE;
#endif

#define  LINK_T_UNICODE     0x00000001U
#define  LINK_T_FILECOPY    0x00000002U

#ifndef __cplusplus
/* These are defined here since headers.hpp can not be included from non C++ code */

#define  MHD_VOLUME         0x0001U
#define  MHD_COMMENT        0x0002U
#define  MHD_LOCK           0x0004U
#define  MHD_SOLID          0x0008U
#define  MHD_PACK_COMMENT   0x0010U
#define  MHD_NEWNUMBERING   0x0010U
#define  MHD_AV             0x0020U
#define  MHD_PROTECT        0x0040U
#define  MHD_PASSWORD       0x0080U
#define  MHD_FIRSTVOLUME    0x0100U
#define  MHD_ENCRYPTVER     0x0200U

#define  LHD_SPLIT_BEFORE   0x0001U
#define  LHD_SPLIT_AFTER    0x0002U
#define  LHD_PASSWORD       0x0004U
#define  LHD_COMMENT        0x0008U
#define  LHD_SOLID          0x0010U

#define  LHD_WINDOWMASK     0x00e0U
#define  LHD_WINDOW64       0x0000U
#define  LHD_WINDOW128      0x0020U
#define  LHD_WINDOW256      0x0040U
#define  LHD_WINDOW512      0x0060U
#define  LHD_WINDOW1024     0x0080U
#define  LHD_WINDOW2048     0x00a0U
#define  LHD_WINDOW4096     0x00c0U
#define  LHD_DIRECTORY      0x00e0U

#define  LHD_LARGE          0x0100U
#define  LHD_UNICODE        0x0200U
#define  LHD_SALT           0x0400U
#define  LHD_VERSION        0x0800U
#define  LHD_EXTTIME        0x1000U
#define  LHD_EXTFLAGS       0x2000U

#define  SKIP_IF_UNKNOWN    0x4000U
#define  LONG_BLOCK         0x8000U

#define  EARC_NEXT_VOLUME   0x0001U /* not last volume */
#define  EARC_DATACRC       0x0002U /* store CRC32 of RAR archive (now used only in volumes) */
#define  EARC_REVSPACE      0x0004U /* reserve space for end of REV file 7 byte record */
#define  EARC_VOLNUMBER     0x0008U /* store a number of current volume */

/* Internal implementation, depends on archive format version. */
enum HOST_SYSTEM {
  /* RAR 5.0 host OS */
  HOST5_WINDOWS=0,HOST5_UNIX=1,

  /* RAR 3.0 host OS. */
  HOST_MSDOS=0,HOST_OS2=1,HOST_WIN32=2,HOST_UNIX=3,HOST_MACOS=4,
  HOST_BEOS=5,HOST_MAX
};

#define MAXPASSWORD 128

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

HANDLE       PASCAL RARInitArchiveEx(struct RAROpenArchiveDataEx *ArchiveData, FileHandle, bool IsArchiveWorkaround);
int          PASCAL RARFreeArchive(HANDLE hArcData);
int          PASCAL RARListArchiveEx(HANDLE hArcData, RARArchiveListEx* fList, off_t* FileDataEnd, int *ResultCode);
void         PASCAL RARFreeListEx(RARArchiveListEx* fList);
unsigned int PASCAL RARGetMainHeaderSize(HANDLE hArcData);
unsigned int PASCAL RARGetMarkHeaderSize(HANDLE hArcData);
FileHandle   PASCAL RARGetFileHandle(HANDLE hArcData);
void         PASCAL RARNextVolumeName(char*, bool);
void         PASCAL RARVolNameToFirstName(char*, bool);
void         PASCAL RARGetFileInfo(HANDLE hArcData, const char *FileName, struct RARWcb *wcb);

#ifdef __cplusplus
}
#endif

#pragma pack()

#undef FileHandle

#endif
