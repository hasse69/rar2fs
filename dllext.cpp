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

// Override the St() function/macro since it is a no-op in RARDLL mode
#undef St
#define St(x) L""x""

#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
static int RarErrorToDll(RAR_EXIT ErrCode);
#else
static int RarErrorToDll(int ErrCode);
#endif

struct DataSet
{
  CommandData Cmd;
#if RARVER_MAJOR > 4 && !(RARVER_MINOR == 0 && RARVER_BETA == 1)
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

HANDLE PASCAL RARInitArchiveEx(struct RAROpenArchiveDataEx *r, FileHandle fh, 
    const bool IsArchiveWorkaround)
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

    char AnsiArcName[NM];
    if (r->ArcName==NULL && r->ArcNameW!=NULL)
    {
      WideToChar(r->ArcNameW,AnsiArcName,NM);
      r->ArcName=AnsiArcName;
    }

    Data->Cmd.AddArcName(r->ArcName,r->ArcNameW);
#else
    Data->Cmd.FileArgs.AddString(L"*");

    char *ArcName = NULL;
    if (r->ArcName!=NULL && r->ArcNameW==NULL)
    {
      ArcName = r->ArcName;
    }

    wchar ArcNameW[NM];
    GetWideName(ArcName,r->ArcNameW,ArcNameW,ASIZE(ArcNameW));

    Data->Cmd.AddArcName(ArcNameW);
#endif
    Data->Cmd.Overwrite=OVERWRITE_ALL;
    Data->Cmd.VersionControl=1;
    Data->Cmd.Callback=r->Callback;
    Data->Cmd.UserData=r->UserData;
    ((FileExt*)&Data->Arc)->SetHandle(fh);
    ((FileExt*)&Data->Arc)->SkipHandle();
#if RARVER_MAJOR > 4
    int64 SavePos = Data->Arc.Tell();   // IsArchive() might destroy file position!
#endif
    if (!Data->Arc.IsArchive(false))
    {
      r->OpenResult=Data->Cmd.DllError!=0 ? Data->Cmd.DllError:ERAR_BAD_ARCHIVE;
      delete Data;
      return(NULL);
    }
#if RARVER_MAJOR > 4
    // Restore file position!
    if (IsArchiveWorkaround)
    {
      if (Data->Arc.Format >= RARFMT50)
        Data->Arc.Seek(SavePos + Data->Arc.Tell() + 1, SEEK_SET); 
      else
        Data->Arc.Seek(SavePos + Data->Arc.Tell(), SEEK_SET);  
      Data->Arc.RawSeek(Data->Arc.Tell(), SEEK_SET);  
    }
    else
    {
      Data->Arc.RawSeek(SavePos, SEEK_SET);  
    }
#endif
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
#if  RARVER_MAJOR > 5 || ( RARVER_MAJOR == 5 && RARVER_MINOR >= 10 )
    Data->Extract.ExtractArchiveInit(Data->Arc);
#else
    Data->Extract.ExtractArchiveInit(&Data->Cmd,Data->Arc);
#endif
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


int PASCAL RARListArchiveEx(HANDLE hArcData, RARArchiveListEx* N, off_t* FileDataEnd, int *ResultCode)
{
  DataSet *Data = (DataSet *)hArcData;
  Archive& Arc = Data->Arc;
  struct RARHeaderDataEx h;
  RARArchiveListEx* N_ = N;

#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
  try
#endif
  {
    *ResultCode = 0;
    uint FileCount = 0;
    while (1)
    {
      int RHCode = 0;
      int PFCode = 0;
      memset(&h, 0, sizeof(h));
      RHCode = RARReadHeaderEx(hArcData,&h);
      if (RHCode)
      {
        *ResultCode = RHCode;
        break;
      }

      if (FileCount)
      {
        N->next = new RARArchiveListEx;
        N = N->next;
      }
      FileCount++;
      memcpy(&N->hdr, &h, sizeof(h));
      N->HeadSize = Arc.FileHead.HeadSize;
      N->Offset = Arc.CurBlockPos;
      N->hdr.Flags = Arc.FileHead.Flags;

      /* For supporting high-precision timestamp.
       * If not available, this value is set to 0 (1601/01/01 00:00:00.000000000).
       * For reference, see http://support.microsoft.com/kb/167296/en
       */
      memset(&N->RawTime, 0, sizeof(struct RARArchiveListEx::RawTime_));
#if RARVER_MAJOR > 4 
      if (Arc.FileHead.mtime.IsSet())
        N->RawTime.mtime = Arc.FileHead.mtime.GetRaw() - 116444736000000000ULL;
      if (Arc.FileHead.ctime.IsSet())
        N->RawTime.ctime = Arc.FileHead.ctime.GetRaw() - 116444736000000000ULL;
      if (Arc.FileHead.atime.IsSet())
        N->RawTime.atime = Arc.FileHead.atime.GetRaw() - 116444736000000000ULL;
#endif

#if RARVER_MAJOR > 4
      // Map some RAR5 properties to old-style flags if applicable
      if (Arc.Format >= RARFMT50)
      {
        unsigned int mask = LHD_SPLIT_BEFORE|LHD_SPLIT_AFTER|LHD_PASSWORD|LHD_DIRECTORY;
        N->hdr.Flags &= ~mask;
        if (Arc.FileHead.SplitBefore)
        N->hdr.Flags |= LHD_SPLIT_BEFORE;
        if (Arc.FileHead.SplitAfter)
          N->hdr.Flags |= LHD_SPLIT_AFTER;
        if (Arc.FileHead.Encrypted)
          N->hdr.Flags |= LHD_PASSWORD;
        if (Arc.FileHead.Dir)
          N->hdr.Flags |= LHD_DIRECTORY;
      }
#endif

      N->LinkTargetFlags = 0;
#if RARVER_MAJOR < 5
      if (N->hdr.HostOS==HOST_UNIX && (N->hdr.FileAttr & 0xF000)==0xA000)
      {
        if (N->hdr.UnpVer < 50)
        {
          int DataSize=Min(N->hdr.PackSize,sizeof(N->LinkTarget)-1);
          Arc.Read(N->LinkTarget,DataSize);
          N->LinkTarget[DataSize]=0;
        }
      }
#else
      if (Arc.FileHead.RedirType != FSREDIR_NONE)
      {
        // Sanity check only that 'RedirType' match 'FileAttr'
        if (Arc.FileHead.RedirType == FSREDIR_UNIXSYMLINK &&
          (N->hdr.FileAttr & 0xF000)==0xA000)
        {
          if (N->hdr.UnpVer < 50)
          {
            int DataSize=Min(N->hdr.PackSize,sizeof(N->LinkTarget)-1);
            Arc.Read(N->LinkTarget,DataSize);
            N->LinkTarget[DataSize]=0;
          } 
          else
          {
            wcscpy(N->LinkTargetW,Arc.FileHead.RedirName);
            N->LinkTargetFlags |= LINK_T_UNICODE; // Make sure UNICODE is set
          }
        }
        else if (Arc.FileHead.RedirType == FSREDIR_FILECOPY)
        {
            wcscpy(N->LinkTargetW,Arc.FileHead.RedirName);
            N->LinkTargetFlags |= LINK_T_FILECOPY;
        }
      }
#endif
      
      if (FileDataEnd)
        *FileDataEnd = Arc.NextBlockPos;

      // Skip to next header
      PFCode = RARProcessFile(hArcData,RAR_SKIP,NULL,NULL);
      if (PFCode)
      {
        *ResultCode = PFCode;
        break;
      }
    }

    N->next = NULL;
    return FileCount;
  }
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
  catch (std::bad_alloc) // Catch 'new' exception.
  {
    if (N->next != NULL)
      delete N->next;
    N->next = NULL;
    cerr << "RARListArchiveEx() caught std:bac_alloc error" << endl;
  }
#endif
  RARFreeListEx(N_);
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


unsigned int PASCAL RARGetMainHeaderSize(HANDLE hArcData)
{
  DataSet *Data=(DataSet*)hArcData;
  return Data->Arc.MainHead.HeadSize;
}
 
 	
unsigned int PASCAL RARGetMarkHeaderSize(HANDLE hArcData)
{
#if RARVER_MAJOR > 4
  DataSet *Data=(DataSet*)hArcData;
  return (Data->Arc.FileHead.UnpVer >= 50 ? SIZEOF_MARKHEAD5 : SIZEOF_MARKHEAD3);
#else
  (void)hArcData;
  return SIZEOF_MARKHEAD;
#endif
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
  WideToChar(NextName,arch,strlen(arch)+1);
#endif
}


void PASCAL RARVolNameToFirstName(char* arch, bool oldstylevolume)
{
#if RARVER_MAJOR < 5
  VolNameToFirstName(arch, arch, !oldstylevolume);
#else
  wchar ArcName[NM];
  CharToWide(arch, ArcName, ASIZE(ArcName));
#if  RARVER_MAJOR > 5 || ( RARVER_MAJOR == 5 && RARVER_MINOR >= 10 )
  VolNameToFirstName(ArcName, ArcName, ASIZE(ArcName), !oldstylevolume);
#else
  VolNameToFirstName(ArcName, ArcName, !oldstylevolume);
#endif
  WideToChar(ArcName,arch,strlen(arch)+1);
#endif
}

#if RARVER_MAJOR > 4
static size_t ListFileHeader(wchar *,Archive &);
#endif
void PASCAL RARGetFileInfo(HANDLE hArcData, const char *FileName, struct RARWcb *wcb)
{
#if RARVER_MAJOR > 4
  char FileNameUtf[NM];
  DataSet *Data = (DataSet *)hArcData;
  Archive& Arc = Data->Arc;
  struct RARHeaderDataEx h;

  memset(&h, 0, sizeof(h));
  wcb->bytes = 0;
  while (!RARReadHeaderEx(hArcData, &h))
  {
    WideToUtf(Arc.FileHead.FileName,FileNameUtf,ASIZE(FileNameUtf));
    if (!strcmp(FileNameUtf, FileName))
    {
      wcb->bytes = ListFileHeader(wcb->data, Arc);
      return;
    }
    (void)RARProcessFile(hArcData,RAR_SKIP,NULL,NULL);
  }
#else
  (void)hArcData;
  (void)FileName;
  wcb->bytes = 0;
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


#if RARVER_MAJOR > 4
// For compatibility with existing translations we use %s to print Unicode
// strings in format strings and convert them to %ls here. %s could work
// without such conversion in Windows, but not in Unix wprintf.
// Note that this function cannot be declared static in some early versions
// of UnRAR source 5.x.x since it is already declared extern by 
// unrar/strfn.hpp!
#if RARVER_MINOR > 0 || (RARVER_BETA == 0 || RARVER_BETA > 7)
void static PrintfPrepareFmt(const wchar *Org,wchar *Cvt,size_t MaxSize)
#else
void PrintfPrepareFmt(const wchar *Org,wchar *Cvt,size_t MaxSize)
#endif
{
  uint Src=0,Dest=0;
  while (Org[Src]!=0 && Dest<MaxSize-1)
  {
    if (Org[Src]=='%' && (Src==0 || Org[Src-1]!='%'))
    {
      uint SPos=Src+1;
      // Skipping a possible width specifier like %-50s.
      while (IsDigit(Org[SPos]) || Org[SPos]=='-')
        SPos++;
      if (Org[SPos]=='s' && Dest<MaxSize-(SPos-Src+1))
      {
        while (Src<SPos)
          Cvt[Dest++]=Org[Src++];
        Cvt[Dest++]='l';
      }
    }

    Cvt[Dest++]=Org[Src++];
  }
  Cvt[Dest]=0;
}


#undef St
#define St(StringId) L""StringId""

static int msprintf(wchar *wcs, const wchar *fmt,...)
{
  va_list arglist;
  int len;
  // This buffer is for format string only, not for entire output,
  // so it can be short enough.
  wchar fmtw[1024];
  va_start(arglist,fmt);
  PrintfPrepareFmt(fmt,fmtw,ASIZE(fmtw));
  len = vswprintf(wcs,1024,fmtw,arglist);
  len = len == -1 ? 0 : len;
  va_end(arglist);
  return len;
}


// This function is stolen with pride as-is from UnRAR source since
// it is not available in SILENT/RARDLL mode.
static void ListFileAttr(uint A,HOST_SYSTEM_TYPE HostType,wchar *AttrStr,size_t AttrSize)
{
  switch(HostType)
  {
    case HSYS_WINDOWS:
      swprintf(AttrStr,AttrSize,L"%c%c%c%c%c%c%c",
              (A & 0x2000) ? 'I' : '.',  // Not content indexed.
              (A & 0x0800) ? 'C' : '.',  // Compressed.
              (A & 0x0020) ? 'A' : '.',  // Archive.
              (A & 0x0010) ? 'D' : '.',  // Directory.
              (A & 0x0004) ? 'S' : '.',  // System.
              (A & 0x0002) ? 'H' : '.',  // Hidden.
              (A & 0x0001) ? 'R' : '.'); // Read-only.
      break;
    case HSYS_UNIX:
      switch (A & 0xF000)
      {
        case 0x4000:
          AttrStr[0]='d';
          break;
        case 0xA000:
          AttrStr[0]='l';
          break;
        default:
          AttrStr[0]='-';
          break;
      }
      swprintf(AttrStr+1,AttrSize-1,L"%c%c%c%c%c%c%c%c%c",
              (A & 0x0100) ? 'r' : '-',
              (A & 0x0080) ? 'w' : '-',
              (A & 0x0040) ? ((A & 0x0800) ? 's':'x'):((A & 0x0800) ? 'S':'-'),
              (A & 0x0020) ? 'r' : '-',
              (A & 0x0010) ? 'w' : '-',
              (A & 0x0008) ? ((A & 0x0400) ? 's':'x'):((A & 0x0400) ? 'S':'-'),
              (A & 0x0004) ? 'r' : '-',
              (A & 0x0002) ? 'w' : '-',
              (A & 0x0001) ? 'x' : '-');
      break;
    case HSYS_UNKNOWN:
      wcscpy(AttrStr,L"?");
      break;
  }
}

// This is a variant of ListFileHeader() function in UnRAR source  
// (somewhat simplfied) since that function is not available in 
// SILENT/RARDLL mode.
// This function outputs the header information in technical format
// to a wcs buffer instead of a file pointer (stderr/stdout).
static size_t ListFileHeader(wchar *wcs,Archive &Arc)
{
  FileHeader &hd=Arc.FileHead;
  wchar *Name=hd.FileName;
  RARFORMAT Format=Arc.Format;

  void *wcs_start = (void *)wcs;

  wchar UnpSizeText[20],PackSizeText[20];
  if (hd.UnpSize==INT64NDF)
    wcscpy(UnpSizeText,L"?");
  else
    itoa(hd.UnpSize,UnpSizeText);
  itoa(hd.PackSize,PackSizeText);

  wchar AttrStr[30];
  ListFileAttr(hd.FileAttr,hd.HSType,AttrStr,ASIZE(AttrStr));

  wchar RatioStr[10];

  if (hd.SplitBefore && hd.SplitAfter)
    wcscpy(RatioStr,L"<->");
  else
    if (hd.SplitBefore)
      wcscpy(RatioStr,L"<--");
    else
      if (hd.SplitAfter)
        wcscpy(RatioStr,L"-->");
      else
        swprintf(RatioStr,ASIZE(RatioStr),L"%d%%",ToPercentUnlim(hd.PackSize,hd.UnpSize));

  wchar DateStr[50];
  hd.mtime.GetText(DateStr,ASIZE(DateStr),true,true);
  wcs += msprintf(wcs, L"\n%12s: %s",St(MListName),Name);
  bool FileBlock=hd.HeaderType==HEAD_FILE;

  const wchar *Type=FileBlock ? (hd.Dir?St(MListDir):St(MListFile)):St(MListService);

  switch(hd.RedirType)
  {
    case FSREDIR_UNIXSYMLINK:
      Type=St(MListUSymlink); break;
    case FSREDIR_WINSYMLINK:
      Type=St(MListWSymlink); break;
    case FSREDIR_JUNCTION:
      Type=St(MListJunction); break;
    case FSREDIR_HARDLINK:
      Type=St(MListHardlink); break;
    case FSREDIR_FILECOPY:
      Type=St(MListCopy);     break;
    case FSREDIR_NONE:
      break;
  }
  wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListType),Type);

  if (hd.RedirType!=FSREDIR_NONE)
  {
    if (Format==RARFMT15)
    {
      char LinkTargetA[NM];
      if (Arc.FileHead.Encrypted)
      {
        // Link data are encrypted. We would need to ask for password
        // and initialize decryption routine to display the link target.
        strncpyz(LinkTargetA,"*<-?->",ASIZE(LinkTargetA));
      }
      else
      {
        int DataSize=(int)Min((size_t)hd.PackSize,ASIZE(LinkTargetA)-1);
        Arc.Read(LinkTargetA,DataSize);
        LinkTargetA[DataSize > 0 ? DataSize : 0] = 0;
      }
      wchar LinkTarget[NM];
      CharToWide(LinkTargetA,LinkTarget,ASIZE(LinkTarget));
      wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListTarget),LinkTarget);
    }
    else
      wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListTarget),hd.RedirName);
  }

  if (!hd.Dir)
  {
    wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListSize),UnpSizeText);
    wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListPacked),PackSizeText);
    wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListRatio),RatioStr);
  }
  if (hd.mtime.IsSet())
    wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListMtime),DateStr);
  if (hd.ctime.IsSet())
  {
    hd.ctime.GetText(DateStr,ASIZE(DateStr),true,true);
    wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListCtime),DateStr);
  }
  if (hd.atime.IsSet())
  {
    hd.atime.GetText(DateStr,ASIZE(DateStr),true,true);
    wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListAtime),DateStr);
  }
  wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListAttr),AttrStr);
  if (hd.FileHash.Type==HASH_CRC32)
    wcs += msprintf(wcs, L"\n%12ls: %8.8X",
      hd.UseHashKey ? L"CRC32 MAC":hd.SplitAfter ? L"Pack-CRC32":L"CRC32",
      hd.FileHash.CRC32);
  if (hd.FileHash.Type==HASH_BLAKE2)
  {
    wchar BlakeStr[BLAKE2_DIGEST_SIZE*2+1];
    BinToHex(hd.FileHash.Digest,BLAKE2_DIGEST_SIZE,NULL,BlakeStr,ASIZE(BlakeStr));
    wcs += msprintf(wcs, L"\n%12ls: %ls",
      hd.UseHashKey ? L"BLAKE2 MAC":hd.SplitAfter ? L"Pack-BLAKE2":L"BLAKE2",
      BlakeStr);
  }

  const wchar *HostOS=L"";
  if (Format==RARFMT50 && hd.HSType!=HSYS_UNKNOWN)
    HostOS=hd.HSType==HSYS_WINDOWS ? L"Windows":L"Unix";
  if (Format==RARFMT15)
  {
    static const wchar *RarOS[]={
      L"DOS",L"OS/2",L"Windows",L"Unix",L"Mac OS",L"BeOS",L"WinCE",L"",L"",L""
    };
    if (hd.HostOS<ASIZE(RarOS))
      HostOS=RarOS[hd.HostOS];
  }
  if (*HostOS!=0)
    wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListHostOS),HostOS);

  wcs += msprintf(wcs, L"\n%12ls: RAR %ls(v%d) -m%d -md=%d%s",St(MListCompInfo),
          Format==RARFMT15 ? L"3.0":L"5.0",hd.UnpVer,hd.Method,
          hd.WinSize>=0x100000 ? hd.WinSize/0x100000:hd.WinSize/0x400,
          hd.WinSize>=0x100000 ? L"M":L"K");

  if (hd.Solid || hd.Encrypted)
  {
    wcs += msprintf(wcs, L"\n%12ls: ",St(MListFlags));
    if (hd.Solid)
      wcs += msprintf(wcs, L"%ls ",St(MListSolid));
    if (hd.Encrypted)
      wcs += msprintf(wcs, L"%ls ",St(MListEnc));
  }

  if (hd.Version)
  {
    uint Version=ParseVersionFileName(Name,false);
    if (Version!=0)
      wcs += msprintf(wcs, L"\n%12ls: %u",St(MListFileVer),Version);
  }

  if (hd.UnixOwnerSet)
  {
    wcs += msprintf(wcs, L"\n%12ls: ",L"Unix owner");
    if (*hd.UnixOwnerName!=0)
      wcs += msprintf(wcs, L"%ls:",GetWide(hd.UnixOwnerName));
    if (*hd.UnixGroupName!=0)
      wcs += msprintf(wcs, L"%ls",GetWide(hd.UnixGroupName));
    if ((*hd.UnixOwnerName!=0 || *hd.UnixGroupName!=0) && (hd.UnixOwnerNumeric || hd.UnixGroupNumeric))
      wcs += msprintf(wcs, L"  ");
    if (hd.UnixOwnerNumeric)
      wcs += msprintf(wcs, L"#%d:",hd.UnixOwnerID);
    if (hd.UnixGroupNumeric)
      wcs += msprintf(wcs, L"#%d:",hd.UnixGroupID);
  }

  wcs += msprintf(wcs, L"\n\n");
  // The below will cover 4 bytes NULL termination
  return ((char *)wcs - (char *)wcs_start);
}

#endif


