//***************************************************************************************
//
//             my_ffs.h - My Simple Flash File System
//                         John C. Overton
//
//   Description:
//      This module implements a fairly simple one dimentional file system built on top  
//      of NOR or NAND flash.  
//
//***************************************************************************************

#include "my_ffs.h"
#include <stdlib.h>
#include <strlib.h>
#include <ctype.h>


// External link to section table...
// The section table contains an entry for each physical flash memory part and
// describes the size of allocation units (sectors) on the part and how many 
// are available to use.  Also, the table contains function pointers to
// primative routines to read, write, and erase a sector on the part.
extern FFS_FLASH_SECTION FlashSectionTable[];

// Singleton static object created at runtime...
FFS_GLOBALS  ThemyffsObject;
FFS_GLOBALS* myffsObj = &ThemyffsObject;


//---------------------------------------------------------------------------------------
// At some point we may need some syncronization...
//---------------------------------------------------------------------------------------
//#define FFS_LOCK()      (_flash().lockWrite())
//#define FFS_UNLOCK()    (_flash().unlockWrite())
void FFS_LOCK(void);
void FFS_UNLOCK(void);


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::Jcffs
//
//    Purpose:          Default constructor
//
//    Inputs:           None
//
//    Returns:          None
//
//    Notes:            Optional:
//                      On startup, check to see if this is the first time we are running.
//                      Go thru all sectors and if there are no sectors with our key, then
//                      erase all sectors.
//
//
//---------------------------------------------------------------------------------------
int FFSInitialize( void )
{
   myffsObj->SectorArray = NULL;

   myffsObj->initializationComplete = false;
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::~Jcffs
//
//    Purpose:          Destructor
//
//    Inputs:           None
//
//    Returns:          None
//
//    Notes:
//
//---------------------------------------------------------------------------------------
Jcffs::~Jcffs( void )
{
   FFS_LOCK();

   // Free Sector Array...
   if( myffsObj->SectorArray )
   {
      _mem_free(SectorArray);
   }

   FFS_TERMLOCK();
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::open
//
//    Purpose:
//
//    Inputs:           Filename    - Pointer to file name.
//                      Flags       - Similar to unix open() flags, but
//                                    added create flag:
//                        FFS_RDONLY - Read only
//                        FFS_WRONLY - Write only
//                        FFS_RDWR   - Read/write
//                        FFS_CREATE - Create new file
//                      Permissions - Similar to unix open() permisions
//
//    Returns:          if >0 - A desciptor number is returned.
//                      if <0 - An FFS return code is returned.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::open( char* Filename, int flags, int permissions )
{
   FFS_FILE_NODE*         Fnode;
   FFS_FILE_DESCRIPTOR*   Fdesc;
   int                     fd;
   unsigned long           CreateCount = 0;


   if( initializationComplete == false )
   {
      myffsObj->Initialize();
   }

   // Allocate a descriptor table entry...
   if( (fd = GetDescriptor()) < 0 )
   {
      return fd;                      // fd will have error code from GetDescriptor().
   }

   FFS_LOCK();

   // Now, point to our new entry...
   Fdesc = &(FileDescriptors[fd]);
   Fnode = &(Fdesc->Fnode);                       // Copy ptr for convenience.


   // See if file exists. Find Fnode on flash and copy into memory.
   // LocateFileNode() will return with sector==-1 if file is not found...
   LocateFileNode(Filename, Fnode, &Fdesc->FnodeSector);

   // If we are not creating this file and it doesn't exist, then return error...
   if( !(flags & FFS_CREATE) && Fdesc->FnodeSector == -1 )
   {
      FreeDescriptor(fd);                         // Free our descriptor entry.

      // File does not exist! Return error...
      FFS_UNLOCK();
      return FFS_RC_FILE_DOES_NOT_EXIST;
   }

   // If we are creating a new file, then prepare for new file.
   if( flags && FFS_CREATE )
   {
      // If old file exists, indicate that we want to delete it when the new file
      // is fully written out and closed...
      if( Fdesc->FnodeSector != -1 )
      {
         CreateCount = Fnode->Count + 1;              // Save create count and add one to it.
         Fdesc->DeleteOldFile  = 1;                   // Delete old file when new one closes.
         Fdesc->OldFnodeSector = Fdesc->FnodeSector;  // And this is where old one is.
      }
      else
      {
         // Copy name into in-core Fnode...
         if( strlen(Filename ) >= sizeof(Fnode->Filename) )
         {
            memcpy(Fnode->Filename, Filename, sizeof(Fnode->Filename) - 1);
            Fnode->Filename[ sizeof(Fnode->Filename) - 1 ] = 0;
         }
         else
         {
            strcpy(Fnode->Filename, Filename);
         }
      }
      Fdesc->FnodeSector = -1;                    // Indicate no fnode allocated yet.
      Fnode->FileSize    = 0;                     // No file length to begin with.
      Fnode->Permissions = permissions;           // Save permissions.
      Fnode->Count       = CreateCount;           // Keep track of create count.
      // Fnode->DataTime = time();                // Save date/time of file creation.
   }

   // Set up descriptor...
   Fdesc->Flags = flags;                          // Save open flags.

   FFS_UNLOCK();

   return fd;
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::close
//
//    Purpose:
//
//    Inputs:           fd - File descriptor number (index into file descriptor
//                           table).
//
//    Returns:          Indication of error.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::close( int fd )
{
   FFS_FILE_DESCRIPTOR*  Fdesc;              // ptr to file descriptor entry.

   // Sanity check...
   if(fd > FFS_MAX_FILE_DESCRIPTORS || !FileDescriptors[fd].InUse )
   {
      return FFS_RC_INVALID_FILE_DESCRIPTOR;
   }

   FFS_LOCK();

   Fdesc = &(FileDescriptors[fd]);                // Copy ptr for convenience.

   // If this is a new file, we will have to write out the fnode...
   if( Fdesc->WriteFnode )
   {
      WriteSector( Fdesc->FnodeSector,
                   sizeof(FFS_SECTOR_HEADER),
                   &(Fdesc->Fnode),
                   sizeof(FFS_FILE_NODE) );
   }

   // If this is a new file and there was an existing older file out there, then
   // we need to delete the old file...
   if(Fdesc->DeleteOldFile)
   {
      FreeSectors( Fdesc->OldFnodeSector );
   }

   // Now free the descriptor...
   FreeDescriptor( fd );

   FFS_UNLOCK();

   return 0;
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::read
//
//    Purpose:
//
//    Inputs:           fd  - File descriptor number.
//                      buf - Pointer to buffer to read into.
//                      n   - Length of buffer, maximum size to read.
//
//    Returns:          None
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::read( int fd, char* buf, int n )
{
   FFS_SECTOR_HEADER     SecHead;            // Current sector file pos is in.
   FFS_FILE_NODE*        Fnode;              // Ptr to In-core fnode in file desc entry.
   FFS_FILE_DESCRIPTOR*  Fdesc;              // ptr to file descriptor entry.
   unsigned long          Sector;             // Sector number.
   unsigned long          Offset;             // Offset into current sector.
   unsigned long          RemLen;             // Calc'd rem length in current sector.
   int                    rc;
   int                    TotalRead = 0;      // Total up amount read to return to caller.


   // Sanity check...
   if(fd > FFS_MAX_FILE_DESCRIPTORS || !FileDescriptors[fd].InUse )
   {
      return FFS_RC_INVALID_FILE_DESCRIPTOR;
   }

   Fdesc = &(FileDescriptors[fd]);                // Copy ptr for convenience.
   Fnode = &(Fdesc->Fnode);                       // Copy ptr for convenience.

   // Check current position to see if there is anything left to read...
   if(Fdesc->Position >= Fnode->FileSize )
   {
      return FFS_RC_INVALID_FILE_POSITION;
   }

   FFS_LOCK();

   // Locate position in file by getting which sector we currently are positioned at. If
   // position is invalid (maybe beyond file), then LocatePosition() will return error code.
   if( (rc = LocatePosition(Fdesc, Fdesc->Position, &Sector, &SecHead, &Offset)) < 0)
   {
      FFS_UNLOCK();
      return rc;
   }

   FFS_LOCK();

   // Check to see if there are that many bytes left to read from file. If not, adjust
   // requested number of bytes...
   if( n > (Fnode->FileSize - Fdesc->Position) )
   {
      n = Fnode->FileSize - Fdesc->Position;
   }

   // If we can only do a partial read (we must read more than just the remainder of
   // this sector), then just read the rest of this sector. Then position to next
   // sector...
   while( n )
   {
      // Calculate remaining length in this sector...
      RemLen = SecHead.SectorLength - Offset;

      // Calculate which is smaller: remaining requested or what's left in this sector...
      if( n < RemLen)
      {
         RemLen = n;                              // Just read what's left in this sector.
      }

      ReadSector(Sector, Offset, buf, RemLen);    // Read what we can from this sector.

      n               -= RemLen;                  // Update what is remaining to read.
      Fdesc->Position += RemLen;                  // Update file position.
      TotalRead       += RemLen;                  // Update total read so far.

      if( n == 0 )                                // Are we done reading?
      {
         break;                                   // Yes, we're done reading.
      }

      buf             += RemLen;                  // Update buffer pointer.

      // Read next sector header. If we can't, then there is a problem with file system...
      Sector = SecHead.Next;
      if((rc = ReadSector(Sector, 0, &SecHead, sizeof(FFS_SECTOR_HEADER))) < 0)
      {
         FFS_UNLOCK();
         return rc;
      }

      // Point to where data starts in this sector...
      Offset = SecHead.DataOffset;
   }

   FFS_UNLOCK();

   return TotalRead;
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::write
//
//    Purpose:
//
//    Inputs:           fd  - File descriptor number.
//                      buf - Pointer to buffer to write from.
//                      n   - Length of buffer, maximum size to write.
//
//    Returns:          None
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::write( int fd, char* buf, int n  )
{
   FFS_SECTOR_HEADER     SecHead;            // Current sector file pos is in.
   FFS_FILE_NODE*        Fnode;              // Ptr to In-core fnode in file desc entry.
   FFS_FILE_DESCRIPTOR*  Fdesc;              // Ptr to file descriptor entry.
   unsigned long          Sector;             // Sector number.
   unsigned long          NewSector;          // A newly allocated sector.
   unsigned long          Offset;             // Offset into current sector.
   unsigned long          RemLen;             // Calc'd rem length in current sector.
   int                    rc;
   int                    TotalWritten = 0;   // Total up amount written to return to caller.


   // Sanity check...
   if(fd > FFS_MAX_FILE_DESCRIPTORS || !FileDescriptors[fd].InUse )
   {
      return FFS_RC_INVALID_FILE_DESCRIPTOR;
   }

   FFS_LOCK();

   Fdesc = &(FileDescriptors[fd]);                // Copy ptr for convenience.
   Fnode = &(Fdesc->Fnode);                       // Copy ptr for convenience.

   // Is this a new file and first time?  Then we need to allocate the first sector. The
   // first sector is where the Fnode will live, but not now; when the file is closed.
   if (Fdesc->FnodeSector == -1)
   {

       // Calc offset that is beyond sector header and filenode containing filename...
       Offset = sizeof(FFS_SECTOR_HEADER) + sizeof(FFS_FILE_NODE);

       if( (rc = AllocateSectorWithFilenode( &Sector, &SecHead )) != 0)
       {
          FFS_UNLOCK();
          return rc;
       }

       // Indicate we need to write out fnode when file closes.
       Fdesc->WriteFnode  = 1;
       Fdesc->FnodeSector = Sector;               // And save first sector# where fnode goes.
   }
   else
   {
      // Locate position in file by getting which sector we currently are positioned at. If
      // position is invalid (maybe beyond file), then LocatePosition() will return error code.
      // The nice thing about LocatePosition() is that if the first sector has not been
      // allocated yet, then he will allocate it. LocatePosition() will return position
      // to start writing to...
      if( (rc = LocatePosition(Fdesc, Fdesc->Position, &Sector, &SecHead, &Offset)) < 0)
      {
         FFS_UNLOCK();
         return rc;
      }
   }

   while( n )
   {
      // Calculate remaining length in this sector...
      RemLen = SecHead.SectorLength - Offset;

      // Calculate which is smaller: remaining requested or what's left in this sector...
      if ( n < RemLen)
      {
          RemLen = n;
      }

      // Write out to sector...
      WriteSector(Sector, Offset, buf, RemLen);    // Write what we can into this sector.

      n               -= RemLen;                  // Update what is remaining to read.
      Fdesc->Position += RemLen;                  // Update file position.
      TotalWritten    += RemLen;                  // Update total written so far.

      // Update total file size in Fnode...
      if( Fdesc->Position > Fnode->FileSize )
      {
         Fnode->FileSize = Fdesc->Position;
      }

      // Are we done writing?
      if( n == 0 )
      {
         break;                                   // yes, we're done writing.
      }

      buf             += RemLen;                  // Update buffer pointer.

      // Allocate another sector. If we can't, then we are out of room. AllocateSector()
      // allocates a free sector and writes out an updated sector header, which is also
      // returned.
      if((rc = AllocateSector( &NewSector, &SecHead )) != 0)
      {
         FFS_UNLOCK();
         return rc;
      }

      // Chain new sector to previous one.  When a Sector is allocated, it's Next chain
      // pointer is 0xFFFFFFFF so that we can update it later (like right now)...
      WriteSector( Sector,
                   ((char*)&(SecHead.Next) - (char*)&SecHead),   // Offset to Next field.
                   &NewSector,                   // Update Next field with new sector nbr.
                   sizeof(NewSector));

      // For every sector after the first one, data starts right after header...
      Offset = sizeof(FFS_SECTOR_HEADER);
      // Now make new sector the current sector...
      Sector = NewSector;
   }

   FFS_UNLOCK();

   return TotalWritten;
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::NextDirectory
//
//    Purpose:          Return next Fnode on each call.
//
//    Inputs:           fd  - File descriptor number.
//                      buf - Pointer to buffer to write from.
//                      n   - Length of buffer, maximum size to write.
//
//    Returns:          0   - Operation was successful. An fnode was returned.
//                      1   - Operation complete, no more files.
//                      <0  - Jcffs return code.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::NextDirectory( unsigned long* Handle, FFS_FILE_NODE* Fnode  )
{
   FFS_SECTOR_HEADER     SecHead;            // Current sector file pos is in.
   unsigned long          Sector;             // Sector number.


   if( initializationComplete == false )
   {
      Initialize();
   }

   FFS_LOCK();

   for( Sector = *Handle; ValidSector(Sector); Sector++ )
   {
      ReadSector( Sector, 0, &SecHead, sizeof(FFS_SECTOR_HEADER) );

      if( SecHead.Status == FFS_SECTOR_HEADER_INUSE_FILENODE )
      {
         // Read Fnode...
         ReadSector(  Sector,
                      sizeof(FFS_SECTOR_HEADER),
                      Fnode,
                      sizeof(FFS_FILE_NODE) );

         *Handle = Sector + 1;

         // Check to see if this file is currently being created and there
         // isn't a displayable name...
         if( Fnode->Filename[0] == 0xff && Fnode->FileSize == -1)
         {
            strcpy(Fnode->Filename, "[New File]");
         }

         return 0;
      }
   }

   FFS_UNLOCK();

   return 1;          // No more files.
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::Erase
//
//    Purpose:          Erase a file.
//
//    Inputs:           filename - Name of file to erase.
//
//    Returns:          0 - If operation was successful.
//                      <0 - Returns Jcffs return code.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::Erase( char* filename  )
{
   FFS_FILE_NODE         Fnode;              // File node returned by LocateFileNode().
   unsigned long          Sector;             // Sector number.


   if( initializationComplete == false )
   {
      Initialize();
   }

   FFS_LOCK();

   // See if file exists. Find Fnode on flash and copy into memory.
   // LocateFileNode() will return with sector==-1 if file is not found...
   LocateFileNode(filename, &Fnode, &Sector);

   // See if file was found. If not, return.
   if(Sector == -1)
   {
      FFS_UNLOCK();
      return FFS_RC_FILE_NOT_FOUND;
   }

   // Erase file...
   FreeSectors(Sector);

   FFS_UNLOCK();

   return 0;
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::Rename
//
//    Purpose:          Rename a file.
//
//    Inputs:           filename     - Name of file to rename.
//                      new_filename - new file name.
//
//    Returns:          0 - Operation successful, file renamed.
//                      <0 - Jcffs error code.
//
//    Notes:            Currently, this feature assumes that all sectors are the same size.
//
//                      To perform this feature, we will allocate a new fnode sector,
//                      copy data to new sector, erase old file, save new fnode.
//
//
//---------------------------------------------------------------------------------------
int Jcffs::Rename( char* filename, char* new_filename )
{
   FFS_FILE_NODE         Fnode;              // File node returned by LocateFileNode().
   FFS_SECTOR_HEADER     SecHead;
   unsigned long          Sector;             // Sector number.
   unsigned long          NewSector;          // New sector number.
   unsigned long          NextSector;         // Saved sector chain pointer.
   unsigned char          Buffer[100];
   unsigned long          Length;
   int                    rc;
   unsigned long          Offset;
   int                    n;

   if( initializationComplete == false )
   {
      Initialize();
   }

   FFS_LOCK();

   // See if file exists. Find Fnode on flash and copy into memory.
   // LocateFileNode() will return with sector==-1 if file is not found...
   LocateFileNode(filename, &Fnode, &Sector);

   // See if file was found. If not, return.
   if(Sector == -1)
   {
      FFS_UNLOCK();
      return FFS_RC_FILE_NOT_FOUND;
   }

   // Now, make sure new filename doesn't exist...
   LocateFileNode(new_filename, &Fnode, &NewSector);

   // See if file was found. If it was, return.
   if(NewSector != -1)
   {
      FFS_UNLOCK();
      return FFS_RC_NEW_NAME_EXISTS;
   }

   // Read Sector header...
   ReadSector( Sector, 0, &SecHead, sizeof(FFS_SECTOR_HEADER));

   Length = SecHead.SectorLength - SecHead.DataOffset;
   NextSector = SecHead.Next;

   // Allocate new fnode sector...
   if( (rc = AllocateSectorWithFilenode( &NewSector, &SecHead )) != 0)
   {
      FFS_UNLOCK();
      return rc;
   }

   // Check to make sector is same size...
   if( Length != (SecHead.SectorLength - SecHead.DataOffset) )
   {
      FreeSectors( NewSector );
      FFS_UNLOCK();
      return FFS_RC_OUT_OF_SPACE;
   }

   // Now, copy data from old fnode sector to new fnode sector...
   Offset = SecHead.DataOffset;
   while( Length )
   {
      n = sizeof(Buffer);
      if( Length < n )
      {
         n = Length;
      }
      ReadSector( Sector, Offset, Buffer, n);
      WriteSector( NewSector, Offset, Buffer, n);
      Length -= n;
      Offset += n;
   }

   // OK, now update fnode with new name...
   if( strlen(new_filename ) >= sizeof(Fnode.Filename) )
   {
      memcpy(Fnode.Filename, new_filename, sizeof(Fnode.Filename) - 1);
      Fnode.Filename[ sizeof(Fnode.Filename) - 1 ] = 0;
   }
   else
   {
      strcpy(Fnode.Filename, new_filename);
   }

   // Write new Fnode back out...
   WriteSector( NewSector, sizeof(FFS_SECTOR_HEADER), &Fnode, sizeof(FFS_FILE_NODE));

   // Update chain pointer (if not -1)...
   if( NextSector != -1 )
   {
      WriteSector( NewSector,
                   ((char*)&(SecHead.Next) - (char*)&SecHead),   // Offset to Next field.
                   &NextSector,                   // Update Next field with new sector nbr.
                   sizeof(NextSector));
   }

   // Erase old fnode. Change status to FREE-DIRTY and then rewrite...
   SecHead.Status = FFS_SECTOR_HEADER_FREE_DIRTY;  // Mark this sector as free but needing erase.
   WriteSector( Sector, (char*)&SecHead.Version - (char*)&SecHead, &(SecHead.Version), 4);

   FFS_UNLOCK();

   return 0;
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::Space
//
//    Purpose:          Get information about used and free space or clear all space.
//
//    Inputs:           Option     - 0 - Return number of free bytes.
//                                   1 - Return the number of free Sectors.
//                                   2 - Return the total number of bytes in file system.
//                                   3 - Return the total number of Sectors in file system.
//                                   128 - Clear all space (used and unused).
//
//
//    Returns:          Total Free size in bytes.
//
//    Notes:            Currently, this feature assumes that all sectors are the same size.
//
//                      To perform this feature, we will allocate a new fnode sector,
//                      copy data to new sector, erase old file, save new fnode.
//
//
//---------------------------------------------------------------------------------------
int Jcffs::Space( int Option )
{
   FFS_SECTOR_HEADER     SecHead;
   FFS_FLASH_SECTION*    Section;
   unsigned long          Sector;
   unsigned long          RelSector;
   unsigned long          TotalSize = 0;


   if( initializationComplete == false )
   {
      Initialize();
   }

   FFS_LOCK();

   if( Option == 128 )
   {
      // Erase all sectors in flash file system...
      for( Sector = 0; GetFlashSectionEntry( Sector, &Section, &RelSector ); Sector++ )
      {
         EraseSector( Sector );
         TotalSize += (Section->SectorSize - sizeof(FFS_SECTOR_HEADER));
      }
   } else if( Option >= 0 && Option <= 3 )
   {
      // Go thru all sectors and tally space depending on option...
      for( Sector = 0; GetFlashSectionEntry( Sector, &Section, &RelSector ); Sector++ )
      {
         ReadSector(Sector, 0, &SecHead, sizeof(FFS_SECTOR_HEADER));

         if( Option == 2                        ||          // Tally all Bytes
             Option == 3                        ||          // Tally all
             SecHead.Status == FFS_SECTOR_HEADER_FREE ||
             SecHead.Status == FFS_SECTOR_HEADER_FREE_DIRTY )
         {
            if(Option == 0 || Option == 2)
            {
               TotalSize += (Section->SectorSize - sizeof(FFS_SECTOR_HEADER));
            }
            else
            {
               TotalSize += 1;   // Just count sectors.
            }

         }
      }
   }

   FFS_UNLOCK();

   return TotalSize;
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::Check
//
//    Purpose:          Check and fix file system.
//
//    Inputs:           None
//
//
//    Returns:          Total count of sectors fixed.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::Check( void )
{
   int                   TotalFixedSectors = 0;
   unsigned long         Sector;
   unsigned long         DeleteSector;
   unsigned long         NextSector;
   FFS_SECTOR_HEADER    SecHeader;
   FFS_FLASH_SECTION*   Section;
   FFS_FILE_NODE        Fnode;
   FFS_FILE_NODE        NextFnode;


   if( initializationComplete == false )
   {
      Initialize();
   }

   FFS_LOCK();

   TotalCrossChain = 0;
   ErrorSectorCount = 0;

   //---------------------------------------------------------------------------
   // Check for errors in the file system that we can clean up...
   // CheckForErrors();
   //---------------------------------------------------------------------------

   // First, count total number of sectors in file system...
   Section = &(FlashSectionTable[0]);

   // Go thru the table until we get to the end...
   for(TotalSectors = 0; Section->Device != 0xff; Section++)
   {
      TotalSectors += Section->Count;     // Tally count.
   }

   SectorArray = _mem_alloc_zero(TotalSectors);

   // Now look thru sectors for Fnodes.  When one is found, follow chain and mark
   // each sector array entry for each valid sector...
   for( Sector = 0; Sector < TotalSectors; Sector++ )
   {
      ReadSector( Sector, 0, &SecHeader, sizeof(FFS_SECTOR_HEADER) );

      if( SecHeader.Key != FFS_SECTOR_HEADER_KEY )
      {
         // Only mark it bad if status byte does not indicate FREE. If it says
         // free, then it can still be allocated later...
         if(SecHeader.Status != FFS_SECTOR_HEADER_FREE &&
            SecHeader.Status != FFS_SECTOR_HEADER_FREE_DIRTY )
         {
            SectorArray[Sector] |= CHECK_SECTOR_BAD;
         }
      }

      switch ( SecHeader.Status )
      {
         // Mark the free ones.
         case FFS_SECTOR_HEADER_FREE:
         case FFS_SECTOR_HEADER_FREE_DIRTY:
            SectorArray[Sector] |= CHECK_SECTOR_FREE;
            break;

         // Ordinary in-use sector. just ignore for now.
         case FFS_SECTOR_HEADER_INUSE:
            break;

         case FFS_SECTOR_HEADER_INUSE_FILENODE:
            // Read Fnode...
            ReadSector(  Sector,
                         sizeof(FFS_SECTOR_HEADER),
                         &Fnode,
                         sizeof(FFS_FILE_NODE) );
            // Check for valid Fnode...
            if( Fnode.FileSize == 0 || Fnode.FileSize == -1 )
            {
               // Mark this one as bad.  It needs to be cleaned up...
               SectorArray[Sector] |= CHECK_SECTOR_BAD;
            }
            else
            {
               // We have a sector with a file node - the start of a file.
               SectorArray[Sector] |= CHECK_SECTOR_FNODE;
               // Check chain of sectors for this file...
               NextSector = SecHeader.Next;
               while( NextSector != -1 )
               {
                  ReadSector( NextSector, 0, &SecHeader, sizeof(FFS_SECTOR_HEADER) );
                  if( (SectorArray[NextSector] & CHECK_SECTOR_FREE)  ||
                      (SectorArray[NextSector] & CHECK_SECTOR_FNODE) ||
                      (SectorArray[NextSector] & CHECK_SECTOR_BAD)   )
                  {
                     TotalCrossChain++;
                  }
                  SectorArray[NextSector] |= CHECK_SECTOR_INUSE;
                  NextSector = SecHeader.Next;
               }
            }
            break;

         // Anything else indicates something wrong.
         // Don't mark these bad quite yet...
         default:
            //(*SectorArray)[Sector] |= CHECK_SECTOR_BAD;
            break;

      }
   }

   // OK, now we have a table that will help us find sectors that have been left
   // estranged.  Go thru table and mark the sector FREE_DIRTY. If they are BAD, then
   // try to erase them....
   for( Sector = 0; Sector < TotalSectors; Sector++ )
   {
      if( !(SectorArray[Sector] & CHECK_SECTOR_INUSE) &&
          !(SectorArray[Sector] & CHECK_SECTOR_FNODE) &&
          !(SectorArray[Sector] & CHECK_SECTOR_FREE)  )
      {
         // If sector isn't bad...
         if( !(SectorArray[Sector] & CHECK_SECTOR_BAD) )
         {
            ReadSector( Sector, 0, &SecHeader, sizeof(FFS_SECTOR_HEADER) );
            SecHeader.Status = FFS_SECTOR_HEADER_FREE_DIRTY;
            WriteSector( Sector, 0, &SecHeader, sizeof(FFS_SECTOR_HEADER) );
            TotalFixedSectors++;
         }
         else
         {
            EraseSector( Sector );
            TotalFixedSectors++;
         }
      }
   }

   // Now, check for duplicate files.  Delete oldest one...
   for( Sector = 0; Sector < TotalSectors; Sector++ )
   {
      ReadSector( Sector, 0, &SecHeader, sizeof(FFS_SECTOR_HEADER) );

      if( SecHeader.Status == FFS_SECTOR_HEADER_INUSE_FILENODE )
      {
         // Read Fnode...
         ReadSector(  Sector,
                      sizeof(FFS_SECTOR_HEADER),
                      &Fnode,
                      sizeof(FFS_FILE_NODE) );

         // Now, go thru each following sector looking for an Fnode with matching
         // name.  If we find one, then check counter and delete file with lower count.
         for( NextSector = Sector + 1; NextSector < TotalSectors; NextSector++ )
         {
            ReadSector( NextSector, 0, &SecHeader, sizeof(FFS_SECTOR_HEADER) );

            if( SecHeader.Status == FFS_SECTOR_HEADER_INUSE_FILENODE )
            {
               // Read Fnode...
               ReadSector(  NextSector,
                            sizeof(FFS_SECTOR_HEADER),
                            &NextFnode,
                            sizeof(FFS_FILE_NODE) );

               // Uppercase names for compare so compare is case-insensitive...
               StringToUpperCase(Fnode.Filename);
               StringToUpperCase(NextFnode.Filename);

               // See if files match.  If they do, delete oldest one...
               if( strcmp(Fnode.Filename, NextFnode.Filename) == 0 )
               {
                  if(Fnode.Count < NextFnode.Count)
                  {
                     DeleteSector = Sector;
                  }
                  else
                  {
                     DeleteSector = NextSector;
                  }

                  while (DeleteSector != -1 )
                  {
                     // All of sector header...
                     ReadSector( DeleteSector, 0, &SecHeader, sizeof(FFS_SECTOR_HEADER) );

                     // Save number of next sector in chain...
                     NextSector = SecHeader.Next;

                     // Change status to FREE...
                     SecHeader.Status = FFS_SECTOR_HEADER_FREE_DIRTY;  // Mark this sector as free but needing erase.

                     // Rewrite portion of sector header that has Status in it..,
                     WriteSector( DeleteSector, (char*)&SecHeader.Version - (char*)&SecHeader, &(SecHeader.Version), 4);
                     TotalFixedSectors++;

                     DeleteSector = NextSector;                // Next sector is now current sector.
                  }

                  if(Fnode.Count < NextFnode.Count)
                  {
                     // We don't need to check any more for this sector.
                     break;
                  }
               }
            }
         }
      }
   }

   FFS_UNLOCK();

   return TotalFixedSectors;
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::LocatePosition
//
//    Purpose:          Locate the current position of a file.
//
//    Inputs:           Fdesc    - Pointer to file descriptor.
//                      Position - Position of fle to locate.
//
//    Outputs:          Sector   - Returned sector number where position is located.
//                      SecHead  - Returned sector header from that sector.
//                      Offset   - Returned offset into sector where position is at.
//
//    Returns:          Either 0 or Jcffs error code.
//
//    Notes:
//                      We locate where the current file position is. That is, we locate
//                      in what sector and at what offset into that sector. And, we return
//                      the sector number of that sector, and the offset into that sector.
//                      Also, the sector header is returned, which contains the size of
//                      the sector.
//
//---------------------------------------------------------------------------------------
int Jcffs::LocatePosition( FFS_FILE_DESCRIPTOR* Fdesc,
                          unsigned long         Position,
                          unsigned long*        Sector,
                          FFS_SECTOR_HEADER*   SecHead,
                          unsigned long*        Offset )
{
    FFS_FILE_NODE*        Fnode;                 // Ptr to In-core fnode in file desc entry.
    int                    rc;
    unsigned long          Count = 0;             // As we're reading sectors, keep count.

    Fnode = &(Fdesc->Fnode);                      // Copy ptr for convenience.

    *Sector = Fdesc->FnodeSector;                 // Start with first sector.

    while(1)
    {
        // Read sector header from this sector. If error, return with that error...
        if( (rc = ReadSector( *Sector, 0, SecHead, sizeof(FFS_SECTOR_HEADER) )) < 0)
        {
            return rc;
        }

        // Is position within this sector?
        if (Position < (Count + (SecHead->SectorLength - SecHead->DataOffset)))
        {
            // Position is within this sector. Calculate Offset...
            *Offset = SecHead->DataOffset + (Position - Count);
            break;
        }

        Count += (SecHead->SectorLength - SecHead->DataOffset);    // Calc running total.

        *Sector = SecHead->Next;                  // Get next sector number.
    }

    return 0;
}





//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::LocateFileNode
//
//    Purpose:          Locate the file in the file system.  If it is found, then
//                      copy filenode to caller's place and return sector where
//                      filenode is, which is also the start of the file.
//
//                      Locate the file by going thru every sector and if it is a sector
//                      with a valid fnode, check the name.
//
//    Inputs:           Filename - the name of the file to locate.
//
//    Outputs:          Fnode  - Where to copy the filenode.
//                      Sector - The sector number where the filenode lives.
//
//    Returns:          0=Found, 1=Not Found, or Jcffs error code.
//
//    Notes:            At some point we could cache the names in a local directory array.
//
//---------------------------------------------------------------------------------------
int Jcffs::LocateFileNode(char* Filename, FFS_FILE_NODE* RtnFnode, unsigned long* RtnSector)
{
    unsigned long         Sector;
    FFS_SECTOR_HEADER    SecHead;
    FFS_FILE_NODE        Fnode;
    char                  CompName[FFS_MAX_FILENAME_LENGTH + 1];
    char                  FnodeName[FFS_MAX_FILENAME_LENGTH + 1];

    Sector = 0;                                   // Start with the first sector.

    strcpy(CompName, Filename);                   // Copy Filename so we can compare case-insensitive.
    StringToUpperCase(CompName);

    while( ValidSector( Sector ) )
    {
        // Read sector header...
        ReadSector( Sector, 0, &SecHead, sizeof(FFS_SECTOR_HEADER) );

        // Does this sector have an fnode?
        if (SecHead.Status == FFS_SECTOR_HEADER_INUSE_FILENODE )
        {
            // Read File Node, which contains filename...
            ReadSector( Sector, sizeof(FFS_SECTOR_HEADER), &Fnode, sizeof(FFS_FILE_NODE) );

            // Copy and uppercase name from fnode so we can compare case-insensitive...
            strcpy(FnodeName, Fnode.Filename);
            StringToUpperCase(FnodeName);

            // Now see if filenames match...
            if( strcmp(FnodeName, CompName) == 0 )
            {
                // Filenames match, so return fnode and sector...
                memcpy(RtnFnode, &Fnode, sizeof(FFS_FILE_NODE));
                *RtnSector = Sector;
                return 1;
            }
        }

        Sector += 1;              // Next sequencial sector number.

    }

    // Could not find file...
    *RtnSector = -1;
    return 0;
}





//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::AllocateSector
//
//    Purpose:          Allocate a sector that wasn't being used.
//
//    Inputs:           None.
//
//    Outputs:          NewSector - Sector number of newly allocated sector.
//                      SecHeader - A copy of new sector header.
//
//    Returns:          0 or an Jcffs error code.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::AllocateSector( unsigned long* NewSector, FFS_SECTOR_HEADER* SecHeader )
{
   FFS_FLASH_SECTION*   Section;


   // Find a free sector. If we found one, clean it and update header...
   if( FindFreeSector( NewSector, SecHeader, &Section ) )
   {
      // We've already read his sector header, so update the sector header so
      // that we can rewrite it after we erase the sector...
      SecHeader->Key            = FFS_SECTOR_HEADER_KEY;
      SecHeader->Next           = -1;
      SecHeader->EraseCount++;
      SecHeader->Version        = FFS_FILE_SYSTEM_VERSION;
      SecHeader->Status         = FFS_SECTOR_HEADER_INUSE;
      SecHeader->SectorChecksum = 0xffff;
      SecHeader->SectorLength   = Section->SectorSize;
      SecHeader->DataOffset     = sizeof(FFS_SECTOR_HEADER);

      EraseSector( *NewSector );

      // Now, rewrite sector header back out...
      WriteSector( *NewSector, 0, SecHeader, sizeof(FFS_SECTOR_HEADER));

      return 0;
   }

   return FFS_RC_OUT_OF_SPACE;
}




//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::AllocateSectorWithFilenode
//
//    Purpose:          Allocate a sector that wasn't being used. Leave space for
//                      file node.
//
//    Inputs:           None.
//
//    Outputs:          NewSector - Sector number of newly allocated sector.
//                      SecHeader - A copy of new sector header.
//
//    Returns:          0 or an Jcffs error code.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::AllocateSectorWithFilenode( unsigned long* NewSector, FFS_SECTOR_HEADER* SecHeader )
{
   FFS_FLASH_SECTION*   Section;


   // Find a free sector. If we found one, clean it and update header...
   if( FindFreeSector( NewSector, SecHeader, &Section ) )
   {
      // We've already read his sector header, so update the sector header so
      // that we can rewrite it after we erase the sector...
      SecHeader->Key            = FFS_SECTOR_HEADER_KEY;
      SecHeader->Next           = -1;
      SecHeader->EraseCount++;
      SecHeader->Version        = FFS_FILE_SYSTEM_VERSION;
      SecHeader->Status         = FFS_SECTOR_HEADER_INUSE_FILENODE;
      SecHeader->SectorChecksum = 0xffff;
      SecHeader->SectorLength   = Section->SectorSize;
      SecHeader->DataOffset     = sizeof(FFS_SECTOR_HEADER) + sizeof(FFS_FILE_NODE);

      EraseSector( *NewSector );

      // Now, rewrite sector header back out...
      WriteSector( *NewSector, 0, SecHeader, sizeof(FFS_SECTOR_HEADER));

      return 0;
   }

   return FFS_RC_OUT_OF_SPACE;
}





//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::FindFreeSector
//
//    Purpose:          Find a sector that isn't being used.
//
//    Inputs:           None.
//
//    Outputs:          NewSector - Return a sector number of a free sector.
//                      SecHeader - Return a copy of current sector header.
//                      Section   - Return a pointer to a pointer to the Section
//                                  table entry.
//
//    Returns:          1 if one is found.
//                      0 if none is found.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::FindFreeSector( unsigned long*       Sector,
                          FFS_SECTOR_HEADER*  SecHeader,
                          FFS_FLASH_SECTION** Section )
{
   unsigned long         ErrorCount = 0;
   unsigned long         RelSector;


   // Find a free sector by sequencially going thru sectors.  Some time later we could
   // implement a round-robin or balancing algorithm...
   for( *Sector = 0; GetFlashSectionEntry( *Sector, Section, &RelSector ); (*Sector)++ )
   {
      ReadSector( *Sector, 0, SecHeader, sizeof(FFS_SECTOR_HEADER) );

      // First check to see if sector header looks valid...
      if( SecHeader->Key == FFS_SECTOR_HEADER_KEY )
      {
         if( SecHeader->Status == FFS_SECTOR_HEADER_FREE ||
             SecHeader->Status == FFS_SECTOR_HEADER_FREE_DIRTY  )
         {
            // Return: we found a free sector...
            return 1;
         }
      }
      else
      {
#if 1
         // Return: Assume that this sector has just never been used before. We'll tally
         // it as an error, but assume it's free and return it to caller...
         ErrorCount++;
         if(ErrorCount > ErrorSectorCount)
         {
            ErrorSectorCount = ErrorCount;
         }
         return 1;
#else

         // Sector header does not look valid. The first time that the Jcffs system
         // was started, we went thru and erased all sectors and put in new headers,
         // so this should not happen.  This is probably an error that we should
         // deal with at some time, but for now, just keep count of bad ones and
         // ignore it for now.
         ErrorCount++;
         if(ErrorCount > ErrorSectorCount)
         {
            ErrorSectorCount = ErrorCount;
         }
#endif
      }
   }

   return 0;
}



//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::FreeSectors
//
//    Purpose:          Free a list of sectors (because we're deleting a file).
//
//    Inputs:           Sector - Starting sector to delete.
//
//    Returns:          0 or Jcffs error code.
//
//    Notes:            The process of freeing a sector involves changing the Status
//                      field to 00.  This is one byte, so we actually read/write
//                      4 bytes around it.  The changing of Status to 00 is reasonable
//                      since we only support NOR flash and because when we write NOR
//                      flash, we can only change ones to zeros. Only the erase sector
//                      function can change zeros back to ones in a sector.
//
//---------------------------------------------------------------------------------------
int Jcffs::FreeSectors( unsigned long Sector )
{
   FFS_SECTOR_HEADER  SecHead;
   unsigned long       NextSector;

   while (Sector != -1 )
   {
      // All of sector header...
      ReadSector( Sector, 0, &SecHead, sizeof(FFS_SECTOR_HEADER) );

      // Save number of next sector in chain...
      NextSector = SecHead.Next;

      // Change status to FREE...
      SecHead.Status = FFS_SECTOR_HEADER_FREE_DIRTY;  // Mark this sector as free but needing erase.

      // Rewrite portion of sector header that has Status in it..,
      WriteSector( Sector, (char*)&SecHead.Version - (char*)&SecHead, &(SecHead.Version), 4);

      Sector = NextSector;                         // Next sector is now current sector.
   }

   return 0;
}




//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::ReadSector
//
//    Purpose:          Read a portion of a sector.
//
//    Inputs:           Sector - Sector number to read from.
//                      Offset - Offset into sector to start reading from.
//                      Buffer - Caller's buffer to copy the read data into.
//                      Length - Length of how much to read from sector.
//
//    Returns:          0 > the length of data read or an Jcffs error code.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::ReadSector(  unsigned long  Sector,
                       unsigned long  Offset,
                       unsigned char* Buffer,
                       int            Length)
{
   FFS_FLASH_SECTION*   Section;
   unsigned long         RelSector;

   // Locate which section this sector is in...
   if( GetFlashSectionEntry( Sector, &Section, &RelSector ) == 0)
   {
      return FFS_RC_INVALID_SECTOR_NUMBER;
   }

   return Section->Read( Section, RelSector, Offset, Buffer, Length );
}



//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::WriteSector
//
//    Purpose:          Write a portion of a sector.
//
//    Inputs:           Sector - Sector number to write to.
//                      Offset - Offset into sector to start writing to.
//                      Buffer - Caller's buffer of data to write.
//                      Length - Length of how much to write to sector.
//
//    Returns:          0 > the length of data written or an Jcffs error code.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::WriteSector( unsigned long  Sector,
                       unsigned long  Offset,
                       unsigned char* Buffer,
                       int            Length )
{
   FFS_FLASH_SECTION*   Section;
   unsigned long         RelSector;

   // Locate which section this sector is in...
   if( GetFlashSectionEntry( Sector, &Section, &RelSector ) == 0)
   {
      return FFS_RC_INVALID_SECTOR_NUMBER;
   }

   return Section->Write( Section, RelSector, Offset, Buffer, Length );
}



//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::EraseSector
//
//    Purpose:          Erase a complete sector.
//
//    Inputs:           Sector - Sector number to erase.
//
//    Returns:          0 or an Jcffs error code.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::EraseSector( unsigned long Sector )
{
   FFS_FLASH_SECTION*   Section;
   unsigned long         RelSector;

   // Locate which section this sector is in...
   if( GetFlashSectionEntry( Sector, &Section, &RelSector ) == 0)
   {
      return FFS_RC_INVALID_SECTOR_NUMBER;
   }

   return Section->Erase( Section, RelSector );
}





//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::ValidSector
//
//    Purpose:          Check to see if sector number is within our file system.
//
//    Inputs:           Sector - Sector number to check to see if valid.
//
//    Returns:          True if within one of the defined sections of flash devices, or
//                      False if not.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::ValidSector( unsigned long Sector )
{
    FFS_FLASH_SECTION    Section;
    unsigned long         RelSector;

    return  GetFlashSectionEntry( Sector, &Section, &RelSector );
}





//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::GetFlashSectionEntry
//
//    Purpose:          Locate which flash section a particular sector is in and return
//                      the FFS_FLASH_SECTION table entry that represents the area
//                      the sector is in.
//
//    Inputs:           Sector - Sector number to locate.
//
//    Outputs:          Section - A pointer to where to return the section table entry ptr.
//                      RelSector - Sector nbr relative to start of a flash device section.
//
//    Returns:          True if within one of the defined sections of flash devices, or
//                      False if not.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::GetFlashSectionEntry(  unsigned long        Sector,
                                 FFS_FLASH_SECTION** Section,
                                 unsigned long*       RelSector )
{
    *Section = &(FlashSectionTable[0]);

    // Go thru the table until we get to the end...
    while ((*Section)->Device != 0xff)
    {
        if (Sector < (*Section)->Count )
        {
           *RelSector = Sector;
            return 1;
        }

        // bump up to next section...
        (*Section)++;
        Sector -= (*Section)->Count;
    }

    return 0;                       // not found/not valid.
}




//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::GetDescriptor
//
//    Purpose:          Allocate a descriptor table entry.
//
//    Inputs:           None.
//
//    Returns:          Either a desriptor number (index into table), or an
//                      Jcffs error code.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::GetDescriptor( void )
{
   int   fd;

   for( fd = 0; fd < FFS_MAX_FILE_DESCRIPTORS; fd++ )
   {
      // Is this one free?
      if( !FileDescriptors[fd].InUse )
      {
         // Clean up this entry...
         memset( (void*)&(FileDescriptors[fd]), 0, sizeof(FFS_FILE_DESCRIPTOR) );

         // Mark this one in use...
         FileDescriptors[fd].InUse = 1;

         // return this descriptor.
         return fd;
      }
   }

   return FFS_RC_TOO_MANY_OPEN_FILES;
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::FreeDescriptor
//
//    Purpose:          Free a descriptor table entry.
//
//    Inputs:           None.
//
//    Returns:          Either a desriptor number (index into table), or an
//                      Jcffs error code.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
int Jcffs::FreeDescriptor( int fd )
{
   FileDescriptors[fd].InUse = 0;            // Clear IN USE flag.

   return 0;
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::StringToUpperCase
//
//    Purpose:          Copy a string and uppercase it.
//
//    Inputs:           Ptr to string to uppercase.
//
//    Returns:          None.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
void Jcffs::StringToUpperCase( char* str )
{
   int i;

   for(i = strlen(str); i > 0 ; i--)
   {
      str[i-1] = toupper(str[i-1]);
   }
}


//---------------------------------------------------------------------------------------
//
//    Function Name:    Jcffs::Initialize
//
//    Purpose:          Do any necessary initialization.
//
//    Inputs:           None.
//
//    Returns:          Nothing.
//
//    Notes:
//
//---------------------------------------------------------------------------------------
void Jcffs::Initialize( void )
{
   if( initializationComplete == false )
   {
      FFS_INITLOCK();
      initializationComplete = true;
   }

}



//---------------------------------------------------------------------------------------
//    C wrappers for file operations...
//---------------------------------------------------------------------------------------
extern "C" int  Jcffs_open( char* Filename, int flags, int permissions )
{
   if( myffsObj )
   {
      return myffsObj->open( Filename, flags, permissions );
   }
   else
   {
      return -1;
   }
}

extern "C" int Jcffs_close( int fd )
{
   if( myffsObj )
   {
      return myffsObj->close( fd );
   }
   else
   {
      return -1;
   }
}

extern "C" int Jcffs_read( int fd, char* buf, int n )
{
   if( myffsObj )
   {
      return myffsObj->read( fd, buf, n );
   }
   else
   {
      return -1;
   }
}

extern "C" int Jcffs_write( int fd, char* buf, int n  )
{
   if( myffsObj )
   {
      return myffsObj->write( fd, buf, n  );
   }
   else
   {
      return -1;
   }
}

extern "C" int Jcffs_NextDirectory( unsigned long* Handle, FFS_FILE_NODE* Fnode )
{
   if( myffsObj )
   {
      return myffsObj->NextDirectory( Handle, Fnode );
   }
   else
   {
      return -1;
   }
}

extern "C" int Jcffs_Erase(  char* filename  )
{
   if( myffsObj )
   {
      return myffsObj->Erase(  filename  );
   }
   else
   {
      return -1;
   }
}

extern "C" int Jcffs_Rename( char* filename, char* new_filename )
{
   if( myffsObj )
   {
      return myffsObj->Rename( filename, new_filename );
   }
   else
   {
      return -1;
   }
}

extern "C" int Jcffs_Space(  int Option )
{
   if( myffsObj )
   {
      return myffsObj->Space( Option );
   }
   else
   {
      return -1;
   }
}

extern "C" int Jcffs_Check( void )
{
   if( myffsObj )
   {
      return myffsObj->Check();
   }
   else
   {
      return -1;
   }
}

