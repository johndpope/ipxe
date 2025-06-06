/*
 * Copyright (C) 2006 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * You can also choose to distribute this program under the terms of
 * the Unmodified Binary Distribution Licence (as given in the file
 * COPYING.UBDL), provided that you have satisfied its requirements.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <byteswap.h>
#include <errno.h>
#include <assert.h>
#include <ipxe/blockdev.h>
#include <ipxe/io.h>
#include <ipxe/acpi.h>
#include <ipxe/sanboot.h>
#include <ipxe/device.h>
#include <ipxe/pci.h>
#include <ipxe/eltorito.h>
#include <realmode.h>
#include <bios.h>
#include <biosint.h>
#include <bootsector.h>
#include <int13.h>

/** @file
 *
 * INT 13 emulation
 *
 * This module provides a mechanism for exporting block devices via
 * the BIOS INT 13 disk interrupt interface.  
 *
 */

/** INT 13 SAN device private data */
struct int13_data {
	/** BIOS natural drive number (0x00-0xff)
	 *
	 * This is the drive number that would have been assigned by
	 * 'naturally' appending the drive to the end of the BIOS
	 * drive list.
	 *
	 * If the emulated drive replaces a preexisting drive, this is
	 * the drive number that the preexisting drive gets remapped
	 * to.
	 */
	unsigned int natural_drive;

	/** Number of cylinders
	 *
	 * The cylinder number field in an INT 13 call is ten bits
	 * wide, giving a maximum of 1024 cylinders.  Conventionally,
	 * when the 7.8GB limit of a CHS address is exceeded, it is
	 * the number of cylinders that is increased beyond the
	 * addressable limit.
	 */
	unsigned int cylinders;
	/** Number of heads
	 *
	 * The head number field in an INT 13 call is eight bits wide,
	 * giving a maximum of 256 heads.  However, apparently all
	 * versions of MS-DOS up to and including Win95 fail with 256
	 * heads, so the maximum encountered in practice is 255.
	 */
	unsigned int heads;
	/** Number of sectors per track
	 *
	 * The sector number field in an INT 13 call is six bits wide,
	 * giving a maximum of 63 sectors, since sector numbering
	 * (unlike head and cylinder numbering) starts at 1, not 0.
	 */
	unsigned int sectors_per_track;

	/** Address of El Torito boot catalog (if any) */
	unsigned int boot_catalog;
	/** Status of last operation */
	int last_status;
};

/** Vector for chaining to other INT 13 handlers */
static struct segoff __text16 ( int13_vector );
#define int13_vector __use_text16 ( int13_vector )

/** Assembly wrapper */
extern void int13_wrapper ( void );

/** Dummy floppy disk parameter table */
static struct int13_fdd_parameters __data16 ( int13_fdd_params ) = {
	/* 512 bytes per sector */
	.bytes_per_sector = 0x02,
	/* Highest sectors per track that we ever return */
	.sectors_per_track = 48,
};
#define int13_fdd_params __use_data16 ( int13_fdd_params )

/**
 * Equipment word
 *
 * This is a cached copy of the BIOS Data Area equipment word at
 * 40:10.
 */
static uint16_t equipment_word;

/**
 * Number of BIOS floppy disk drives
 *
 * This is derived from the equipment word.  It is held in .text16 to
 * allow for easy access by the INT 13,08 wrapper.
 */
static uint8_t __text16 ( num_fdds );
#define num_fdds __use_text16 ( num_fdds )

/**
 * Number of BIOS hard disk drives
 *
 * This is a cached copy of the BIOS Data Area number of hard disk
 * drives at 40:75.  It is held in .text16 to allow for easy access by
 * the INT 13,08 wrapper.
 */
static uint8_t __text16 ( num_drives );
#define num_drives __use_text16 ( num_drives )

/**
 * Calculate SAN device capacity (limited to 32 bits)
 *
 * @v sandev		SAN device
 * @ret blocks		Number of blocks
 */
static inline uint32_t int13_capacity32 ( struct san_device *sandev ) {
	uint64_t capacity = sandev_capacity ( sandev );
	return ( ( capacity <= 0xffffffffUL ) ? capacity : 0xffffffff );
}

/**
 * Test if SAN device is a floppy disk drive
 *
 * @v sandev		SAN device
 * @ret is_fdd		SAN device is a floppy disk drive
 */
static inline int int13_is_fdd ( struct san_device *sandev ) {
	return ( ! ( sandev->drive & 0x80 ) );
}

/**
 * Parse El Torito parameters
 *
 * @v sandev		SAN device
 * @v scratch		Scratch area for single-sector reads
 * @ret rc		Return status code
 *
 * Reads and parses El Torito parameters, if present.
 */
static int int13_parse_eltorito ( struct san_device *sandev, void *scratch ) {
	struct int13_data *int13 = sandev->priv;
	static const struct eltorito_descriptor_fixed boot_check = {
		.type = ISO9660_TYPE_BOOT,
		.id = ISO9660_ID,
		.version = 1,
		.system_id = "EL TORITO SPECIFICATION",
	};
	struct eltorito_descriptor *boot = scratch;
	int rc;

	/* Read boot record volume descriptor */
	if ( ( rc = sandev_read ( sandev, ELTORITO_LBA, 1, boot ) ) != 0 ) {
		DBGC ( sandev->drive, "INT13 drive %02x could not read El "
		       "Torito boot record volume descriptor: %s\n",
		       sandev->drive, strerror ( rc ) );
		return rc;
	}

	/* Check for an El Torito boot catalog */
	if ( memcmp ( boot, &boot_check, sizeof ( boot_check ) ) == 0 ) {
		int13->boot_catalog = boot->sector;
		DBGC ( sandev->drive, "INT13 drive %02x has an El Torito boot "
		       "catalog at LBA %08x\n", sandev->drive,
		       int13->boot_catalog );
	} else {
		DBGC ( sandev->drive, "INT13 drive %02x has no El Torito boot "
		       "catalog\n", sandev->drive );
	}

	return 0;
}

/**
 * Guess INT 13 hard disk drive geometry
 *
 * @v sandev		SAN device
 * @v scratch		Scratch area for single-sector reads
 * @ret heads		Guessed number of heads
 * @ret sectors		Guessed number of sectors per track
 * @ret rc		Return status code
 *
 * Guesses the drive geometry by inspecting the partition table.
 */
static int int13_guess_geometry_hdd ( struct san_device *sandev, void *scratch,
				      unsigned int *heads,
				      unsigned int *sectors ) {
	struct master_boot_record *mbr = scratch;
	struct partition_table_entry *partition;
	unsigned int i;
	unsigned int start_cylinder;
	unsigned int start_head;
	unsigned int start_sector;
	unsigned int end_head;
	unsigned int end_sector;
	int rc;

	/* Read partition table */
	if ( ( rc = sandev_read ( sandev, 0, 1, mbr ) ) != 0 ) {
		DBGC ( sandev->drive, "INT13 drive %02x could not read "
		       "partition table to guess geometry: %s\n",
		       sandev->drive, strerror ( rc ) );
		return rc;
	}
	DBGC2 ( sandev->drive, "INT13 drive %02x has MBR:\n", sandev->drive );
	DBGC2_HDA ( sandev->drive, 0, mbr, sizeof ( *mbr ) );
	DBGC ( sandev->drive, "INT13 drive %02x has signature %08x\n",
	       sandev->drive, mbr->signature );

	/* Scan through partition table and modify guesses for
	 * heads and sectors_per_track if we find any used
	 * partitions.
	 */
	*heads = 0;
	*sectors = 0;
	for ( i = 0 ; i < 4 ; i++ ) {

		/* Skip empty partitions */
		partition = &mbr->partitions[i];
		if ( ! partition->type )
			continue;

		/* If partition starts on cylinder 0 then we can
		 * unambiguously determine the number of sectors.
		 */
		start_cylinder = PART_CYLINDER ( partition->chs_start );
		start_head = PART_HEAD ( partition->chs_start );
		start_sector = PART_SECTOR ( partition->chs_start );
		if ( ( start_cylinder == 0 ) && ( start_head != 0 ) ) {
			*sectors = ( ( partition->start + 1 - start_sector ) /
				     start_head );
			DBGC ( sandev->drive, "INT13 drive %02x guessing "
			       "C/H/S xx/xx/%d based on partition %d\n",
			       sandev->drive, *sectors, ( i + 1 ) );
		}

		/* If partition ends on a higher head or sector number
		 * than our current guess, then increase the guess.
		 */
		end_head = PART_HEAD ( partition->chs_end );
		end_sector = PART_SECTOR ( partition->chs_end );
		if ( ( end_head + 1 ) > *heads ) {
			*heads = ( end_head + 1 );
			DBGC ( sandev->drive, "INT13 drive %02x guessing "
			       "C/H/S xx/%d/xx based on partition %d\n",
			       sandev->drive, *heads, ( i + 1 ) );
		}
		if ( end_sector > *sectors ) {
			*sectors = end_sector;
			DBGC ( sandev->drive, "INT13 drive %02x guessing "
			       "C/H/S xx/xx/%d based on partition %d\n",
			       sandev->drive, *sectors, ( i + 1 ) );
		}
	}

	/* Default guess is xx/255/63 */
	if ( ! *heads )
		*heads = 255;
	if ( ! *sectors )
		*sectors = 63;

	return 0;
}

/** Recognised floppy disk geometries */
static const struct int13_fdd_geometry int13_fdd_geometries[] = {
	INT13_FDD_GEOMETRY ( 40, 1, 8 ),
	INT13_FDD_GEOMETRY ( 40, 1, 9 ),
	INT13_FDD_GEOMETRY ( 40, 2, 8 ),
	INT13_FDD_GEOMETRY ( 40, 1, 9 ),
	INT13_FDD_GEOMETRY ( 80, 2, 8 ),
	INT13_FDD_GEOMETRY ( 80, 2, 9 ),
	INT13_FDD_GEOMETRY ( 80, 2, 15 ),
	INT13_FDD_GEOMETRY ( 80, 2, 18 ),
	INT13_FDD_GEOMETRY ( 80, 2, 20 ),
	INT13_FDD_GEOMETRY ( 80, 2, 21 ),
	INT13_FDD_GEOMETRY ( 82, 2, 21 ),
	INT13_FDD_GEOMETRY ( 83, 2, 21 ),
	INT13_FDD_GEOMETRY ( 80, 2, 22 ),
	INT13_FDD_GEOMETRY ( 80, 2, 23 ),
	INT13_FDD_GEOMETRY ( 80, 2, 24 ),
	INT13_FDD_GEOMETRY ( 80, 2, 36 ),
	INT13_FDD_GEOMETRY ( 80, 2, 39 ),
	INT13_FDD_GEOMETRY ( 80, 2, 40 ),
	INT13_FDD_GEOMETRY ( 80, 2, 44 ),
	INT13_FDD_GEOMETRY ( 80, 2, 48 ),
};

/**
 * Guess INT 13 floppy disk drive geometry
 *
 * @v sandev		SAN device
 * @ret heads		Guessed number of heads
 * @ret sectors		Guessed number of sectors per track
 * @ret rc		Return status code
 *
 * Guesses the drive geometry by inspecting the disk size.
 */
static int int13_guess_geometry_fdd ( struct san_device *sandev,
				      unsigned int *heads,
				      unsigned int *sectors ) {
	unsigned int blocks = sandev_capacity ( sandev );
	const struct int13_fdd_geometry *geometry;
	unsigned int cylinders;
	unsigned int i;

	/* Look for a match against a known geometry */
	for ( i = 0 ; i < ( sizeof ( int13_fdd_geometries ) /
			    sizeof ( int13_fdd_geometries[0] ) ) ; i++ ) {
		geometry = &int13_fdd_geometries[i];
		cylinders = INT13_FDD_CYLINDERS ( geometry );
		*heads = INT13_FDD_HEADS ( geometry );
		*sectors = INT13_FDD_SECTORS ( geometry );
		if ( ( cylinders * (*heads) * (*sectors) ) == blocks ) {
			DBGC ( sandev->drive, "INT13 drive %02x guessing "
			       "C/H/S %d/%d/%d based on size %dK\n",
			       sandev->drive, cylinders, *heads, *sectors,
			       ( blocks / 2 ) );
			return 0;
		}
	}

	/* Otherwise, assume a partial disk image in the most common
	 * format (1440K, 80/2/18).
	 */
	*heads = 2;
	*sectors = 18;
	DBGC ( sandev->drive, "INT13 drive %02x guessing C/H/S xx/%d/%d "
	       "based on size %dK\n", sandev->drive, *heads, *sectors,
	       ( blocks / 2 ) );
	return 0;
}

/**
 * Guess INT 13 drive geometry
 *
 * @v sandev		SAN device
 * @v scratch		Scratch area for single-sector reads
 * @ret rc		Return status code
 */
static int int13_guess_geometry ( struct san_device *sandev, void *scratch ) {
	struct int13_data *int13 = sandev->priv;
	unsigned int guessed_heads;
	unsigned int guessed_sectors;
	unsigned int blocks;
	unsigned int blocks_per_cyl;
	int rc;

	/* Guess geometry according to drive type */
	if ( int13_is_fdd ( sandev ) ) {
		if ( ( rc = int13_guess_geometry_fdd ( sandev, &guessed_heads,
						       &guessed_sectors )) != 0)
			return rc;
	} else {
		if ( ( rc = int13_guess_geometry_hdd ( sandev, scratch,
						       &guessed_heads,
						       &guessed_sectors )) != 0)
			return rc;
	}

	/* Apply guesses if no geometry already specified */
	if ( ! int13->heads )
		int13->heads = guessed_heads;
	if ( ! int13->sectors_per_track )
		int13->sectors_per_track = guessed_sectors;
	if ( ! int13->cylinders ) {
		/* Avoid attempting a 64-bit divide on a 32-bit system */
		blocks = int13_capacity32 ( sandev );
		blocks_per_cyl = ( int13->heads * int13->sectors_per_track );
		assert ( blocks_per_cyl != 0 );
		int13->cylinders = ( blocks / blocks_per_cyl );
		if ( int13->cylinders > 1024 )
			int13->cylinders = 1024;
	}

	return 0;
}

/**
 * Update BIOS drive count
 */
static void int13_sync_num_drives ( void ) {
	struct san_device *sandev;
	struct int13_data *int13;
	uint8_t *counter;
	uint8_t max_drive;
	uint8_t required;

	/* Get current drive counts */
	get_real ( equipment_word, BDA_SEG, BDA_EQUIPMENT_WORD );
	get_real ( num_drives, BDA_SEG, BDA_NUM_DRIVES );
	num_fdds = ( ( equipment_word & 0x0001 ) ?
		     ( ( ( equipment_word >> 6 ) & 0x3 ) + 1 ) : 0 );

	/* Ensure count is large enough to cover all of our SAN devices */
	for_each_sandev ( sandev ) {
		int13 = sandev->priv;
		counter = ( int13_is_fdd ( sandev ) ? &num_fdds : &num_drives );
		max_drive = sandev->drive;
		if ( max_drive < int13->natural_drive )
			max_drive = int13->natural_drive;
		required = ( ( max_drive & 0x7f ) + 1 );
		if ( *counter < required ) {
			*counter = required;
			DBGC ( sandev->drive, "INT13 drive %02x added to "
			       "drive count: %d HDDs, %d FDDs\n",
			       sandev->drive, num_drives, num_fdds );
		}
	}

	/* Update current drive count */
	equipment_word &= ~( ( 0x3 << 6 ) | 0x0001 );
	if ( num_fdds ) {
		equipment_word |= ( 0x0001 |
				    ( ( ( num_fdds - 1 ) & 0x3 ) << 6 ) );
	}
	put_real ( equipment_word, BDA_SEG, BDA_EQUIPMENT_WORD );
	put_real ( num_drives, BDA_SEG, BDA_NUM_DRIVES );
}

/**
 * Check number of drives
 */
static void int13_check_num_drives ( void ) {
	uint16_t check_equipment_word;
	uint8_t check_num_drives;

	get_real ( check_equipment_word, BDA_SEG, BDA_EQUIPMENT_WORD );
	get_real ( check_num_drives, BDA_SEG, BDA_NUM_DRIVES );
	if ( ( check_equipment_word != equipment_word ) ||
	     ( check_num_drives != num_drives ) ) {
		int13_sync_num_drives();
	}
}

/**
 * INT 13, 00 - Reset disk system
 *
 * @v sandev		SAN device
 * @ret status		Status code
 */
static int int13_reset ( struct san_device *sandev,
			 struct i386_all_regs *ix86 __unused ) {
	int rc;

	DBGC2 ( sandev->drive, "Reset drive\n" );

	/* Reset SAN device */
	if ( ( rc = sandev_reset ( sandev ) ) != 0 )
		return -INT13_STATUS_RESET_FAILED;

	return 0;
}

/**
 * INT 13, 01 - Get status of last operation
 *
 * @v sandev		SAN device
 * @ret status		Status code
 */
static int int13_get_last_status ( struct san_device *sandev,
				   struct i386_all_regs *ix86 __unused ) {
	struct int13_data *int13 = sandev->priv;

	DBGC2 ( sandev->drive, "Get status of last operation\n" );
	return int13->last_status;
}

/**
 * Read / write sectors
 *
 * @v sandev		SAN device
 * @v al		Number of sectors to read or write (must be nonzero)
 * @v ch		Low bits of cylinder number
 * @v cl (bits 7:6)	High bits of cylinder number
 * @v cl (bits 5:0)	Sector number
 * @v dh		Head number
 * @v es:bx		Data buffer
 * @v sandev_rw		SAN device read/write method
 * @ret status		Status code
 * @ret al		Number of sectors read or written
 */
static int int13_rw_sectors ( struct san_device *sandev,
			      struct i386_all_regs *ix86,
			      int ( * sandev_rw ) ( struct san_device *sandev,
						    uint64_t lba,
						    unsigned int count,
						    void *buffer ) ) {
	struct int13_data *int13 = sandev->priv;
	unsigned int cylinder, head, sector;
	unsigned long lba;
	unsigned int count;
	void *buffer;
	int rc;

	/* Validate blocksize */
	if ( sandev_blksize ( sandev ) != INT13_BLKSIZE ) {
		DBGC ( sandev->drive, "\nINT 13 drive %02x invalid blocksize "
		       "(%zd) for non-extended read/write\n",
		       sandev->drive, sandev_blksize ( sandev ) );
		return -INT13_STATUS_INVALID;
	}

	/* Calculate parameters */
	cylinder = ( ( ( ix86->regs.cl & 0xc0 ) << 2 ) | ix86->regs.ch );
	head = ix86->regs.dh;
	sector = ( ix86->regs.cl & 0x3f );
	if ( ( cylinder >= int13->cylinders ) ||
	     ( head >= int13->heads ) ||
	     ( sector < 1 ) || ( sector > int13->sectors_per_track ) ) {
		DBGC ( sandev->drive, "C/H/S %d/%d/%d out of range for "
		       "geometry %d/%d/%d\n", cylinder, head, sector,
		       int13->cylinders, int13->heads,
		       int13->sectors_per_track );
		return -INT13_STATUS_INVALID;
	}
	lba = ( ( ( ( cylinder * int13->heads ) + head )
		  * int13->sectors_per_track ) + sector - 1 );
	count = ix86->regs.al;
	buffer = real_to_virt ( ix86->segs.es, ix86->regs.bx );

	DBGC2 ( sandev->drive, "C/H/S %d/%d/%d = LBA %08lx <-> %04x:%04x "
		"(count %d)\n", cylinder, head, sector, lba, ix86->segs.es,
		ix86->regs.bx, count );

	/* Read from / write to block device */
	if ( ( rc = sandev_rw ( sandev, lba, count, buffer ) ) != 0 ){
		DBGC ( sandev->drive, "INT13 drive %02x I/O failed: %s\n",
		       sandev->drive, strerror ( rc ) );
		return -INT13_STATUS_READ_ERROR;
	}

	return 0;
}

/**
 * INT 13, 02 - Read sectors
 *
 * @v sandev		SAN device
 * @v al		Number of sectors to read (must be nonzero)
 * @v ch		Low bits of cylinder number
 * @v cl (bits 7:6)	High bits of cylinder number
 * @v cl (bits 5:0)	Sector number
 * @v dh		Head number
 * @v es:bx		Data buffer
 * @ret status		Status code
 * @ret al		Number of sectors read
 */
static int int13_read_sectors ( struct san_device *sandev,
				struct i386_all_regs *ix86 ) {

	DBGC2 ( sandev->drive, "Read: " );
	return int13_rw_sectors ( sandev, ix86, sandev_read );
}

/**
 * INT 13, 03 - Write sectors
 *
 * @v sandev		SAN device
 * @v al		Number of sectors to write (must be nonzero)
 * @v ch		Low bits of cylinder number
 * @v cl (bits 7:6)	High bits of cylinder number
 * @v cl (bits 5:0)	Sector number
 * @v dh		Head number
 * @v es:bx		Data buffer
 * @ret status		Status code
 * @ret al		Number of sectors written
 */
static int int13_write_sectors ( struct san_device *sandev,
				 struct i386_all_regs *ix86 ) {

	DBGC2 ( sandev->drive, "Write: " );
	return int13_rw_sectors ( sandev, ix86, sandev_write );
}

/**
 * INT 13, 08 - Get drive parameters
 *
 * @v sandev		SAN device
 * @ret status		Status code
 * @ret ch		Low bits of maximum cylinder number
 * @ret cl (bits 7:6)	High bits of maximum cylinder number
 * @ret cl (bits 5:0)	Maximum sector number
 * @ret dh		Maximum head number
 * @ret dl		Number of drives
 */
static int int13_get_parameters ( struct san_device *sandev,
				  struct i386_all_regs *ix86 ) {
	struct int13_data *int13 = sandev->priv;
	unsigned int max_cylinder = int13->cylinders - 1;
	unsigned int max_head = int13->heads - 1;
	unsigned int max_sector = int13->sectors_per_track; /* sic */

	DBGC2 ( sandev->drive, "Get drive parameters\n" );

	/* Validate blocksize */
	if ( sandev_blksize ( sandev ) != INT13_BLKSIZE ) {
		DBGC ( sandev->drive, "\nINT 13 drive %02x invalid blocksize "
		       "(%zd) for non-extended parameters\n",
		       sandev->drive, sandev_blksize ( sandev ) );
		return -INT13_STATUS_INVALID;
	}

	/* Common parameters */
	ix86->regs.ch = ( max_cylinder & 0xff );
	ix86->regs.cl = ( ( ( max_cylinder >> 8 ) << 6 ) | max_sector );
	ix86->regs.dh = max_head;
	ix86->regs.dl = ( int13_is_fdd ( sandev ) ? num_fdds : num_drives );

	/* Floppy-specific parameters */
	if ( int13_is_fdd ( sandev ) ) {
		ix86->regs.bl = INT13_FDD_TYPE_1M44;
		ix86->segs.es = rm_ds;
		ix86->regs.di = __from_data16 ( &int13_fdd_params );
	}

	return 0;
}

/**
 * INT 13, 15 - Get disk type
 *
 * @v sandev		SAN device
 * @ret ah		Type code
 * @ret cx:dx		Sector count
 * @ret status		Status code / disk type
 */
static int int13_get_disk_type ( struct san_device *sandev,
				 struct i386_all_regs *ix86 ) {
	uint32_t blocks;

	DBGC2 ( sandev->drive, "Get disk type\n" );

	if ( int13_is_fdd ( sandev ) ) {
		return INT13_DISK_TYPE_FDD;
	} else {
		blocks = int13_capacity32 ( sandev );
		ix86->regs.cx = ( blocks >> 16 );
		ix86->regs.dx = ( blocks & 0xffff );
		return INT13_DISK_TYPE_HDD;
	}
}

/**
 * INT 13, 41 - Extensions installation check
 *
 * @v sandev		SAN device
 * @v bx		0x55aa
 * @ret bx		0xaa55
 * @ret cx		Extensions API support bitmap
 * @ret status		Status code / API version
 */
static int int13_extension_check ( struct san_device *sandev,
				   struct i386_all_regs *ix86 ) {

	if ( ( ix86->regs.bx == 0x55aa ) && ! int13_is_fdd ( sandev ) ) {
		DBGC2 ( sandev->drive, "INT13 extensions check\n" );
		ix86->regs.bx = 0xaa55;
		ix86->regs.cx = ( INT13_EXTENSION_LINEAR |
				  INT13_EXTENSION_EDD |
				  INT13_EXTENSION_64BIT );
		return INT13_EXTENSION_VER_3_0;
	} else {
		return -INT13_STATUS_INVALID;
	}
}

/**
 * Extended read / write
 *
 * @v sandev		SAN device
 * @v ds:si		Disk address packet
 * @v sandev_rw		SAN device read/write method
 * @ret status		Status code
 */
static int int13_extended_rw ( struct san_device *sandev,
			       struct i386_all_regs *ix86,
			       int ( * sandev_rw ) ( struct san_device *sandev,
						     uint64_t lba,
						     unsigned int count,
						     void *buffer ) ) {
	struct int13_disk_address addr;
	uint8_t bufsize;
	uint64_t lba;
	unsigned long count;
	void *buffer;
	int rc;

	/* Extended reads are not allowed on floppy drives.
	 * ELTORITO.SYS seems to assume that we are really a CD-ROM if
	 * we support extended reads for a floppy drive.
	 */
	if ( int13_is_fdd ( sandev ) )
		return -INT13_STATUS_INVALID;

	/* Get buffer size */
	get_real ( bufsize, ix86->segs.ds,
		   ( ix86->regs.si + offsetof ( typeof ( addr ), bufsize ) ) );
	if ( bufsize < offsetof ( typeof ( addr ), buffer_phys ) ) {
		DBGC2 ( sandev->drive, "<invalid buffer size %#02x\n>\n",
			bufsize );
		return -INT13_STATUS_INVALID;
	}

	/* Read parameters from disk address structure */
	memset ( &addr, 0, sizeof ( addr ) );
	copy_from_real ( &addr, ix86->segs.ds, ix86->regs.si, bufsize );
	lba = addr.lba;
	DBGC2 ( sandev->drive, "LBA %08llx <-> ",
		( ( unsigned long long ) lba ) );
	if ( ( addr.count == 0xff ) ||
	     ( ( addr.buffer.segment == 0xffff ) &&
	       ( addr.buffer.offset == 0xffff ) ) ) {
		buffer = phys_to_virt ( addr.buffer_phys );
		DBGC2 ( sandev->drive, "%08llx",
			( ( unsigned long long ) addr.buffer_phys ) );
	} else {
		buffer = real_to_virt ( addr.buffer.segment,
					addr.buffer.offset );
		DBGC2 ( sandev->drive, "%04x:%04x", addr.buffer.segment,
			addr.buffer.offset );
	}
	if ( addr.count <= 0x7f ) {
		count = addr.count;
	} else if ( addr.count == 0xff ) {
		count = addr.long_count;
	} else {
		DBGC2 ( sandev->drive, " <invalid count %#02x>\n", addr.count );
		return -INT13_STATUS_INVALID;
	}
	DBGC2 ( sandev->drive, " (count %ld)\n", count );

	/* Read from / write to block device */
	if ( ( rc = sandev_rw ( sandev, lba, count, buffer ) ) != 0 ) {
		DBGC ( sandev->drive, "INT13 drive %02x extended I/O failed: "
		       "%s\n", sandev->drive, strerror ( rc ) );
		/* Record that no blocks were transferred successfully */
		addr.count = 0;
		put_real ( addr.count, ix86->segs.ds,
			   ( ix86->regs.si +
			     offsetof ( typeof ( addr ), count ) ) );
		return -INT13_STATUS_READ_ERROR;
	}

	return 0;
}

/**
 * INT 13, 42 - Extended read
 *
 * @v sandev		SAN device
 * @v ds:si		Disk address packet
 * @ret status		Status code
 */
static int int13_extended_read ( struct san_device *sandev,
				 struct i386_all_regs *ix86 ) {

	DBGC2 ( sandev->drive, "Extended read: " );
	return int13_extended_rw ( sandev, ix86, sandev_read );
}

/**
 * INT 13, 43 - Extended write
 *
 * @v sandev		SAN device
 * @v ds:si		Disk address packet
 * @ret status		Status code
 */
static int int13_extended_write ( struct san_device *sandev,
				  struct i386_all_regs *ix86 ) {

	DBGC2 ( sandev->drive, "Extended write: " );
	return int13_extended_rw ( sandev, ix86, sandev_write );
}

/**
 * INT 13, 44 - Verify sectors
 *
 * @v sandev		SAN device
 * @v ds:si		Disk address packet
 * @ret status		Status code
 */
static int int13_extended_verify ( struct san_device *sandev,
				   struct i386_all_regs *ix86 ) {
	struct int13_disk_address addr;
	uint64_t lba;
	unsigned long count;

	/* Read parameters from disk address structure */
	if ( DBG_EXTRA ) {
		copy_from_real ( &addr, ix86->segs.ds, ix86->regs.si,
				 sizeof ( addr ));
		lba = addr.lba;
		count = addr.count;
		DBGC2 ( sandev->drive, "Verify: LBA %08llx (count %ld)\n",
			( ( unsigned long long ) lba ), count );
	}

	/* We have no mechanism for verifying sectors */
	return -INT13_STATUS_INVALID;
}

/**
 * INT 13, 44 - Extended seek
 *
 * @v sandev		SAN device
 * @v ds:si		Disk address packet
 * @ret status		Status code
 */
static int int13_extended_seek ( struct san_device *sandev,
				 struct i386_all_regs *ix86 ) {
	struct int13_disk_address addr;
	uint64_t lba;
	unsigned long count;

	/* Read parameters from disk address structure */
	if ( DBG_EXTRA ) {
		copy_from_real ( &addr, ix86->segs.ds, ix86->regs.si,
				 sizeof ( addr ));
		lba = addr.lba;
		count = addr.count;
		DBGC2 ( sandev->drive, "Seek: LBA %08llx (count %ld)\n",
			( ( unsigned long long ) lba ), count );
	}

	/* Ignore and return success */
	return 0;
}

/**
 * Build device path information
 *
 * @v sandev		SAN device
 * @v dpi		Device path information
 * @ret rc		Return status code
 */
static int int13_device_path_info ( struct san_device *sandev,
				    struct edd_device_path_information *dpi ) {
	struct san_path *sanpath;
	struct device *device;
	struct device_description *desc;
	unsigned int i;
	uint8_t sum = 0;
	int rc;

	/* Reopen block device if necessary */
	if ( sandev_needs_reopen ( sandev ) &&
	     ( ( rc = sandev_reopen ( sandev ) ) != 0 ) )
		return rc;
	sanpath = sandev->active;
	assert ( sanpath != NULL );

	/* Get underlying hardware device */
	device = identify_device ( &sanpath->block );
	if ( ! device ) {
		DBGC ( sandev->drive, "INT13 drive %02x cannot identify "
		       "hardware device\n", sandev->drive );
		return -ENODEV;
	}

	/* Fill in bus type and interface path */
	desc = &device->desc;
	switch ( desc->bus_type ) {
	case BUS_TYPE_PCI:
		dpi->host_bus_type.type = EDD_BUS_TYPE_PCI;
		dpi->interface_path.pci.bus = PCI_BUS ( desc->location );
		dpi->interface_path.pci.slot = PCI_SLOT ( desc->location );
		dpi->interface_path.pci.function = PCI_FUNC ( desc->location );
		dpi->interface_path.pci.channel = 0xff; /* unused */
		break;
	default:
		DBGC ( sandev->drive, "INT13 drive %02x unrecognised bus "
		       "type %d\n", sandev->drive, desc->bus_type );
		return -ENOTSUP;
	}

	/* Get EDD block device description */
	if ( ( rc = edd_describe ( &sanpath->block, &dpi->interface_type,
				   &dpi->device_path ) ) != 0 ) {
		DBGC ( sandev->drive, "INT13 drive %02x cannot identify "
		       "block device: %s\n", sandev->drive, strerror ( rc ) );
		return rc;
	}

	/* Fill in common fields and fix checksum */
	dpi->key = EDD_DEVICE_PATH_INFO_KEY;
	dpi->len = sizeof ( *dpi );
	for ( i = 0 ; i < sizeof ( *dpi ) ; i++ )
		sum += *( ( ( uint8_t * ) dpi ) + i );
	dpi->checksum -= sum;

	return 0;
}

/**
 * INT 13, 48 - Get extended parameters
 *
 * @v sandev		SAN device
 * @v ds:si		Drive parameter table
 * @ret status		Status code
 */
static int int13_get_extended_parameters ( struct san_device *sandev,
					   struct i386_all_regs *ix86 ) {
	struct int13_data *int13 = sandev->priv;
	struct int13_disk_parameters params;
	struct segoff address;
	size_t len = sizeof ( params );
	uint16_t bufsize;
	int rc;

	/* Get buffer size */
	get_real ( bufsize, ix86->segs.ds,
		   ( ix86->regs.si + offsetof ( typeof ( params ), bufsize )));

	DBGC2 ( sandev->drive, "Get extended drive parameters to "
		"%04x:%04x+%02x\n", ix86->segs.ds, ix86->regs.si, bufsize );

	/* Build drive parameters */
	memset ( &params, 0, sizeof ( params ) );
	params.flags = INT13_FL_DMA_TRANSPARENT;
	if ( ( int13->cylinders < 1024 ) &&
	     ( sandev_capacity ( sandev ) <= INT13_MAX_CHS_SECTORS ) ) {
		params.flags |= INT13_FL_CHS_VALID;
	}
	params.cylinders = int13->cylinders;
	params.heads = int13->heads;
	params.sectors_per_track = int13->sectors_per_track;
	params.sectors = sandev_capacity ( sandev );
	params.sector_size = sandev_blksize ( sandev );
	memset ( &params.dpte, 0xff, sizeof ( params.dpte ) );
	if ( ( rc = int13_device_path_info ( sandev, &params.dpi ) ) != 0 ) {
		DBGC ( sandev->drive, "INT13 drive %02x could not provide "
		       "device path information: %s\n",
		       sandev->drive, strerror ( rc ) );
		len = offsetof ( typeof ( params ), dpi );
	}

	/* Calculate returned "buffer size" (which will be less than
	 * the length actually copied if device path information is
	 * present).
	 */
	if ( bufsize < offsetof ( typeof ( params ), dpte ) )
		return -INT13_STATUS_INVALID;
	if ( bufsize < offsetof ( typeof ( params ), dpi ) ) {
		params.bufsize = offsetof ( typeof ( params ), dpte );
	} else {
		params.bufsize = offsetof ( typeof ( params ), dpi );
	}

	DBGC ( sandev->drive, "INT 13 drive %02x described using extended "
	       "parameters:\n", sandev->drive );
	address.segment = ix86->segs.ds;
	address.offset = ix86->regs.si;
	DBGC_HDA ( sandev->drive, address, &params, len );

	/* Return drive parameters */
	if ( len > bufsize )
		len = bufsize;
	copy_to_real ( ix86->segs.ds, ix86->regs.si, &params, len );

	return 0;
}

/**
 * INT 13, 4b - Get status or terminate CD-ROM emulation
 *
 * @v sandev		SAN device
 * @v ds:si		Specification packet
 * @ret status		Status code
 */
static int int13_cdrom_status_terminate ( struct san_device *sandev,
					  struct i386_all_regs *ix86 ) {
	struct int13_cdrom_specification specification;

	DBGC2 ( sandev->drive, "Get CD-ROM emulation status to %04x:%04x%s\n",
		ix86->segs.ds, ix86->regs.si,
		( ix86->regs.al ? "" : " and terminate" ) );

	/* Fail if we are not a CD-ROM */
	if ( ! sandev->is_cdrom ) {
		DBGC ( sandev->drive, "INT13 drive %02x is not a CD-ROM\n",
		       sandev->drive );
		return -INT13_STATUS_INVALID;
	}

	/* Build specification packet */
	memset ( &specification, 0, sizeof ( specification ) );
	specification.size = sizeof ( specification );
	specification.drive = sandev->drive;

	/* Return specification packet */
	copy_to_real ( ix86->segs.ds, ix86->regs.si, &specification,
		       sizeof ( specification ) );

	return 0;
}


/**
 * INT 13, 4d - Read CD-ROM boot catalog
 *
 * @v sandev		SAN device
 * @v ds:si		Command packet
 * @ret status		Status code
 */
static int int13_cdrom_read_boot_catalog ( struct san_device *sandev,
					   struct i386_all_regs *ix86 ) {
	struct int13_data *int13 = sandev->priv;
	struct int13_cdrom_boot_catalog_command command;
	unsigned int start;
	int rc;

	/* Read parameters from command packet */
	copy_from_real ( &command, ix86->segs.ds, ix86->regs.si,
			 sizeof ( command ) );
	DBGC2 ( sandev->drive, "Read CD-ROM boot catalog to %08x\n",
		command.buffer );

	/* Fail if we have no boot catalog */
	if ( ! int13->boot_catalog ) {
		DBGC ( sandev->drive, "INT13 drive %02x has no boot catalog\n",
		       sandev->drive );
		return -INT13_STATUS_INVALID;
	}
	start = ( int13->boot_catalog + command.start );

	/* Read from boot catalog */
	if ( ( rc = sandev_read ( sandev, start, command.count,
				  phys_to_virt ( command.buffer ) ) ) != 0 ) {
		DBGC ( sandev->drive, "INT13 drive %02x could not read boot "
		       "catalog: %s\n", sandev->drive, strerror ( rc ) );
		return -INT13_STATUS_READ_ERROR;
	}

	return 0;
}

/**
 * INT 13 handler
 *
 */
static __asmcall __used void int13 ( struct i386_all_regs *ix86 ) {
	int command = ix86->regs.ah;
	unsigned int bios_drive = ix86->regs.dl;
	struct san_device *sandev;
	struct int13_data *int13;
	int status;

	/* Check BIOS hasn't killed off our drive */
	int13_check_num_drives();

	for_each_sandev ( sandev ) {

		int13 = sandev->priv;
		if ( bios_drive != sandev->drive ) {
			/* Remap any accesses to this drive's natural number */
			if ( bios_drive == int13->natural_drive ) {
				DBGC2 ( sandev->drive, "INT13,%02x (%02x) "
					"remapped to (%02x)\n", ix86->regs.ah,
					bios_drive, sandev->drive );
				ix86->regs.dl = sandev->drive;
				return;
			} else if ( ( ( bios_drive & 0x7f ) == 0x7f ) &&
				    ( command == INT13_CDROM_STATUS_TERMINATE )
				    && sandev->is_cdrom ) {
				/* Catch non-drive-specific CD-ROM calls */
			} else {
				continue;
			}
		}
		
		DBGC2 ( sandev->drive, "INT13,%02x (%02x): ",
			ix86->regs.ah, bios_drive );

		switch ( command ) {
		case INT13_RESET:
			status = int13_reset ( sandev, ix86 );
			break;
		case INT13_GET_LAST_STATUS:
			status = int13_get_last_status ( sandev, ix86 );
			break;
		case INT13_READ_SECTORS:
			status = int13_read_sectors ( sandev, ix86 );
			break;
		case INT13_WRITE_SECTORS:
			status = int13_write_sectors ( sandev, ix86 );
			break;
		case INT13_GET_PARAMETERS:
			status = int13_get_parameters ( sandev, ix86 );
			break;
		case INT13_GET_DISK_TYPE:
			status = int13_get_disk_type ( sandev, ix86 );
			break;
		case INT13_EXTENSION_CHECK:
			status = int13_extension_check ( sandev, ix86 );
			break;
		case INT13_EXTENDED_READ:
			status = int13_extended_read ( sandev, ix86 );
			break;
		case INT13_EXTENDED_WRITE:
			status = int13_extended_write ( sandev, ix86 );
			break;
		case INT13_EXTENDED_VERIFY:
			status = int13_extended_verify ( sandev, ix86 );
			break;
		case INT13_EXTENDED_SEEK:
			status = int13_extended_seek ( sandev, ix86 );
			break;
		case INT13_GET_EXTENDED_PARAMETERS:
			status = int13_get_extended_parameters ( sandev, ix86 );
			break;
		case INT13_CDROM_STATUS_TERMINATE:
			status = int13_cdrom_status_terminate ( sandev, ix86 );
			break;
		case INT13_CDROM_READ_BOOT_CATALOG:
			status = int13_cdrom_read_boot_catalog ( sandev, ix86 );
			break;
		default:
			DBGC2 ( sandev->drive, "*** Unrecognised INT13 ***\n" );
			status = -INT13_STATUS_INVALID;
			break;
		}

		/* Store status for INT 13,01 */
		int13->last_status = status;

		/* Negative status indicates an error */
		if ( status < 0 ) {
			status = -status;
			DBGC ( sandev->drive, "INT13,%02x (%02x) failed with "
			       "status %02x\n", ix86->regs.ah, sandev->drive,
			       status );
		} else {
			ix86->flags &= ~CF;
		}
		ix86->regs.ah = status;

		/* Set OF to indicate to wrapper not to chain this call */
		ix86->flags |= OF;

		return;
	}
}

/**
 * Hook INT 13 handler
 *
 */
static void int13_hook_vector ( void ) {
	/* Assembly wrapper to call int13().  int13() sets OF if we
	 * should not chain to the previous handler.  (The wrapper
	 * clears CF and OF before calling int13()).
	 */
	__asm__  __volatile__ (
	       TEXT16_CODE ( "\nint13_wrapper:\n\t"
			     /* Preserve %ax and %dx for future reference */
			     "pushw %%bp\n\t"
			     "movw %%sp, %%bp\n\t"			     
			     "pushw %%ax\n\t"
			     "pushw %%dx\n\t"
			     /* Clear OF, set CF, call int13() */
			     "orb $0, %%al\n\t" 
			     "stc\n\t"
			     VIRT_CALL ( int13 )
			     /* Chain if OF not set */
			     "jo 1f\n\t"
			     "pushfw\n\t"
			     "lcall *%%cs:int13_vector\n\t"
			     "\n1:\n\t"
			     /* Overwrite flags for iret */
			     "pushfw\n\t"
			     "popw 6(%%bp)\n\t"
			     /* Fix up %dl:
			      *
			      * INT 13,15 : do nothing if hard disk
			      * INT 13,08 : load with number of drives
			      * all others: restore original value
			      */
			     "cmpb $0x15, -1(%%bp)\n\t"
			     "jne 2f\n\t"
			     "testb $0x80, -4(%%bp)\n\t"
			     "jnz 3f\n\t"
			     "\n2:\n\t"
			     "movb -4(%%bp), %%dl\n\t"
			     "cmpb $0x08, -1(%%bp)\n\t"
			     "jne 3f\n\t"
			     "testb $0x80, %%dl\n\t"
			     "movb %%cs:num_drives, %%dl\n\t"
			     "jnz 3f\n\t"
			     "movb %%cs:num_fdds, %%dl\n\t"
			     /* Return */
			     "\n3:\n\t"
			     "movw %%bp, %%sp\n\t"
			     "popw %%bp\n\t"
			     "iret\n\t" ) : : );

	hook_bios_interrupt ( 0x13, ( intptr_t ) int13_wrapper, &int13_vector );
}

/**
 * Unhook INT 13 handler
 */
static void int13_unhook_vector ( void ) {
	unhook_bios_interrupt ( 0x13, ( intptr_t ) int13_wrapper,
				&int13_vector );
}

/**
 * Hook INT 13 SAN device
 *
 * @v drive		Drive number
 * @v uris		List of URIs
 * @v count		Number of URIs
 * @v flags		Flags
 * @ret drive		Drive number, or negative error
 *
 * Registers the drive with the INT 13 emulation subsystem, and hooks
 * the INT 13 interrupt vector (if not already hooked).
 */
static int int13_hook ( unsigned int drive, struct uri **uris,
			unsigned int count, unsigned int flags ) {
	struct san_device *sandev;
	struct int13_data *int13;
	unsigned int natural_drive;
	void *scratch;
	int need_hook = ( ! have_sandevs() );
	int rc;

	/* Calculate natural drive number */
	int13_sync_num_drives();
	natural_drive = ( ( drive & 0x80 ) ? ( num_drives | 0x80 ) : num_fdds );

	/* Use natural drive number if directed to do so */
	if ( ( drive & 0x7f ) == 0x7f )
		drive = natural_drive;

	/* Allocate SAN device */
	sandev = alloc_sandev ( uris, count, sizeof ( *int13 ) );
	if ( ! sandev ) {
		rc = -ENOMEM;
		goto err_alloc;
	}
	int13 = sandev->priv;
	int13->natural_drive = natural_drive;

	/* Register SAN device */
	if ( ( rc = register_sandev ( sandev, drive, flags ) ) != 0 ) {
		DBGC ( drive, "INT13 drive %02x could not register: %s\n",
		       drive, strerror ( rc ) );
		goto err_register;
	}

	/* Allocate scratch area */
	scratch = malloc ( sandev_blksize ( sandev ) );
	if ( ! scratch )
		goto err_alloc_scratch;

	/* Parse parameters, if present */
	if ( sandev->is_cdrom &&
	     ( ( rc = int13_parse_eltorito ( sandev, scratch ) ) != 0 ) )
		goto err_parse_eltorito;

	/* Give drive a default geometry, if applicable */
	if ( ( sandev_blksize ( sandev ) == INT13_BLKSIZE ) &&
	     ( ( rc = int13_guess_geometry ( sandev, scratch ) ) != 0 ) )
		goto err_guess_geometry;

	DBGC ( drive, "INT13 drive %02x (naturally %02x) registered with "
	       "C/H/S geometry %d/%d/%d\n", drive, int13->natural_drive,
	       int13->cylinders, int13->heads, int13->sectors_per_track );

	/* Hook INT 13 vector if not already hooked */
	if ( need_hook ) {
		int13_hook_vector();
		devices_get();
	}

	/* Update BIOS drive count */
	int13_sync_num_drives();

	free ( scratch );
	return drive;

 err_guess_geometry:
 err_parse_eltorito:
	free ( scratch );
 err_alloc_scratch:
	unregister_sandev ( sandev );
 err_register:
	sandev_put ( sandev );
 err_alloc:
	return rc;
}

/**
 * Unhook INT 13 SAN device
 *
 * @v drive		Drive number
 *
 * Unregisters the drive from the INT 13 emulation subsystem.  If this
 * is the last SAN device, the INT 13 vector is unhooked (if
 * possible).
 */
static void int13_unhook ( unsigned int drive ) {
	struct san_device *sandev;

	/* Find drive */
	sandev = sandev_find ( drive );
	if ( ! sandev ) {
		DBGC ( drive, "INT13 drive %02x is not a SAN drive\n", drive );
		return;
	}

	/* Unregister SAN device */
	unregister_sandev ( sandev );

	/* Should adjust BIOS drive count, but it's difficult
	 * to do so reliably.
	 */

	DBGC ( drive, "INT13 drive %02x unregistered\n", drive );

	/* Unhook INT 13 vector if no more drives */
	if ( ! have_sandevs() ) {
		devices_put();
		int13_unhook_vector();
	}

	/* Drop reference to drive */
	sandev_put ( sandev );
}

/**
 * Load and verify master boot record from INT 13 drive
 *
 * @v drive		Drive number
 * @v address		Boot code address to fill in
 * @ret rc		Return status code
 */
static int int13_load_mbr ( unsigned int drive, struct segoff *address ) {
	uint16_t status;
	int discard_b, discard_c, discard_d;
	uint16_t magic;

	/* Use INT 13, 02 to read the MBR */
	address->segment = 0;
	address->offset = 0x7c00;
	__asm__ __volatile__ ( REAL_CODE ( "pushw %%es\n\t"
					   "pushl %%ebx\n\t"
					   "popw %%bx\n\t"
					   "popw %%es\n\t"
					   "stc\n\t"
					   "sti\n\t"
					   "int $0x13\n\t"
					   "sti\n\t" /* BIOS bugs */
					   "jc 1f\n\t"
					   "xorw %%ax, %%ax\n\t"
					   "\n1:\n\t"
					   "popw %%es\n\t" )
			       : "=a" ( status ), "=b" ( discard_b ),
				 "=c" ( discard_c ), "=d" ( discard_d )
			       : "a" ( 0x0201 ), "b" ( *address ),
				 "c" ( 1 ), "d" ( drive ) );
	if ( status ) {
		DBGC ( drive, "INT13 drive %02x could not read MBR (status "
		       "%04x)\n", drive, status );
		return -EIO;
	}

	/* Check magic signature */
	get_real ( magic, address->segment,
		   ( address->offset +
		     offsetof ( struct master_boot_record, magic ) ) );
	if ( magic != INT13_MBR_MAGIC ) {
		DBGC ( drive, "INT13 drive %02x does not contain a valid MBR\n",
		       drive );
		return -ENOEXEC;
	}

	return 0;
}

/** El Torito boot catalog command packet */
static struct int13_cdrom_boot_catalog_command __data16 ( eltorito_cmd ) = {
	.size = sizeof ( struct int13_cdrom_boot_catalog_command ),
	.count = 1,
	.buffer = 0x7c00,
	.start = 0,
};
#define eltorito_cmd __use_data16 ( eltorito_cmd )

/** El Torito disk address packet */
static struct int13_disk_address __bss16 ( eltorito_address );
#define eltorito_address __use_data16 ( eltorito_address )

/**
 * Load and verify El Torito boot record from INT 13 drive
 *
 * @v drive		Drive number
 * @v address		Boot code address to fill in
 * @ret rc		Return status code
 */
static int int13_load_eltorito ( unsigned int drive, struct segoff *address ) {
	struct {
		struct eltorito_validation_entry valid;
		struct eltorito_boot_entry boot;
	} __attribute__ (( packed )) catalog;
	uint16_t status;

	/* Use INT 13, 4d to read the boot catalog */
	__asm__ __volatile__ ( REAL_CODE ( "stc\n\t"
					   "sti\n\t"
					   "int $0x13\n\t"
					   "sti\n\t" /* BIOS bugs */
					   "jc 1f\n\t"
					   "xorw %%ax, %%ax\n\t"
					   "\n1:\n\t" )
			       : "=a" ( status )
			       : "a" ( 0x4d00 ), "d" ( drive ),
				 "S" ( __from_data16 ( &eltorito_cmd ) ) );
	if ( status ) {
		DBGC ( drive, "INT13 drive %02x could not read El Torito boot "
		       "catalog (status %04x)\n", drive, status );
		return -EIO;
	}
	memcpy ( &catalog, phys_to_virt ( eltorito_cmd.buffer ),
		 sizeof ( catalog ) );

	/* Sanity checks */
	if ( catalog.valid.platform_id != ELTORITO_PLATFORM_X86 ) {
		DBGC ( drive, "INT13 drive %02x El Torito specifies unknown "
		       "platform %02x\n", drive, catalog.valid.platform_id );
		return -ENOEXEC;
	}
	if ( catalog.boot.indicator != ELTORITO_BOOTABLE ) {
		DBGC ( drive, "INT13 drive %02x El Torito is not bootable\n",
		       drive );
		return -ENOEXEC;
	}
	if ( catalog.boot.media_type != ELTORITO_NO_EMULATION ) {
		DBGC ( drive, "INT13 drive %02x El Torito requires emulation "
		       "type %02x\n", drive, catalog.boot.media_type );
		return -ENOTSUP;
	}
	DBGC ( drive, "INT13 drive %02x El Torito boot image at LBA %08x "
	       "(count %d)\n", drive, catalog.boot.start, catalog.boot.length );
	address->segment = ( catalog.boot.load_segment ?
			     catalog.boot.load_segment : 0x7c0 );
	address->offset = 0;
	DBGC ( drive, "INT13 drive %02x El Torito boot image loads at "
	       "%04x:%04x\n", drive, address->segment, address->offset );

	/* Use INT 13, 42 to read the boot image */
	eltorito_address.bufsize =
		offsetof ( typeof ( eltorito_address ), buffer_phys );
	eltorito_address.count = catalog.boot.length;
	eltorito_address.buffer = *address;
	eltorito_address.lba = catalog.boot.start;
	__asm__ __volatile__ ( REAL_CODE ( "stc\n\t"
					   "sti\n\t"
					   "int $0x13\n\t"
					   "sti\n\t" /* BIOS bugs */
					   "jc 1f\n\t"
					   "xorw %%ax, %%ax\n\t"
					   "\n1:\n\t" )
			       : "=a" ( status )
			       : "a" ( 0x4200 ), "d" ( drive ),
				 "S" ( __from_data16 ( &eltorito_address ) ) );
	if ( status ) {
		DBGC ( drive, "INT13 drive %02x could not read El Torito boot "
		       "image (status %04x)\n", drive, status );
		return -EIO;
	}

	return 0;
}

/**
 * Attempt to boot from an INT 13 drive
 *
 * @v drive		Drive number
 * @v config		Boot configuration parameters
 * @ret rc		Return status code
 *
 * This boots from the specified INT 13 drive by loading the Master
 * Boot Record to 0000:7c00 and jumping to it.  INT 18 is hooked to
 * capture an attempt by the MBR to boot the next device.  (This is
 * the closest thing to a return path from an MBR).
 *
 * Note that this function can never return success, by definition.
 */
static int int13_boot ( unsigned int drive,
			struct san_boot_config *config __unused ) {
	struct memory_map memmap;
	struct segoff address;
	int rc;

	/* Look for a usable boot sector */
	if ( ( ( rc = int13_load_mbr ( drive, &address ) ) != 0 ) &&
	     ( ( rc = int13_load_eltorito ( drive, &address ) ) != 0 ) )
		return rc;

	/* Dump out memory map prior to boot, if memmap debugging is
	 * enabled.  Not required for program flow, but we have so
	 * many problems that turn out to be memory-map related that
	 * it's worth doing.
	 */
	get_memmap ( &memmap );

	/* Jump to boot sector */
	if ( ( rc = call_bootsector ( address.segment, address.offset,
				      drive ) ) != 0 ) {
		DBGC ( drive, "INT13 drive %02x boot returned: %s\n",
		       drive, strerror ( rc ) );
		return rc;
	}

	return -ECANCELED; /* -EIMPOSSIBLE */
}

/** Maximum size of boot firmware table(s) */
#define XBFTAB_SIZE 768

/** Alignment of boot firmware table entries */
#define XBFTAB_ALIGN 16

/** The boot firmware table(s) generated by iPXE */
static uint8_t __bss16_array ( xbftab, [XBFTAB_SIZE] )
	__attribute__ (( aligned ( XBFTAB_ALIGN ) ));
#define xbftab __use_data16 ( xbftab )

/** Total used length of boot firmware tables */
static size_t xbftab_used;

/**
 * Install ACPI table
 *
 * @v acpi		ACPI description header
 * @ret rc		Return status code
 */
static int int13_install ( struct acpi_header *acpi ) {
	struct segoff xbft_address;
	struct acpi_header *installed;
	size_t len;

	/* Check length */
	len = acpi->length;
	if ( len > ( sizeof ( xbftab ) - xbftab_used ) ) {
		DBGC ( acpi, "INT13 out of space for %s table\n",
		       acpi_name ( acpi->signature ) );
		return -ENOSPC;
	}

	/* Install table */
	installed = ( ( ( void * ) xbftab ) + xbftab_used );
	memcpy ( installed, acpi, len );
	xbft_address.segment = rm_ds;
	xbft_address.offset = __from_data16 ( installed );

	/* Fill in common parameters */
	strncpy ( installed->oem_id, "FENSYS",
		  sizeof ( installed->oem_id ) );
	strncpy ( installed->oem_table_id, "iPXE",
		  sizeof ( installed->oem_table_id ) );

	/* Fix checksum */
	acpi_fix_checksum ( installed );

	/* Update used length */
	xbftab_used = ( ( xbftab_used + len + XBFTAB_ALIGN - 1 ) &
			~( XBFTAB_ALIGN - 1 ) );

	DBGC ( acpi, "INT13 installed %s:\n",
	       acpi_name ( installed->signature ) );
	DBGC_HDA ( acpi, xbft_address, installed, len );
	return 0;
}

/**
 * Describe SAN devices for SAN-booted operating system
 *
 * @ret rc		Return status code
 */
static int int13_describe ( void ) {
	int rc;

	/* Clear tables */
	memset ( &xbftab, 0, sizeof ( xbftab ) );
	xbftab_used = 0;

	/* Install ACPI tables */
	if ( ( rc = acpi_install ( int13_install ) ) != 0 ) {
		DBG ( "INT13 could not install ACPI tables: %s\n",
		      strerror ( rc ) );
		return rc;
	}

	return 0;
}

PROVIDE_SANBOOT ( pcbios, san_hook, int13_hook );
PROVIDE_SANBOOT ( pcbios, san_unhook, int13_unhook );
PROVIDE_SANBOOT ( pcbios, san_boot, int13_boot );
PROVIDE_SANBOOT ( pcbios, san_describe, int13_describe );
