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

#ifndef _RAR_FILEEXT_
#define _RAR_FILEEXT_

#include <version.hpp>

typedef FILE* FileHandle;
#define BAD_HANDLE NULL

class FileExt
{
  private:
    FileHandle hFile;
    bool LastWrite;
    FILE_HANDLETYPE HandleType;
    bool SkipClose;
    bool IgnoreReadErrors;
    bool NewFile;
    bool AllowDelete;
    bool AllowExceptions;
  protected:
    bool OpenShared;
  public:
#if RARVER_MAJOR < 5
    char FileName[NM];
#endif
    wchar FileNameW[NM];

    FILE_ERRORTYPE ErrorType;
#if RARVER_MAJOR < 5
    uint CloseCount;
#endif
  public:
    FileExt();
    virtual ~FileExt();
    FileHandle GetHandle() {return(hFile);};
    void SetHandle(FileHandle FH) {hFile=FH;};
    void SkipHandle() {SkipClose=true;};
};

#endif
