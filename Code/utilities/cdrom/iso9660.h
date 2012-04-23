#ifndef ISO9660_H
#define ISO9660_H

#define TypeCode_BootRecord                     0x00
#define TypeCode_PrimaryVolume                  0x01
#define TypeCode_SupplementaryVolume            0x02
#define TypeCode_VolumePartition                0x03
#define TypeCode_VolumeSetTerminator            0xFF

/* ----------- <RockRidge> ------------ */

// PX - POSIX file attributes

struct ISO9660_RR_PX_Entry_s
{
    char    Signature[2];
    char    Length;
    char    Version;
    int   FileMode[2];
    int   FileLinks[2];
    int   FileUserID[2];
    int   FileGroupID[2];
    int   FileSerialNumber[2];
};

// PN - POSIX device number
struct ISO9660_RR_PN_Entry_s
{
    char    Signature[2];
    char    Length;
    char    Version;
    int   DeviceNumberHi[2];
    int   DeviceNumberLo[2];
};

// SL - Symbolic Link
struct  ISO9660_RR_SL_Entry_s
{
    char    Signature[2];
    char    Length;
    char    Version;
    char    Flags;
    char*   ComponentArea;
};

// NM - Alternate Name
struct  ISO9660_RR_NM_Entry_s
{
    char    Signature[2];
    char    Length;
    char    Version;
    char    Flags;
    char*    Name;
};

// TF - Time stamp(s) for a file
struct  ISO9660_RR_TF_Entry_s
{
    char    Signature[2];
    char    Length;
    char    Version;
    char    Flags;
    char*    Timestamps;
};

/* ----------- </RockRidge> ----------- */


struct ISO9660_BootRecord_s
{
    char    Type;
    char    ID[5];
    char    Version;
    char    BootSystemID[32];
    char    BootID[32];
    char    BootSystemUse[1977];
};

struct  ISO9660_DirectoryEntry_s
{
    char    Length;
    char    ExtAttributeLength;
    int   LBA[2];
    int   DataLength[2];
    char    RecordingDateAndTime[7];
    char    FileFlags;
    char    FileUnitSize;   // interleaved mode only (zero otherwise)
    char    GapSize;        // interleaved mode only (zero otherwise)
    short   VolumeSequenceNumber[2];
    char    LengthOfFileIdentifier;
    char*    FileIdentifier;
};


struct ISO9660_PVD_s 
{
    char    Type;
    char    ID[5];
    char    Version;
    char    Unused0;
    char    SystemID[32];
    char    VolumeID[32];
    char    Unused1[8];
    int     VolumeSpaceSize[2];
    char    Unused2[32];
    short   VolumeSetSize[2];
    short   VolumeSequenceNumber[2];
    short   LogicalBlockSize[2];

    int     PathTableSize[2];
    int     LocationOfTypeLPathTable;
    int     LocationOfOptionalTypeLPathTable;
    int     LocationOfTypeMPathTable;
    int     LocationOfOptionalTypeMPathTable; 
    char    RootDirectoryEntry[34];
    char    PublisherIdentifier[128];
    char    VolumeSetIdentifier[128];
    char    DataPreparerIdentifier[128];
    char    ApplicationIdentifier[128];
    char    CopyrightFileIdentifier[37];
    char    AbstractFileIdentifier[37];
    char    BibliographicFileIdentifier[37];
    char    VolumeCreationDateAndTime[17];
    char    VolumeModificationDateAndTime[17];
    char    VolumeExpirationDateAndTime[17];
    char    VolumeEffectiveDateAndTime[17];
    char    FileStructureVersion;
    char    Unused3;
    char    ApplicationUsed[512];
    char    Reserved[653];
};

#endif
