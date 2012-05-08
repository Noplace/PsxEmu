/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#include <windows.h>
#include "iso9660.h"

void read_iso_cd() {
  HANDLE device = CreateFile("\\\\.\\E:",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
  
  DISK_GEOMETRY         geometry;
  PREVENT_MEDIA_REMOVAL cdlock;
  cdlock.PreventMediaRemoval = true;
  DWORD not_used;
  if (device == INVALID_HANDLE_VALUE)
    return;
  

  DeviceIoControl(device,IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &geometry, sizeof(geometry),&not_used,NULL);

  SetFilePointer(device,geometry.BytesPerSector*16,NULL,FILE_BEGIN);

  LPBYTE sector_ptr;

  sector_ptr = (LPBYTE)VirtualAlloc(NULL, geometry.BytesPerSector, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);

  ReadFile (device, sector_ptr, geometry.BytesPerSector, &not_used, NULL);
  
  ISO9660_PVD_s vol_desc;
  memcpy(&vol_desc,sector_ptr,2048);
   
  VirtualFree(sector_ptr,0,MEM_RELEASE);
  CloseHandle(device);
}