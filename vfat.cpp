/*
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Richard Braakman <richard.braakman@jollamobile.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
*/

/*
 * This file is a C++ file in order to get the convenience of std::vector.
 * The style is otherwise C though, and it follows C idioms.
 */

#include "vfat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fts.h>

#include <vector>
#include "ConvertUTF.h"

#include "fat.h"

#define SECTORS_PER_CLUSTER (CLUSTER_SIZE / SECTOR_SIZE)
#define RESERVED_SECTORS 32  /* before first FAT */

/*
 * The layout of a FAT filesystem is very simple.
 *
 * - First come RESERVED_SECTORS sectors, which include the boot sector
 * and the filesystem information sector.
 *
 * - Then comes a file allocation table (usually there are two copies
 * but only one is needed here because it's not a real on-disk filesystem).
 *
 * - Then come the data clusters, which are CLUSTER_SIZE each.
 *
 * - Everything is aligned on sector boundaries.
 *
 * - The FAT uses 4 bytes per data cluster to record allocation.
 * The allocations are singly linked lists, with each entry pointing
 * to the next or being an end marker. The first two entries are dummy
 * and don't refer to data clusters.
 *
 * - Some of the files recorded in the FAT are directories, which contain
 * lists of filenames, file sizes and their starting cluster numbers.
 * The boot sector has a pointer to the root directory.
 */

/*
 * The virtual FAT image created here is always laid out with the
 * directories in the beginning and the files at the end, and all
 * the free space in between.
 */

/* This is part of the FAT spec: a fatfs with less than this
 * number of clusters must be FAT12 or FAT16 */
#define MIN_FAT32_CLUSTERS 65525
/* Despite its name, FAT32 only uses 28 bits in its entries.
 * The top 4 bits should be cleared when allocating.
 * Entries 0x0ffffff0 and higher are reserved, as are 0 and 1. */
#define MAX_FAT32_CLUSTERS (0x0ffffff0 - RESERVED_FAT_ENTRIES)

/* The attribute flags used in directory entries */
#define FAT_ATTR_NONE      0x00
#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_LABEL     0x08
#define FAT_ATTR_DIRECTORY 0x10

#define FAT_ATTR_LFN       0x0f  /* marker for long file name segments */

/* The traditional definition of min, unsafe against side effects in a or b
 * but at least not limiting the types of a or b */
#define min(a, b) ((a) < (b) ? (a) : (b))

static const char *g_top_dir;

static uint32_t g_fat_sectors;
static uint32_t g_data_clusters;
static uint32_t g_total_sectors;

/* All multibyte values in here are stored in little-endian format */
uint8_t boot_sector[SECTOR_SIZE] = {
	0xeb, 0xfe, 0x90,  /* x86 asm, infinite loop */
	'T', 'O', 'J', 'B', 'L', 'O', 'C', 'K', /* system id */
	/* 0x00B, start of bios parameter block */
	SECTOR_SIZE & 0xff, (SECTOR_SIZE >> 8) & 0xff,
	SECTORS_PER_CLUSTER,
	RESERVED_SECTORS & 0xff, (RESERVED_SECTORS >> 8) & 0xff,
	1, /* number of FATs */
	0, 0, /* root directory size, N/A for FAT32 */
	0, 0, /* number of sectors, stored below for FAT32 */
	0xf8, /* media descriptor: "fixed disk" */
	0, 0, /* sectors per FAT, stored below for FAT32 */
	1, 0, 1, 0,  /* cylinders and heads info, unused */
	0, 0, 0, 0,  /* sectors before start of partition */
#define SECTORCOUNT_OFFSET 0x20
	0, 0, 0, 0,  /* total sectors */
#define FATSECTORS_OFFSET 0x24
	0, 0, 0, 0,  /* sectors per FAT */
	0, 0,  /* flags about FAT usage, can be left 0 */
	0, 0,  /* fat32 format version 0.0 */
	ROOT_DIR_CLUSTER, 0, 0, 0,  /* cluster number of root directory */
	1, 0,  /* location of filesystem information sector */
	0, 0,  /* location of backup boot sector (none) */
	0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 12 bytes reserved */
	0x80,  /* drive number; 0x80 for first fixed disk */
	0,  /* reserved */
	0x29,  /* indicates next 3 fields are valid */
#define VOLUME_ID_OFFSET 0x43
	0, 0, 0, 0,  /* volume serial number, try to be unique */
#define VOLUME_LABEL_OFFSET 0x47
	'T', 'O', 'J', 'B', 'L', 'O', 'C', 'K', 'F', 'S', ' ',  /* label */
	'F', 'A', 'T', '3', '2', ' ', ' ', ' ',  /* filesystem type */
	0, /* the rest is zero filled */
};
	
uint8_t fsinfo_sector[SECTOR_SIZE];

/* Filenames on the FAT side are represented by little-endian UTF-16 strings,
 * with a terminating 0 value which is included. */
typedef std::vector<uint16_t> filename_t;
#define DIR_ENTRY_SIZE 32
#define CHARS_PER_DIR_ENTRY 13

// These are initialized by vfat_init
static filename_t dot_name;  // contains "."
static filename_t dot_dot_name;  // contains ".."

/*
 * Information about allocated directories.
 * Directories are allocated from the start of the FAT, but to make
 * the scanning code simpler they don't have to be allocated contiguously
 * the way mapped files are.
 */
struct dir_info {
	uint32_t starting_cluster; /* number of first cluster of this dir */
	uint32_t allocated; /* number of allocated clusters */
	std::vector<char> data;
	const char *path;  /* path down from g_top_dir */
};

std::vector<struct dir_info> dir_infos;

/*
 * Information about mapped-in files.
 */
struct filemap_info {
	uint32_t starting_cluster;
	const char *path;  /* path down from g_top_dir */
};

/* filemaps are kept sorted by descending starting_cluster */
std::vector<struct filemap_info> filemaps;

void fill_filename_part(char *data, int seq_nr, bool is_last,
	const filename_t &filename, uint8_t checksum)
{
	static const int char_offsets[CHARS_PER_DIR_ENTRY] =
		{ 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	int i;
	int fn_offset;
	const uint16_t *fn_data;
	int max_i;

	if (is_last)
		data[0] = seq_nr | 0x40;
	else
		data[0] = seq_nr;
	data[11] = FAT_ATTR_LFN;
	data[12] = 0;  /* reserved */
	data[13] = checksum;
	data[26] = 0;  /* cluster nr (unused) */
	data[27] = 0;  /* cluster nr (unused) */

	/* TODO: this loop could be speeded up by special-casing is_last,
	 * because that's the only time that the upper bound might change */

	fn_offset = (seq_nr - 1) * CHARS_PER_DIR_ENTRY;
	fn_data = &filename[0];
	max_i = min(CHARS_PER_DIR_ENTRY, filename.size() - fn_offset);
	for (i = 0; i < max_i; i++, fn_offset++) {
		data[char_offsets[i]] = fn_data[fn_offset] & 0xff;
		data[char_offsets[i] + 1] = fn_data[fn_offset] >> 8;
	}
	for ( ; i < CHARS_PER_DIR_ENTRY; i++) {
		data[char_offsets[i]] = 0xff;
		data[char_offsets[i] + 1] = 0xff;
	}
}

uint8_t calc_vfat_checksum(uint8_t *entry)
{
	uint8_t sum = 0;
	int i;

	for (i = 0; i < 11; i++) {
		sum = ((sum & 1) << 7) + (sum >> 1) + entry[i];
	}
	return sum;
}

/*
 * Fill in just enough of the short entry to be able to calculate the checksum
 */
void prep_short_entry(uint8_t *entry)  /* at least 11-byte buffer */
{
	static uint32_t counter = 1;
	uint32_t uniq = counter++;
	int i;

	/* The first 11 bytes are the shortname buffer.
	 * Fill it with an invalid but still unique value.
	 * See http://lkml.org/lkml/2009/6/26/313 for the algorithm.
	 */

	entry[0] = ' ';
	entry[1] = 0;
	for (i = 2; i < 8; i++) {
		entry[i] = uniq & 0x1f;
		uniq >>= 5;
	}
	entry[8] = '/';
	entry[9] = 0;
	entry[10] = 0;
}

void encode_datetime(uint8_t *buf, time_t stamp)  /* 4-byte buffer */
{
	struct tm *t = localtime(&stamp);
	uint16_t time_part;
	uint16_t date_part;

	time_part = (t->tm_sec / 2) | (t->tm_min << 5) | (t->tm_hour << 11);
	/* struct tm measures years from 1900, but FAT measures from 1980 */
	date_part = (t->tm_mday) | ((t->tm_mon + 1) << 5)
		| ((t->tm_year - 80) << 9);

	buf[0] = time_part & 0xff;
	buf[1] = (time_part >> 8) & 0xff;
	buf[2] = date_part & 0xff;
	buf[3] = (date_part >> 8) & 0xff;
}

void encode_date(uint8_t *buf, time_t stamp)  /* 2-byte buffer */
{
	struct tm *t = gmtime(&stamp);
	uint16_t date_part;

	/* struct tm measures years from 1900, but FAT measures from 1980 */
	date_part = (t->tm_mday) | ((t->tm_mon + 1) << 5)
		| ((t->tm_year - 80) << 9);

	buf[0] = date_part & 0xff;
	buf[1] = (date_part >> 8) & 0xff;
}

bool add_dir_entry(uint32_t parent_clust, uint32_t entry_clust,
	const filename_t &filename, uint32_t file_size, uint8_t attrs,
	time_t mtime, time_t atime)
{
	struct dir_info *parent;
	int num_entries;
	uint32_t clusters_needed;
	int seq_nr;
	int data_offset;
	uint8_t checksum;
	uint8_t short_entry[DIR_ENTRY_SIZE];
	int dir_index;

	/* special case for the root directory, which is found in cluster 2
	 * but which must be referred to as cluster 0 in directory entries,
	 * so it's convenient to correct for it here so that callers don't
	 * have to. */
	if (parent_clust == 0)
		parent_clust = ROOT_DIR_CLUSTER;

	dir_index = fat_dir_index(parent_clust);
	if (dir_index < 0)
		return false;

	parent = &dir_infos[dir_index];

	/* Check if the result will fit in the allocated space */
	/* add one entry for the shortname */
	num_entries = 1 + (filename.size() + CHARS_PER_DIR_ENTRY - 1)
				/ CHARS_PER_DIR_ENTRY;
	if (num_entries > 32) /* filesystem spec limitation */
		return false;
	clusters_needed = ALIGN(parent->data.size()
		+ num_entries * DIR_ENTRY_SIZE, CLUSTER_SIZE) / CLUSTER_SIZE;
	if (clusters_needed > parent->allocated) {
		if (fat_extend(parent->starting_cluster,
			clusters_needed - parent->allocated)) {
			parent->allocated = clusters_needed;
		} else {
			return false;
		}
	}

	prep_short_entry(short_entry);
	attrs |= FAT_ATTR_READ_ONLY;  /* always read-only */
	if (attrs & FAT_ATTR_DIRECTORY)
		file_size = 0;
	short_entry[11] = attrs;
	short_entry[12] = 0;
	/* Slightly higher resolution creation time.
	 * The normal time format only encodes down to 2-second precision. */
	short_entry[13] = (mtime & 1) * 100;
	/* this field calls for creation time but we don't have that, so
	 * substitute last modification time */
	encode_datetime(&short_entry[14], mtime);  /* 4 bytes */
	encode_date(&short_entry[18], atime); /* 2 bytes */
	short_entry[20] = (entry_clust >> 16) & 0xff;
	short_entry[21] = (entry_clust >> 24) & 0xff;
	encode_datetime(&short_entry[22], mtime);
	short_entry[26] = entry_clust & 0xff;
	short_entry[27] = (entry_clust >> 8) & 0xff;
	short_entry[28] = file_size & 0xff;
	short_entry[29] = (file_size >> 8) & 0xff;
	short_entry[30] = (file_size >> 16) & 0xff;
	short_entry[31] = (file_size >> 24) & 0xff;

	data_offset = parent->data.size();
	parent->data.resize(parent->data.size() + num_entries * DIR_ENTRY_SIZE);

	checksum = calc_vfat_checksum(short_entry);
	/* The name parts are stored last-to-first, with decreasing seq_nr */
	for (seq_nr = num_entries - 1; seq_nr >= 1; seq_nr--) {
		fill_filename_part(&parent->data[data_offset], seq_nr,
			seq_nr == num_entries - 1, filename, checksum);
		data_offset += 32;
	}

	memcpy(&parent->data[data_offset], short_entry, DIR_ENTRY_SIZE);
	return true;
}

uint32_t alloc_new_dir(const char *path)
{
	struct dir_info new_dir;

	new_dir.starting_cluster = fat_alloc_dir(dir_infos.size());
	new_dir.allocated = 1;
	new_dir.path = strdup(path);

	dir_infos.push_back(new_dir);

	return new_dir.starting_cluster;
}

uint32_t map_file(const char *name, uint32_t size)
{
	uint32_t nr_clust = ALIGN(size, CLUSTER_SIZE) / CLUSTER_SIZE;
	struct filemap_info fm;

	fm.starting_cluster = fat_alloc_filemap(filemaps.size(), nr_clust);
	fm.path = strdup(name);

	filemaps.push_back(fm);
	return fm.starting_cluster;
}

int dir_fill(char *buf, uint32_t len, int dir_index, uint32_t offset)
{
	std::vector<char> *datap = &dir_infos[dir_index].data;
	uint32_t extra = 0;
	if (offset + len > datap->size()) {
		extra = offset + len - datap->size();
		if (extra > len)
			extra = len;
	}
	memcpy(buf, &datap->front() + offset, len - extra);
	memset(buf + len - extra, 0, extra);
	return 0;
}

int filemap_fill(char *buf, uint32_t len, int fmap_index, uint32_t offset)
{
	const char *path = filemaps[fmap_index].path;
	int nread;
	int fd;
	int ret = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return errno;

	if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
		ret = errno;
	} else {
		nread = read(fd, buf, len);
		if (nread < 0)
			ret = errno;
		else if ((uint32_t) nread < len) // past end of file
			memset(buf + nread, 0, len - nread);
	}
	close(fd);
	return ret;
}

int vfat_fill(void *buf, uint64_t from, uint32_t len)
{
	int ret = 0;

	/*
	 * This is structured as a loop so that each clause can handle
	 * just the case it's focused on and then pass the buck
	 * by setting maxcopy smaller than len.
	 */
	while (len > 0 && ret == 0) {
		uint32_t maxcopy = 0;
		uint32_t sector_nr = from / SECTOR_SIZE;
		if (sector_nr < RESERVED_SECTORS) {
			uint32_t offset = from % SECTOR_SIZE;
			if (sector_nr == 0) {
				maxcopy = min(len, SECTOR_SIZE - from);
				memcpy(buf, &boot_sector[offset], maxcopy);
			} else if (sector_nr == 1) {
				maxcopy = min(len, 2*SECTOR_SIZE - from);
				memcpy(buf, &fsinfo_sector[offset], maxcopy);
			} else {
				maxcopy = min(len,
					RESERVED_SECTORS * SECTOR_SIZE - from);
				memset(buf, 0, maxcopy);
			}
		} else if (sector_nr < RESERVED_SECTORS + g_fat_sectors) {
			/* FAT sector */
			uint32_t entry_nr = (from
				- RESERVED_SECTORS * SECTOR_SIZE) / 4;
			maxcopy = min(len, (RESERVED_SECTORS + g_fat_sectors)
				* SECTOR_SIZE - from);
			if (from % 4 || maxcopy < 4) {
				/* deal with unaligned reads */
				uint32_t fat_entry;
				fat_fill(&fat_entry, entry_nr, 1);
				maxcopy = min(maxcopy, 4);
				memcpy(buf, ((char *)&fat_entry) + from % 4,
					maxcopy);
			} else {
				maxcopy -= maxcopy % 4;
				fat_fill(buf, entry_nr, maxcopy / 4);
			}
		} else if (sector_nr < g_total_sectors) {
			uint64_t adj = from - (RESERVED_SECTORS + g_fat_sectors)
				* SECTOR_SIZE;
			uint32_t data_cluster = (adj / CLUSTER_SIZE)
				+ RESERVED_FAT_ENTRIES;
			uint32_t offset = adj % CLUSTER_SIZE;
			ret = data_fill((char *)buf, len, data_cluster,
				offset, &maxcopy);
		} else {
			/* past end of image */
			ret = EINVAL;
		}
		len -= maxcopy;
		buf = (char *) buf + maxcopy;
		from += maxcopy;
	}

	if (ret && len)
		memset(buf, 0, len);
	return ret;
}

void init_boot_sector(const char *label)
{
	uint32_t volume_id = time(NULL);

	boot_sector[SECTORCOUNT_OFFSET + 0] = g_total_sectors;
	boot_sector[SECTORCOUNT_OFFSET + 1] = g_total_sectors >> 8;
	boot_sector[SECTORCOUNT_OFFSET + 2] = g_total_sectors >> 16;
	boot_sector[SECTORCOUNT_OFFSET + 3] = g_total_sectors >> 24;

	boot_sector[FATSECTORS_OFFSET + 0] = g_fat_sectors;
	boot_sector[FATSECTORS_OFFSET + 1] = g_fat_sectors >> 8;
	boot_sector[FATSECTORS_OFFSET + 2] = g_fat_sectors >> 16;
	boot_sector[FATSECTORS_OFFSET + 3] = g_fat_sectors >> 24;

	boot_sector[VOLUME_ID_OFFSET + 0] = volume_id;
	boot_sector[VOLUME_ID_OFFSET + 1] = volume_id >> 8;
	boot_sector[VOLUME_ID_OFFSET + 2] = volume_id >> 16;
	boot_sector[VOLUME_ID_OFFSET + 3] = volume_id >> 24;

	if (label) {
		size_t len = strnlen(label, 11);
		memcpy(boot_sector + VOLUME_LABEL_OFFSET, label, len);
		memset(boot_sector + VOLUME_LABEL_OFFSET + len, ' ', 11 - len);
	}
}

void init_fsinfo_sector(void)
{
	/* Nothing really useful here, but it's expected to be present */
	memcpy(&fsinfo_sector[0], "RRaA", 4);  /* magic */
	memcpy(&fsinfo_sector[0x1e4], "rrAa", 4);  /* more magic */
	/* unset values for first free cluster and last allocated cluster */
	memcpy(&fsinfo_sector[0x1e8], "\xff\xff\xff\xff", 4);
	memcpy(&fsinfo_sector[0x1ec], "\xff\xff\xff\xff", 4);
	memcpy(&fsinfo_sector[0x1fc], "\0\0\x55\xaa", 4);  /* magic here too */
}

static int convert_name(const char *name8, int namelen, filename_t & name16)
{
	ConversionResult result;
	const uint8_t *inp = (const uint8_t *) name8;
	uint16_t *bufp;

	/* VFAT filenames have to be in UTF-16. Linux filenames
	 * aren't in any particular encoding, but nearly all
	 * systems use UTF-8 these days. */

	/* name16 is passed in to receive the result. */
	name16.clear();
	/* The worst case is that name8 is pure ASCII so each byte
	 * expands to one 16-bit element, so make enough space for that
	 * plus a terminating NUL */
	name16.resize(namelen + 1);

	bufp = &name16[0];

	result = ConvertUTF8toUTF16LE(&inp, inp + namelen,
		&bufp, bufp + namelen, strictConversion);
	if (result != conversionOK)
		return -1;

	/* The conversion routine will have set bufp to point just
	 * past the end of the converted data. Anything past that
	 * will be junk, so shrink the name to leave that out. */
	*bufp++ = 0;
	name16.resize(bufp - &name16[0]);
	return 0;
}

static void scan_fts(FTS *ftsp, FTSENT *entp)
{
	uint32_t clust;
	uint32_t parent;
	off_t size;
	filename_t name;

	/*
	 * The scan makes use of entp->fts_number, which is a field
	 * reserved for our use. For directories we store the cluster
	 * number there, so that we can look it up when scanning the
	 * directory's children. The field is initialized to 0, so
	 * 0 means the root directory.
	 */
	switch (entp->fts_info) {
		case FTS_D: /* directory, first visit */
			if (entp->fts_level == 0) /* root dir is already made */
				break;
			if (convert_name(entp->fts_name,
				entp->fts_namelen, name) < 0) {
				/* directory name couldn't be represented.
				 * skip it and its children. */
				fts_set(ftsp, entp, FTS_SKIP);
				break;
			}
			clust = alloc_new_dir(entp->fts_path);
			parent = entp->fts_parent->fts_number;
			
			/* link the new directory into the hierarchy */
			add_dir_entry(clust, clust, dot_name, 0,
                                FAT_ATTR_DIRECTORY,
				entp->fts_statp->st_mtime,
				entp->fts_statp->st_atime);
			add_dir_entry(clust, parent, dot_dot_name, 0,
                                FAT_ATTR_DIRECTORY,
				entp->fts_parent->fts_statp->st_mtime,
				entp->fts_parent->fts_statp->st_atime);
			add_dir_entry(parent, clust, name, 0,
                                FAT_ATTR_DIRECTORY,
				entp->fts_statp->st_mtime,
				entp->fts_statp->st_atime);
			entp->fts_number = clust;
			break;

		case FTS_F: /* normal file */
			size = entp->fts_statp->st_size;
			if ((off_t) (uint32_t) size != size)
				break;  /* can't represent size */
			if (convert_name(entp->fts_name,
				entp->fts_namelen, name) < 0)
				break;  /* can't represent name */
			parent = entp->fts_parent->fts_number;
			if (size > 0)
				clust = map_file(entp->fts_path, size);
			else
				clust = 0;
			add_dir_entry(parent, clust, name, size,
                                FAT_ATTR_NONE,
				entp->fts_statp->st_mtime,
				entp->fts_statp->st_atime);
			break;

		case FTS_DP: /* directory, second visit (after all children) */
			break;

		default:
			/* Ignore anything else: unstattable files,
			 * unreadable directories, symbolic links, etc.
			 * It won't be representable in the FAT anyway. */
			break;
	}
}

void scan_target_dir(void)
{
	FTS *ftsp;
	FTSENT *entp;
	/* fts_open takes a (char * const *) array, and there's no
	 * way to get a (const char *) into that array without forcing
	 * something. fts_open does intend to leave the strings alone,
	 * so it's safe. */
	char *path_argv[] = { (char *) g_top_dir, 0 };

	/* FTS is a glibc helper for scanning directory trees */
	ftsp = fts_open(path_argv, FTS_PHYSICAL | FTS_XDEV, NULL);
	while ((entp = fts_read(ftsp)))
		scan_fts(ftsp, entp);

	fts_close(ftsp);
	
}

void vfat_init(const char *target_dir, uint64_t free_space, const char *label)
{
	g_top_dir = target_dir;

	init_boot_sector(label);
	init_fsinfo_sector();

	fat_init(g_data_clusters);

	alloc_new_dir("."); /* create empty root directory */

	dot_name.clear();
	dot_name.push_back(htole16('.'));
	dot_name.push_back(0);

	dot_dot_name.clear();
	dot_dot_name.push_back(htole16('.'));
	dot_dot_name.push_back(htole16('.'));
	dot_dot_name.push_back(0);

	scan_target_dir();
	fat_finalize(free_space / CLUSTER_SIZE);
}

/* TODO: do something about the hidden coupling between this function
 * and vfat_init above. */
uint32_t vfat_adjust_size(uint32_t sectors, uint32_t sector_size)
{
	uint32_t data_clusters;
	uint32_t fat_sectors;
	if (sector_size != SECTOR_SIZE)
		return 0;

	/* first calculation is far too optimistic because we need fat space */
	data_clusters = (sectors - RESERVED_SECTORS) / SECTORS_PER_CLUSTER;
	fat_sectors = ALIGN((data_clusters + RESERVED_FAT_ENTRIES) * 4,
		SECTOR_SIZE) / SECTOR_SIZE;

	/* second calculation corrects for that */
	data_clusters = (sectors - fat_sectors - RESERVED_SECTORS)
		/ SECTORS_PER_CLUSTER;
	if (data_clusters < MIN_FAT32_CLUSTERS)  /* imposed by spec */
		data_clusters = MIN_FAT32_CLUSTERS;
	if (data_clusters > MAX_FAT32_CLUSTERS)
		data_clusters = MAX_FAT32_CLUSTERS;
	fat_sectors = ALIGN((data_clusters + RESERVED_FAT_ENTRIES) * 4,
		SECTOR_SIZE) / SECTOR_SIZE;

	g_fat_sectors = fat_sectors;
	g_data_clusters = data_clusters;
	g_total_sectors = RESERVED_SECTORS + fat_sectors
		+ data_clusters * SECTORS_PER_CLUSTER;
	fprintf(stderr, "Image size %lu sectors, %lu reserved, %lu FAT\n",
		(unsigned long) g_total_sectors,
		(unsigned long) RESERVED_SECTORS,
		(unsigned long) g_fat_sectors);
	fprintf(stderr, "Sector size %d, cluster size %d\n",
		SECTOR_SIZE, CLUSTER_SIZE);
	fprintf(stderr, "Contains %lu data clusters starting at 0x%llx\n",
		(unsigned long) g_data_clusters,
		(unsigned long long) (RESERVED_SECTORS + g_fat_sectors) * SECTOR_SIZE);
	return g_total_sectors;
}
