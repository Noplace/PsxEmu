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