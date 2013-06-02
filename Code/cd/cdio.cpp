#include <windows.h>
#include <winioctl.h>


HANDLE hCD;

void cdioInit() {
  hCD = CreateFile ("\\\\.\\F:", GENERIC_READ,
                     FILE_SHARE_READ|FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                     NULL);

}


void cdioDeinit() {
  CloseHandle(hCD);
}


void cdioTest() {
  DWORD   dwNotUsed;
  if (hCD != INVALID_HANDLE_VALUE)
   {
      DISK_GEOMETRY         dgCDROM;
      PREVENT_MEDIA_REMOVAL pmrLockCDROM;

      // Lock the compact disc in the CD-ROM drive to prevent accidental
      // removal while reading from it.
      pmrLockCDROM.PreventMediaRemoval = TRUE;
      DeviceIoControl (hCD, IOCTL_DISK_MEDIA_REMOVAL,
                       &pmrLockCDROM, sizeof(pmrLockCDROM), NULL,
                       0, &dwNotUsed, NULL);

      // Get sector size of compact disc
      if (DeviceIoControl (hCD, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                           NULL, 0, &dgCDROM, sizeof(dgCDROM),
                           &dwNotUsed, NULL))
      {
         LPBYTE lpSector;
         DWORD  dwSize = 2 * dgCDROM.BytesPerSector;  // 2 sectors

         // Allocate buffer to hold sectors from compact disc. Note that
         // the buffer will be allocated on a sector boundary because the
         // allocation granularity is larger than the size of a sector on a
         // compact disk.
         lpSector = (LPBYTE)VirtualAlloc (NULL, dwSize,
                                  MEM_COMMIT|MEM_RESERVE,
                                  PAGE_READWRITE);

         // Move to 16th sector for something interesting to read.
         SetFilePointer (hCD, dgCDROM.BytesPerSector * 16,
                         NULL, FILE_BEGIN);

         // Read sectors from the compact disc and write them to a file.
         if (ReadFile (hCD, lpSector, dwSize, &dwNotUsed, NULL)) {

         }
            //WriteFile (hFile, lpSector, dwSize, &dwNotUsed, NULL);

         VirtualFree (lpSector, 0, MEM_RELEASE);
      }

      // Unlock the disc in the CD-ROM drive.
      pmrLockCDROM.PreventMediaRemoval = FALSE;
      DeviceIoControl (hCD, IOCTL_DISK_MEDIA_REMOVAL,
                       &pmrLockCDROM, sizeof(pmrLockCDROM), NULL,
                       0, &dwNotUsed, NULL);
  }
}