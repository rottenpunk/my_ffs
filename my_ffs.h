//*****************************************************************************
//
//             my_ffs.h - My Simple Flash File System
//                         John C. Overton
//
//   Description:
//      This include file defines various things for My Flash File
//      System.
//
//*****************************************************************************
#ifndef _FFS_H
#define _FFS_H


#define FFS_MAX_FILENAME_LENGTH   64      // Maximum filename length excluding null termination.

#define FFS_FILE_SYSTEM_VERSION    1      // Implementation version.

//------------------------------------------------------------------------------------------------
// Each sector starts with this header.  A sector is the smallest unit that is erasable
// on a flash device.  Each sector is consecutively numbered beginning with the first
// sector that we will manage.  Each flash device has sectors that we manage and, perhaps,
// sectors that we will not manage.  NOTE: Make total size word aligned.
//------------------------------------------------------------------------------------------------
typedef struct myffs_sector_header
{
   unsigned long  Key;                     // A sanity check key.
   unsigned long  Next;                    // Sector number of next sector for file.
   unsigned long  EraseCount;              // Keep count of erases for sector balancing.
   unsigned char  Version;                 // Version of FFS File system.
   unsigned char  Status;                  // Various flags, see below.
   unsigned short SectorChecksum;          // Checksum of entire sector, when complete.
   unsigned long  SectorLength;            // Length of this sector.
   unsigned long  DataOffset;              // Offset to where data starts.

} FFS_SECTOR_HEADER;

// Key is used as a sanity check.
#define FFS_SECTOR_HEADER_KEY        0x6d666673    // "mffs"

// Possible values for Status...
#define FFS_SECTOR_HEADER_INUSE            0x0f // This sector is in use.
#define FFS_SECTOR_HEADER_INUSE_FILENODE   0xf0 // In use and contains a filenode after header.
#define FFS_SECTOR_HEADER_FREE             0xff // This sector is free.
#define FFS_SECTOR_HEADER_FREE_DIRTY       0x00 // This sector is free but needing to be erased.



//------------------------------------------------------------------------------------------------
// A filenode, or directory entry. If Sector contains the start of a file, then the file
// node immediately follows the sector header.  The start of the file's data follows the
// filenode.  NOTE: Make total size word aligned.
//------------------------------------------------------------------------------------------------
typedef struct myffs_file_node
{
   unsigned char  Permissions;             // Read/write/execute permissions.
   char           Filename[FFS_MAX_FILENAME_LENGTH+1];
   unsigned long  FileSize;                // Total size of file.
   unsigned long  DataTime;                // Data/time in seconds from 1970.
   unsigned long  Count;                   // Count each time a file is created with same name.

} FFS_FILE_NODE;



//------------------------------------------------------------------------------------------------
// File Descriptor table entry.
//------------------------------------------------------------------------------------------------
#define FFS_MAX_FILE_DESCRIPTORS  2

typedef struct myffs_file_descriptor
{
   unsigned char      InUse;               // 1 = inuse.
   unsigned char      Flags;               // Flags from open.
   unsigned char      DeleteOldFile;       // Delete existing file when new file closes.
   unsigned char      WriteFnode;          // We need to write out fnode when file closes.
   unsigned long      FnodeSector;         // Sector where File Node lives.
   unsigned long      OldFnodeSector;      // If we are to delete existing file, here's it's fnode.
   unsigned long      Position;            // Current position into file.
   FFS_FILE_NODE      Fnode;               // Copy of File Node.

} FFS_FILE_DESCRIPTOR;


#define FFS_RDONLY    0x0000
#define FFS_WRONLY    0x0001
#define FFS_RDWR      0x0002
#define FFS_CREATE    0x0100


//------------------------------------------------------------------------------------------------
// File check flags.  These flags are used at initialization time to do a check of the file system.
//------------------------------------------------------------------------------------------------
#define CHECK_SECTOR_NOTSEEN   0x00
#define CHECK_SECTOR_BAD       0x01
#define CHECK_SECTOR_FNODE     0x02
#define CHECK_SECTOR_FREE      0x04
#define CHECK_SECTOR_INUSE     0x08


//------------------------------------------------------------------------------------------------
// The flash section table contains entries that describe sections of a flash device
// that we will manage.  Sections must start on a sector boundary. A sector is the
// smallest unit that is erasable on a flash device.  On a flash device, we will only
// manage (will become part of the file system) sectors that are defined to us by an
// entry in this table.  The end of the table will be marked by Device == 0xff...
//------------------------------------------------------------------------------------------------
typedef struct myffs_flash_section
{
   unsigned char  Device;                  // Device number or 0xff if end of table.
   unsigned long  Start;                   // Number of starting sector to manage relative to 0.
   unsigned long  Count;                   // Number of sectors in this section.
   unsigned long  SectorSize;              // Size of each sector in this section.
   // Read a portion of a sector routine.
   int (*Read) (  struct myffs_flash_section* section,
                  unsigned long              Sector,
                  unsigned long              Offset,
                  unsigned char*             Buffer,
                  int                        Length );
   // Write a portion of a sector routine.
   int (*Write) ( struct myffs_flash_section* section,
                  unsigned long              Sector,
                  unsigned long              Offset,
                  unsigned char*             Buffer,
                  int                        Length );

   // Erase a sector routine.
   int (*Erase) ( struct myffs_flash_section* section,
                  unsigned long              Sector );

} FFS_FLASH_SECTION;


//------------------------------------------------------------------------------------------------
// The global object contains global information for MY_FFS...
//------------------------------------------------------------------------------------------------
typedef struct myffs_globals
{

   bool initializationComplete;

   // Table of usable/allocatable descriptors. When a file is open, a descriptor will be used...
   FFS_FILE_DESCRIPTOR  FileDescriptors[FFS_MAX_FILE_DESCRIPTORS];

   // Keep count of sectors that seem to be bad. This is kind'a a high-water mark...
   unsigned long ErrorSectorCount;

   // Pointer to initialization clean-up array.  This array has one byte for every sector in
   // the file system and is used when Jcffs starts up to go thru all sectors to see if there
   // are sectors that may have been orphaned due to power loss.
   unsigned char* SectorArray;

   // Total sectors in the file system.  Calculated when Jcffs is initialized.
   unsigned long       TotalSectors;

   // At initialization time, we check to see if there are file system errors.  This is a
   // count of sectors that have been somehow cross-linked...
   unsigned long       TotalCrossChain;

} FFS_GLOBALS;


//------------------------------------------------------------------------------------------------
// FFS Return codes...
//------------------------------------------------------------------------------------------------

#define FFS_RC_TOO_MANY_OPEN_FILES     (-1)
#define FFS_RC_FILE_DOES_NOT_EXIST     (-2)
#define FFS_RC_INVALID_FILE_DESCRIPTOR (-3)
#define FFS_RC_INVALID_FILE_POSITION   (-4)
#define FFS_RC_INVALID_SECTOR_NUMBER   (-5)
#define FFS_RC_OUT_OF_SPACE            (-6)
#define FFS_RC_FILE_NOT_FOUND          (-7)
#define FFS_RC_NEW_NAME_EXISTS         (-8)


//------------------------------------------------------------------------------------------------
// MY_FFS API calls...
//------------------------------------------------------------------------------------------------
// Initialize the flash file system...
int FFSInitialize( void );

// Terminate use of the flash file system...
int FFSTerminate( void );

int FFSOpen(  char* Filename, int flags, int permissions );
int FFSClose( int fd );
int FFSRead(  int fd, char* buf, int n );
int FFSWrite( int fd, char* buf, int n  );

int FFSNextDirectory( unsigned long* Handle, FFS_FILE_NODE* Fnode  );
int FFSErase( char* filename  );
int FFSRename( char* filename, char* new_filename );
int FFSSpace(  int Option );
int FFSCheck( void );


//------------------------------------------------------------------------------------------------
// MY_FFS internal functions...
//------------------------------------------------------------------------------------------------


static   int LocatePosition( FFS_FILE_DESCRIPTOR* Fdesc,
                       unsigned long         Position,
                       unsigned long*        Sector,
                       FFS_SECTOR_HEADER*   SecHead,
                       unsigned long*        Offset );

static   int LocateFileNode( char* Filename, FFS_FILE_NODE* RtnFnode, unsigned long* RtnSector);

static   int AllocateSector( unsigned long* NewSector, FFS_SECTOR_HEADER* SecHeader );

static   int AllocateSectorWithFilenode( unsigned long* NewSector, FFS_SECTOR_HEADER* SecHeader );

static   int FindFreeSector( unsigned long*       Sector,
                       FFS_SECTOR_HEADER*  SecHeader,
                       FFS_FLASH_SECTION** Section );

static   int FreeSectors(    unsigned long Sector );

static   int ReadSector(     unsigned long  Sector,
                       unsigned long  Offset,
                       unsigned char* Buffer,
                       int            Length);

static   int WriteSector(    unsigned long  Sector,
                       unsigned long  Offset,
                       unsigned char* Buffer,
                       int            Length );

static   int EraseSector(    unsigned long Sector );

static   int ValidSector(    unsigned long Sector );

static   int GetFlashSectionEntry(  unsigned long        Sector,
                              FFS_FLASH_SECTION** Section,
                              unsigned long*       RelSector );

static   int GetDescriptor( void );

static   int FreeDescriptor( int fd );

static   void StringToUpperCase( char* str );

static   void Initialize( void );



#endif   // _FFS_H

