/*
    Copyright (C) 2009-2013 Hans Beckerus (hans.beckerus@gmail.com)

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
    It requires the complete unrar source in order to compile.

    Unrar source may be used in any software to handle RAR archives
    without limitations free of charge, but cannot be used to re-create
    the RAR compression algorithm, which is proprietary. Distribution
    of modified Unrar source in separate form or as a part of other
    software is permitted, provided that it is clearly stated in
    the documentation and source comments that the code may not be used
    to develop a RAR (WinRAR) compatible archiver.
*/

#include <iostream>
#include "version.hpp"
#include "rar.hpp"
#include "dllext.hpp"
#include "fileext.hpp"
using namespace std;

// Map some old definitions to >=5.0 if applicable
#if RARVER_MAJOR < 5 
#define MainHead NewMhd
#define FileHead NewLhd
#define BrokenHeader BrokenFileHeader
#define HEAD_FILE FILE_HEAD
#define HEAD_ENDARC ENDARC_HEAD
#endif

#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
static int RarErrorToDll(RAR_EXIT ErrCode);
#else
static int RarErrorToDll(int ErrCode);
#endif

struct DataSet
{
  CommandData Cmd;
#if RARVER_MAJOR > 4 && (RARVER_MINOR > 0 || RARVER_BETA > 1)
  Archive Arc;
  CmdExtract Extract;
#else
  CmdExtract Extract;
  Archive Arc;
#endif
  int OpenMode;
  int HeaderSize;

#if RARVER_MAJOR < 5
  DataSet():Arc(&Cmd) {}
#else
  DataSet():Arc(&Cmd), Extract(&Cmd) {};
#endif
};

HANDLE PASCAL RARInitArchive(struct RAROpenArchiveData *r, FileHandle fh)
{
  RAROpenArchiveDataEx rx;
  memset(&rx,0,sizeof(rx));
  rx.ArcName=r->ArcName;
  rx.OpenMode=r->OpenMode;
  rx.CmtBuf=r->CmtBuf;
  rx.CmtBufSize=r->CmtBufSize;
  HANDLE hArc=RARInitArchiveEx(&rx, fh);
  r->OpenResult=rx.OpenResult;
  r->CmtSize=rx.CmtSize;
  r->CmtState=rx.CmtState;
  return(hArc);
}

HANDLE PASCAL RARInitArchiveEx(struct RAROpenArchiveDataEx *r, FileHandle fh)
{
  DataSet *Data=NULL;
  try
  {
    r->OpenResult=0;
    Data=new DataSet;
    Data->Cmd.DllError=0;
    Data->OpenMode=r->OpenMode;
#if RARVER_MAJOR < 5
    Data->Cmd.FileArgs->AddString("*");

    char an[NM];
    if (r->ArcName==NULL && r->ArcNameW!=NULL)
    {
      WideToChar(r->ArcNameW,an,NM);
      r->ArcName=an;
    }

    Data->Cmd.AddArcName(r->ArcName,r->ArcNameW);
#else
    Data->Cmd.FileArgs.AddString(L"*");

    char AnsiArcName[NM];
    *AnsiArcName=0;
    if (r->ArcName!=NULL)
    {
      strncpyz(AnsiArcName,r->ArcName,ASIZE(AnsiArcName));
    }

    wchar ArcName[NM];
    GetWideName(AnsiArcName,r->ArcNameW,ArcName,ASIZE(ArcName));

    Data->Cmd.AddArcName(ArcName);
#endif
    Data->Cmd.Overwrite=OVERWRITE_ALL;
    Data->Cmd.VersionControl=1;
    ((FileExt*)&Data->Arc)->SetHandle(fh);
    ((FileExt*)&Data->Arc)->SkipHandle();
    if (!Data->Arc.IsArchive(false))
    {
      r->OpenResult=Data->Cmd.DllError!=0 ? Data->Cmd.DllError:ERAR_BAD_ARCHIVE;
      delete Data;
      return(NULL);
    }
#if RARVER_MAJOR < 5
    r->Flags=Data->Arc.MainHead.Flags;

    Array<byte> CmtData;
    if (r->CmtBufSize!=0 && Data->Arc.GetComment(&CmtData,NULL))
    {
      r->Flags|=2;
      size_t Size=CmtData.Size()+1;
      r->CmtState=Size>r->CmtBufSize ? ERAR_SMALL_BUF:1;
      r->CmtSize=(uint)Min(Size,r->CmtBufSize);
      memcpy(r->CmtBuf,&CmtData[0],r->CmtSize-1);
      if (Size<=r->CmtBufSize)
        r->CmtBuf[r->CmtSize-1]=0;
    }
    else
      r->CmtState=r->CmtSize=0;
    if (Data->Arc.Signed)
      r->Flags|=0x20;
#else
    r->Flags = 0;

    if (Data->Arc.Volume)
      r->Flags|=0x01;
    if (Data->Arc.Locked)
      r->Flags|=0x04;
    if (Data->Arc.Solid)
      r->Flags|=0x08;
    if (Data->Arc.NewNumbering)
      r->Flags|=0x10;
    if (Data->Arc.Signed)
      r->Flags|=0x20;
    if (Data->Arc.Protected)
      r->Flags|=0x40;
    if (Data->Arc.Encrypted)
      r->Flags|=0x80;
    if (Data->Arc.FirstVolume)
      r->Flags|=0x100;

    Array<wchar> CmtDataW;
    if (r->CmtBufSize!=0 && Data->Arc.GetComment(&CmtDataW))
    {
      Array<char> CmtData(CmtDataW.Size()*4+1);
      memset(&CmtData[0],0,CmtData.Size());
      WideToChar(&CmtDataW[0],&CmtData[0],CmtData.Size()-1);
      size_t Size=strlen(&CmtData[0])+1;

      r->Flags|=2;
      r->CmtState=Size>r->CmtBufSize ? ERAR_SMALL_BUF:1;
      r->CmtSize=(uint)Min(Size,r->CmtBufSize);
      memcpy(r->CmtBuf,&CmtData[0],r->CmtSize-1);
      if (Size<=r->CmtBufSize)
        r->CmtBuf[r->CmtSize-1]=0;
    }
    else
      r->CmtState=r->CmtSize=0;
#endif
    Data->Extract.ExtractArchiveInit(&Data->Cmd,Data->Arc);
    return((HANDLE)Data);
  }
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
  catch (RAR_EXIT ErrCode)
  {
    if (Data!=NULL && Data->Cmd.DllError!=0)
      r->OpenResult=Data->Cmd.DllError;
    else
      r->OpenResult=RarErrorToDll(ErrCode);
    if (Data != NULL)
      delete Data;
    return(NULL);
  }
  catch (std::bad_alloc) // Catch 'new' exception.
  {
    r->OpenResult=ERAR_NO_MEMORY;
    if (Data != NULL)
      delete Data;
  }
#else
  catch (int ErrCode)
  {
    r->OpenResult=RarErrorToDll(ErrCode);
  }
#endif
  return(NULL);
}


int PASCAL RARFreeArchive(HANDLE hArcData)
{
  DataSet *Data=(DataSet *)hArcData;
  bool Success=Data==NULL ? false:true;
  delete Data;
  return(Success ? 0:ERAR_ECLOSE);
}


int PASCAL RARListArchiveEx(HANDLE hArcData, RARArchiveListEx* N, off_t* FileDataEnd)
{
  uint FileCount=0;
  try {
     DataSet *Data=(DataSet *)hArcData;
     Archive& Arc = Data->Arc;

     while(Arc.ReadHeader()>0)
     {
       if (Arc.BrokenHeader)
         break;
       int HeaderType=Arc.GetHeaderType();
       if (HeaderType==HEAD_ENDARC)
       {
         break;
       }
       switch(HeaderType)
       {
         case HEAD_FILE:
           if (FileCount)
           {
             N->next = new RARArchiveListEx;
             N = N->next;
           }
           FileCount++;

#if RARVER_MAJOR < 5
           IntToExt(Arc.FileHead.FileName,Arc.FileHead.FileName);
           strncpyz(N->FileName,Arc.FileHead.FileName,ASIZE(N->FileName));
           if (*Arc.FileHead.FileNameW)
             wcsncpy(N->FileNameW,Arc.FileHead.FileNameW,ASIZE(N->FileNameW));
           else
           {
             CharToWide(Arc.FileHead.FileName,N->FileNameW);
           }
#else
           wcsncpy(N->FileNameW,Arc.FileHead.FileName,ASIZE(N->FileNameW));
           WideToChar(N->FileNameW,N->FileName,ASIZE(N->FileName));
#endif
           N->Flags = Arc.FileHead.Flags;
#if RARVER_MAJOR > 4
           // Map some 5.0 properties to old-style flags if applicable
           if (Arc.Format >= RARFMT50)
           {
             unsigned int mask = LHD_SPLIT_BEFORE|LHD_SPLIT_AFTER|LHD_PASSWORD|LHD_DIRECTORY;
             N->Flags &= ~mask;
             if (Arc.FileHead.SplitBefore)
               N->Flags |= LHD_SPLIT_BEFORE;
             if (Arc.FileHead.SplitAfter)
               N->Flags |= LHD_SPLIT_AFTER;
             if (Arc.FileHead.Encrypted)
               N->Flags |= LHD_PASSWORD;
             if (Arc.FileHead.Dir)
               N->Flags |= LHD_DIRECTORY;
           }
#endif
           N->PackSize = Arc.FileHead.PackSize;
#if RARVER_MAJOR < 5
           N->PackSizeHigh = Arc.FileHead.HighPackSize;
#else
           N->PackSizeHigh = Arc.FileHead.PackSize>>32;
#endif
           N->UnpSize = Arc.FileHead.UnpSize;
#if RARVER_MAJOR < 5
           N->UnpSizeHigh = Arc.FileHead.HighUnpSize;
#else
           N->UnpSizeHigh = Arc.FileHead.UnpSize>>32;
#endif
           N->HostOS = Arc.FileHead.HostOS;
#if RARVER_MAJOR < 5
           N->FileCRC = Arc.FileHead.FileCRC;
           N->FileTime = Arc.FileHead.FileTime;
#else
           N->FileCRC = Arc.FileHead.FileHash.CRC32;
           N->FileTime = Arc.FileHead.mtime.GetDos();
#endif

#if RARVER_MAJOR < 5
           N->UnpVer = Arc.FileHead.UnpVer;
#else
           if (Data->Arc.Format>=RARFMT50)
             N->UnpVer=Data->Arc.FileHead.UnpVer==0 ? 50 : 200; // If it is not 0, just set it to something big.
           else
             N->UnpVer=Data->Arc.FileHead.UnpVer;
#endif

#if RARVER_MAJOR < 5
           N->Method = Arc.FileHead.Method;
#else
           N->Method = Arc.FileHead.Method + 0x30;
#endif
           N->FileAttr = Arc.FileHead.FileAttr;
           N->HeadSize = Arc.FileHead.HeadSize;
           N->Offset = Arc.CurBlockPos;

           if (FileDataEnd)
             *FileDataEnd = Arc.NextBlockPos;
           break;

         default:
           break;
       }
       Arc.SeekToNext();
     }
     N->next = NULL;
     return FileCount;
  }
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
  catch (RAR_EXIT ErrCode)
#else
  catch (int ErrCode)
#endif
  {
    N->next = NULL;
    cerr << "RarListArchiveEx() caught error "
         << RarErrorToDll(ErrCode)
         << endl;
  }
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
  catch (std::bad_alloc) // Catch 'new' exception.
  {
    if (N->next != NULL)
      delete N->next;
    N->next = NULL;
  }
#endif
  return 0;
}


void PASCAL RARFreeListEx(RARArchiveListEx* L)
{
  RARArchiveListEx* N = L?L->next:NULL;
  while (N)
  {
    RARArchiveListEx* tmp = N;
    N = N->next;
    delete tmp;
  }
}


FileHandle PASCAL RARGetFileHandle(HANDLE hArcData)
{
  DataSet *Data=(DataSet*)hArcData;
  return Data->Arc.GetHandle()!=BAD_HANDLE?Data->Arc.GetHandle():NULL;
}


void PASCAL RARNextVolumeName(char* arch, bool oldstylevolume)
{
#if RARVER_MAJOR < 5
  NextVolumeName(arch, NULL, 0, oldstylevolume);
#else
  wchar NextName[NM];
  CharToWide(arch, NextName, ASIZE(NextName));
  NextVolumeName(NextName, ASIZE(NextName), oldstylevolume);
  WideToChar(NextName,arch,NM);
#endif
}


void PASCAL RARVolNameToFirstName(char* arch, bool oldstylevolume)
{
#if RARVER_MAJOR < 5
  VolNameToFirstName(arch, arch, !oldstylevolume);
#else
  wchar ArcName[NM];
  CharToWide(arch, ArcName, ASIZE(ArcName));
  VolNameToFirstName(ArcName, ArcName, !oldstylevolume);
  WideToChar(ArcName,arch,NM);
#endif
}


#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
static int RarErrorToDll(RAR_EXIT ErrCode)
#else
static int RarErrorToDll(int ErrCode)
#endif
{
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
  switch(ErrCode)
  {
    case RARX_FATAL:
      return(ERAR_EREAD);
    case RARX_CRC:
      return(ERAR_BAD_DATA);
    case RARX_WRITE:
      return(ERAR_EWRITE);
    case RARX_OPEN:
      return(ERAR_EOPEN);
    case RARX_CREATE:
      return(ERAR_ECREATE);
    case RARX_MEMORY:
      return(ERAR_NO_MEMORY);
    case RARX_SUCCESS:
      return(0);
    default:
      ;
  }
#else
  switch(ErrCode)
  {
    case FATAL_ERROR:
      return(ERAR_EREAD);
    case CRC_ERROR:
      return(ERAR_BAD_DATA);
    case WRITE_ERROR:
      return(ERAR_EWRITE);
    case OPEN_ERROR:
      return(ERAR_EOPEN);
    case CREATE_ERROR:
      return(ERAR_ECREATE);
    case MEMORY_ERROR:
      return(ERAR_NO_MEMORY);
    case SUCCESS:
      return(0);
    default:
      ;
  }
#endif
  return(ERAR_UNKNOWN);
}

