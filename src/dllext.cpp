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
#define St(x) L"" x ""

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

int PASCAL RARListArchiveEx(HANDLE hArcData, RARArchiveDataEx **NN)
{
  DataSet *Data = (DataSet *)hArcData;
  Archive& Arc = Data->Arc;
  struct RARHeaderDataEx h;
  struct RARArchiveDataEx *N;

#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
  try
#endif
  {
    int RHCode = 0;
    memset(&h, 0, sizeof(h));
    RHCode = RARReadHeaderEx(hArcData,&h);
    if (RHCode)
    {
      return RHCode;
    }

    if (!*NN)
    {
      *NN = new RARArchiveDataEx;
    }
    N = *NN;
    memcpy(&N->hdr, &h, sizeof(h));
    N->HeadSize = Arc.FileHead.HeadSize;
    N->Offset = Arc.CurBlockPos;
    N->FileDataEnd = Arc.NextBlockPos;

    // For supporting high-precision timestamp.
    // If not available, this value is set to 0 (1601/01/01 00:00:00.000000000).
    // For reference, see http://support.microsoft.com/kb/167296/en
    memset(&N->RawTime, 0, sizeof(struct RARArchiveDataEx::RawTime_));
#if RARVER_MAJOR > 4
#if RARVER_MAJOR > 5 || (RARVER_MAJOR == 5 && RARVER_MINOR >= 50)
    // High-precision(1 ns) UNIX timestamp from 1970-01-01
    if (Arc.FileHead.mtime.IsSet())
      N->RawTime.mtime = Arc.FileHead.mtime.GetUnixNS();
    if (Arc.FileHead.ctime.IsSet())
      N->RawTime.ctime = Arc.FileHead.ctime.GetUnixNS();
    if (Arc.FileHead.atime.IsSet())
      N->RawTime.atime = Arc.FileHead.atime.GetUnixNS();
#else
    // High-precision(100 ns) Windows timestamp from 1601-01-01
    if (Arc.FileHead.mtime.IsSet())
      N->RawTime.mtime = Arc.FileHead.mtime.GetRaw() - 116444736000000000ULL;
    if (Arc.FileHead.ctime.IsSet())
      N->RawTime.ctime = Arc.FileHead.ctime.GetRaw() - 116444736000000000ULL;
    if (Arc.FileHead.atime.IsSet())
      N->RawTime.atime = Arc.FileHead.atime.GetRaw() - 116444736000000000ULL;
#endif
#endif

    N->hdr.Flags = 0;
#if RARVER_MAJOR < 5
    if ((Arc.FileHead.Flags & LHD_WINDOWMASK) == LHD_DIRECTORY)
      N->hdr.Flags |= RHDF_DIRECTORY;
    if (Arc.FileHead.Flags & LHD_SPLIT_BEFORE)
      N->hdr.Flags |= RHDF_SPLITBEFORE;
    if (Arc.FileHead.Flags & LHD_SPLIT_AFTER)
      N->hdr.Flags |= RHDF_SPLITAFTER;
    if (Arc.FileHead.Flags & LHD_PASSWORD)
      N->hdr.Flags |= RHDF_ENCRYPTED;
    if (Arc.FileHead.Flags & LHD_SOLID)
      N->hdr.Flags |= RHDF_SOLID;
#else
    if (Arc.FileHead.SplitBefore)
      N->hdr.Flags |= RHDF_SPLITBEFORE;
    if (Arc.FileHead.SplitAfter)
      N->hdr.Flags |= RHDF_SPLITAFTER;
    if (Arc.FileHead.Encrypted)
      N->hdr.Flags |= RHDF_ENCRYPTED;
    if (Arc.FileHead.Dir)
      N->hdr.Flags |= RHDF_DIRECTORY;
    if (Arc.FileHead.Solid)
      N->hdr.Flags |= RHDF_SOLID;
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
    // Skip to next header
    return RARProcessFile(hArcData,RAR_SKIP,NULL,NULL);
  }
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
  catch (std::bad_alloc&) // Catch 'new' exception.
  {
    if (*NN) {
      delete *NN;
      *NN = NULL;
    }
    cerr << "RARListArchiveEx() caught std:bac_alloc error" << endl;
  }
#endif
  return 0;
}

void PASCAL RARFreeArchiveDataEx(RARArchiveDataEx **NN)
{
  if (*NN) {
    delete *NN;
    *NN = NULL;
  }
}

void PASCAL RARNextVolumeName(char *arch, bool oldstylevolume)
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


void PASCAL RARVolNameToFirstName(char *arch, bool oldstylevolume)
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
// (somewhat simplified) since that function is not available in 
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
#if ( RARVER_MAJOR > 4 && RARVER_MINOR >= 30 ) || RARVER_MAJOR > 5
    itoa(hd.UnpSize,UnpSizeText,ASIZE(UnpSizeText));
  itoa(hd.PackSize,PackSizeText,ASIZE(PackSizeText));
#else
    itoa(hd.UnpSize,UnpSizeText);
  itoa(hd.PackSize,PackSizeText);
#endif

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
#if ( RARVER_MAJOR > 4 && RARVER_MINOR >= 30 ) || RARVER_MAJOR > 5
  hd.mtime.GetText(DateStr,ASIZE(DateStr),true);
#else
  hd.mtime.GetText(DateStr,ASIZE(DateStr),true,true);
#endif
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
#if ( RARVER_MAJOR > 4 && RARVER_MINOR >= 30 ) || RARVER_MAJOR > 5
    hd.ctime.GetText(DateStr,ASIZE(DateStr),true);
#else
    hd.ctime.GetText(DateStr,ASIZE(DateStr),true,true);
#endif
    wcs += msprintf(wcs, L"\n%12ls: %ls",St(MListCtime),DateStr);
  }
  if (hd.atime.IsSet())
  {
#if ( RARVER_MAJOR > 4 && RARVER_MINOR >= 30 ) || RARVER_MAJOR > 5
    hd.atime.GetText(DateStr,ASIZE(DateStr),true);
#else
    hd.atime.GetText(DateStr,ASIZE(DateStr),true,true);
#endif
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
          Format==RARFMT15 ? L"1.5":L"5.0",
#if RARVER_MAJOR > 5 || ( RARVER_MAJOR == 5 && RARVER_MINOR >= 70 )
          hd.UnpVer==VER_UNKNOWN ? 0 : hd.UnpVer,hd.Method,
#else
          hd.UnpVer,hd.Method,
#endif
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


