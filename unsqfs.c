/*
 * Copyright (c) 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011
 * Phillip Lougher <phillip@lougher.demon.co.uk>
 *
 * Copyright (c) 2010 LG Electronics
 * Chan Jeong <chan.jeong@lge.com>
 *
 * Copyright (c) 2012 Reality Diluted, LLC
 * Steven J. Hill <sjhill@realitydiluted.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * unsquashfs-min.c
 *
 * Unsquash a squashfs filesystem with minimal support. This code is
 * for little endian only, ignores uid/gid, ignores xattr, only works
 * for squashfs version >4.0, only supports zlib and lzo compression,
 * is only for Linux, is not multi-threaded and does not support any
 * regular expressions. You have been warned.
 *    -Steve
 *
 * To build as a stand-alone test application:
 *
 *    gcc -O2 -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_GNU_SOURCE \
 *       -DTESTING -Wall unsquashfs-min.c -lz -llzo2 -o unsquashfs-func
 *
 * To build as a part of a library or application compile this file
 * and link with the following CFLAGS and LDFLAGS:
 *
 *    CFLAGS += -O2 -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_GNU_SOURCE
 *    LDFLAGS += -lz -llzo2
 */

#define _GNU_SOURCE

#define TRUE 1
#define FALSE 0
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <fnmatch.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <zlib.h>
#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>

#define SQUASHFS_CACHED_FRAGMENTS	CONFIG_SQUASHFS_FRAGMENT_CACHE_SIZE	
#define SQUASHFS_MAGIC			0x73717368
#define SQUASHFS_START			0

/* size of metadata (inode and directory) blocks */
#define SQUASHFS_METADATA_SIZE		8192

/* default size of data blocks */
#define SQUASHFS_FILE_SIZE		131072
#define SQUASHFS_FILE_MAX_SIZE		1048576

/* Max length of filename (not 255) */
#define SQUASHFS_NAME_LEN		256

#define SQUASHFS_INVALID_FRAG		((unsigned int) 0xffffffff)

/* Max number of types and file types */
#define SQUASHFS_DIR_TYPE		1
#define SQUASHFS_FILE_TYPE		2
#define SQUASHFS_LDIR_TYPE		8

/* Flag whether block is compressed or uncompressed, bit is set if block is
 * uncompressed */
#define SQUASHFS_COMPRESSED_BIT		(1 << 15)

#define SQUASHFS_COMPRESSED_SIZE(B)	(((B) & ~SQUASHFS_COMPRESSED_BIT) ? \
		(B) & ~SQUASHFS_COMPRESSED_BIT :  SQUASHFS_COMPRESSED_BIT)

#define SQUASHFS_COMPRESSED(B)		(!((B) & SQUASHFS_COMPRESSED_BIT))

#define SQUASHFS_COMPRESSED_BIT_BLOCK		(1 << 24)

#define SQUASHFS_COMPRESSED_SIZE_BLOCK(B)	((B) & \
	~SQUASHFS_COMPRESSED_BIT_BLOCK)

#define SQUASHFS_COMPRESSED_BLOCK(B)	(!((B) & SQUASHFS_COMPRESSED_BIT_BLOCK))

/*
 * Inode number ops.  Inodes consist of a compressed block number, and an
 * uncompressed  offset within that block
 */
#define SQUASHFS_INODE_BLK(a)		((unsigned int) ((a) >> 16))

#define SQUASHFS_INODE_OFFSET(a)	((unsigned int) ((a) & 0xffff))

/* fragment and fragment table defines */
#define SQUASHFS_FRAGMENT_BYTES(A)	((A) * sizeof(struct squashfs_fragment_entry))

#define SQUASHFS_FRAGMENT_INDEX(A)	(SQUASHFS_FRAGMENT_BYTES(A) / \
					SQUASHFS_METADATA_SIZE)

#define SQUASHFS_FRAGMENT_INDEX_OFFSET(A)	(SQUASHFS_FRAGMENT_BYTES(A) % \
						SQUASHFS_METADATA_SIZE)

#define SQUASHFS_FRAGMENT_INDEXES(A)	((SQUASHFS_FRAGMENT_BYTES(A) + \
					SQUASHFS_METADATA_SIZE - 1) / \
					SQUASHFS_METADATA_SIZE)

#define SQUASHFS_FRAGMENT_INDEX_BYTES(A)	(SQUASHFS_FRAGMENT_INDEXES(A) *\
						sizeof(long long))

/*
 * definitions for structures on disk
 */

typedef long long		squashfs_block;
typedef long long		squashfs_inode;

#define ZLIB_COMPRESSION	1
#define LZO_COMPRESSION		3

struct squashfs_super_block {
	unsigned int		s_magic;
	unsigned int		inodes;
	unsigned int		mkfs_time /* time of filesystem creation */;
	unsigned int		block_size;
	unsigned int		fragments;
	unsigned short		compression;
	unsigned short		block_log;
	unsigned short		flags;
	unsigned short		res0;
	unsigned short		s_major;
	unsigned short		s_minor;
	squashfs_inode		root_inode;
	long long		bytes_used;
	long long		res1;
	long long		res2;
	long long		inode_table_start;
	long long		directory_table_start;
	long long		fragment_table_start;
	long long		res3;
};

struct squashfs_dir_index {
	unsigned int		index;
	unsigned int		start_block;
	unsigned int		size;
	unsigned char		name[0];
};

struct squashfs_base_inode_header {
	unsigned short		inode_type;
	unsigned short		mode;
	unsigned short		uid;
	unsigned short		guid;
	unsigned int		mtime;
	unsigned int 		inode_number;
};

struct squashfs_reg_inode_header {
	unsigned short		inode_type;
	unsigned short		mode;
	unsigned short		uid;
	unsigned short		guid;
	unsigned int		mtime;
	unsigned int 		inode_number;
	unsigned int		start_block;
	unsigned int		fragment;
	unsigned int		offset;
	unsigned int		file_size;
	unsigned int		block_list[0];
};

struct squashfs_dir_inode_header {
	unsigned short		inode_type;
	unsigned short		mode;
	unsigned short		uid;
	unsigned short		guid;
	unsigned int		mtime;
	unsigned int 		inode_number;
	unsigned int		start_block;
	unsigned int		nlink;
	unsigned short		file_size;
	unsigned short		offset;
	unsigned int		parent_inode;
};

struct squashfs_ldir_inode_header {
	unsigned short		inode_type;
	unsigned short		mode;
	unsigned short		uid;
	unsigned short		guid;
	unsigned int		mtime;
	unsigned int 		inode_number;
	unsigned int		nlink;
	unsigned int		file_size;
	unsigned int		start_block;
	unsigned int		parent_inode;
	unsigned short		i_count;
	unsigned short		offset;
	unsigned int		res0;
	struct squashfs_dir_index	index[0];
};

union squashfs_inode_header {
	struct squashfs_base_inode_header	base;
	struct squashfs_reg_inode_header	reg;
	struct squashfs_dir_inode_header	dir;
	struct squashfs_ldir_inode_header	ldir;
};
	
struct squashfs_dir_entry {
	unsigned short		offset;
	short			inode_number;
	unsigned short		type;
	unsigned short		size;
	char			name[0];
};

struct squashfs_dir_header {
	unsigned int		count;
	unsigned int		start_block;
	unsigned int		inode_number;
};

struct squashfs_fragment_entry {
	long long		start_block;
	unsigned int		size;
	unsigned int		unused;
};

#ifdef SQUASHFS_TRACE
#define TRACE(s, args...) \
		do { \
			printf("\n"); \
			printf("unsquashfs: "s, ## args); \
		} while(0)
#else
#define TRACE(s, args...)
#endif

#define ERROR(s, args...) \
		do { \
			fprintf(stderr, "\n"); \
			fprintf(stderr, s, ## args); \
		} while(0)

#define EXIT_UNSQUASH(s, args...) \
		do { \
			fprintf(stderr, "FATAL ERROR aborting: "s, ## args); \
			exit(1); \
		} while(0)

#define CALCULATE_HASH(start)	(start & 0xffff)

struct hash_table_entry {
	long long	start;
	int		bytes;
	struct hash_table_entry *next;
};

struct inode {
	int blocks;
	char *block_ptr;
	long long data;
	int fragment;
	int frag_bytes;
	int inode_number;
	int mode;
	int offset;
	long long start;
	time_t time;
	int type;
	char sparse;
};

/* Cache status struct */
struct cache {
	int	buffer_size;
	struct cache_entry *hash_table[65536];
};

/* struct describing a cache entry */
struct cache_entry {
	struct cache *cache;
	long long block;
	int	size;
	char *data;
};

/* default size of fragment buffer in Mbytes */
#define FRAGMENT_BUFFER_DEFAULT 256
/* default size of data buffer in Mbytes */
#define DATA_BUFFER_DEFAULT 256

#define DIR_ENT_SIZE	16

struct dir_ent	{
	char		name[SQUASHFS_NAME_LEN + 1];
	unsigned int	start_block;
	unsigned int	offset;
	unsigned int	type;
};

struct dir {
	int		dir_count;
	int 		cur_entry;
	unsigned int	mode;
	unsigned int	mtime;
	struct dir_ent	*dirs;
};

struct file_entry {
	int offset;
	int size;
	struct cache_entry *buffer;
};

struct squashfs_file {
	int fd;
	int blocks;
	long long file_size;
	int mode;
	time_t time;
	char *pathname;
	char sparse;
};

struct path_entry {
	char *name;
	struct pathname *paths;
};

struct pathname {
	int names;
	struct path_entry *name;
};

struct pathnames {
	int count;
	struct pathname *path[0];
};
#define PATHS_ALLOC_SIZE 10


struct squashfs_super_block sBlk;
struct squashfs_fragment_entry *fragment_table;

int bytes = 0, file_count = 0, dir_count = 0;
struct cache *fragment_cache, *data_cache;
char *inode_table = NULL, *directory_table = NULL;
struct hash_table_entry *inode_table_hash[65536], *directory_table_hash[65536];
int fd;
unsigned int block_size;
unsigned int block_log;
char **created_inode;
char *zero_data = NULL;
unsigned int cur_blocks = 0;

int lookup_type[] = {
	0,
	S_IFDIR,
	S_IFREG,
	S_IFLNK,
	S_IFBLK,
	S_IFCHR,
	S_IFIFO,
	S_IFSOCK,
	S_IFDIR,
	S_IFREG,
	S_IFLNK,
	S_IFBLK,
	S_IFCHR,
	S_IFIFO,
	S_IFSOCK
};

#ifndef TESTING
/* buffer to return to caller*/
static char *private_buffer;
static int private_count;
#endif


/* forward delcarations */
int read_fs_bytes(int fd, long long byte, int bytes, void *buff);
int read_block(int fd, long long start, long long *next, void *block);
int lookup_entry(struct hash_table_entry *hash_table[], long long start);
void *reader(void *arg);
void *writer(void *arg);
void *deflator(void *arg);


void read_block_list(unsigned int *block_list, char *block_ptr, int blocks)
{
	TRACE("read_block_list: blocks %d\n", blocks);

	memcpy(block_list, block_ptr, blocks * sizeof(unsigned int));
}

int read_fragment_table()
{
	int res, i, indexes = SQUASHFS_FRAGMENT_INDEXES(sBlk.fragments);
	long long fragment_table_index[indexes];

	TRACE("read_fragment_table: %d fragments, reading %d fragment indexes "
		"from 0x%llx\n", sBlk.fragments, indexes,
		sBlk.fragment_table_start);

	if(sBlk.fragments == 0)
		return TRUE;

	fragment_table = malloc(sBlk.fragments *
		sizeof(struct squashfs_fragment_entry));
	if(fragment_table == NULL)
		EXIT_UNSQUASH("read_fragment_table: failed to allocate "
			"fragment table\n");

	res = read_fs_bytes(fd, sBlk.fragment_table_start,
		SQUASHFS_FRAGMENT_INDEX_BYTES(sBlk.fragments),
		fragment_table_index);
	if(res == FALSE) {
		ERROR("read_fragment_table: failed to read fragment table "
			"index\n");
		return FALSE;
	}

	for(i = 0; i < indexes; i++) {
		int length = read_block(fd, fragment_table_index[i], NULL,
			((char *) fragment_table) + (i *
			SQUASHFS_METADATA_SIZE));
		TRACE("Read fragment table block %d, from 0x%llx, length %d\n",
			i, fragment_table_index[i], length);
		if(length == FALSE) {
			ERROR("read_fragment_table: failed to read fragment "
				"table index\n");
			return FALSE;
		}
	}

	return TRUE;
}

void read_fragment(unsigned int fragment, long long *start_block, int *size)
{
	TRACE("read_fragment: reading fragment %d\n", fragment);

	struct squashfs_fragment_entry *fragment_entry;

	fragment_entry = &fragment_table[fragment];
	*start_block = fragment_entry->start_block;
	*size = fragment_entry->size;
}

struct inode *read_inode(unsigned int start_block, unsigned int offset)
{
	static union squashfs_inode_header header;
	long long start = sBlk.inode_table_start + start_block;
	int bytes = lookup_entry(inode_table_hash, start);
	char *block_ptr = inode_table + bytes + offset;
	static struct inode i;

	TRACE("read_inode: reading inode [%d:%d]\n", start_block,  offset);

	if(bytes == -1)
		EXIT_UNSQUASH("read_inode: inode table block %lld not found\n",
			start); 		

	memcpy(&header.base, block_ptr, sizeof(*(&header.base)));

	i.mode = lookup_type[header.base.inode_type] | header.base.mode;
	i.type = header.base.inode_type;
	i.time = header.base.mtime;
	i.inode_number = header.base.inode_number;

	switch(header.base.inode_type) {
		case SQUASHFS_DIR_TYPE: {
			struct squashfs_dir_inode_header *inode = &header.dir;

			memcpy(inode, block_ptr, sizeof(*(inode)));

			i.data = inode->file_size;
			i.offset = inode->offset;
			i.start = inode->start_block;
			break;
		}
		case SQUASHFS_FILE_TYPE: {
			struct squashfs_reg_inode_header *inode = &header.reg;

			memcpy(inode, block_ptr, sizeof(*(inode)));

			i.data = inode->file_size;
			i.frag_bytes = inode->fragment == SQUASHFS_INVALID_FRAG
				?  0 : inode->file_size % sBlk.block_size;
			i.fragment = inode->fragment;
			i.offset = inode->offset;
			i.blocks = inode->fragment == SQUASHFS_INVALID_FRAG ?
				(i.data + sBlk.block_size - 1) >>
				sBlk.block_log :
				i.data >> sBlk.block_log;
			i.start = inode->start_block;
			i.sparse = 0;
			i.block_ptr = block_ptr + sizeof(*inode);
			break;
		}	
                case SQUASHFS_LDIR_TYPE: {
                        struct squashfs_ldir_inode_header *inode = &header.ldir;

			memcpy(inode, block_ptr, sizeof(*(inode)));

                        i.data = inode->file_size;
                        i.offset = inode->offset;
                        i.start = inode->start_block;
                        break;
                }
		default:
			TRACE("read_inode: skipping inode type %d\n", header.base.inode_type);
			return NULL;
	}
	return &i;
}

struct dir *squashfs_opendir(unsigned int block_start, unsigned int offset,
	struct inode **i)
{
	struct squashfs_dir_header dirh;
	char buffer[sizeof(struct squashfs_dir_entry) + SQUASHFS_NAME_LEN + 1]
		__attribute__((aligned));
	struct squashfs_dir_entry *dire = (struct squashfs_dir_entry *) buffer;
	long long start;
	int bytes;
	int dir_count, size;
	struct dir_ent *new_dir;
	struct dir *dir;

	TRACE("squashfs_opendir: inode start block %d, offset %d\n",
		block_start, offset);

	*i = read_inode(block_start, offset);
	start = sBlk.directory_table_start + (*i)->start;
	bytes = lookup_entry(directory_table_hash, start);

	if(bytes == -1)
		EXIT_UNSQUASH("squashfs_opendir: directory block %d not "
			"found!\n", block_start);

	bytes += (*i)->offset;
	size = (*i)->data + bytes - 3;

	dir = malloc(sizeof(struct dir));
	if(dir == NULL)
		EXIT_UNSQUASH("squashfs_opendir: malloc failed!\n");

	dir->dir_count = 0;
	dir->cur_entry = 0;
	dir->mode = (*i)->mode;
	dir->mtime = (*i)->time;
	dir->dirs = NULL;

	while(bytes < size) {			
		memcpy(&dirh, directory_table + bytes, sizeof(*(&dirh)));
	
		dir_count = dirh.count + 1;
		TRACE("squashfs_opendir: Read directory header @ byte position "
			"%d, %d directory entries\n", bytes, dir_count);
		bytes += sizeof(dirh);

		while(dir_count--) {
			memcpy(dire, directory_table + bytes, sizeof(*(dire)));

			bytes += sizeof(*dire);

			memcpy(dire->name, directory_table + bytes,
				dire->size + 1);
			dire->name[dire->size + 1] = '\0';
			TRACE("squashfs_opendir: directory entry %s, inode "
				"%d:%d, type %d\n", dire->name,
				dirh.start_block, dire->offset, dire->type);
			if((dir->dir_count % DIR_ENT_SIZE) == 0) {
				new_dir = realloc(dir->dirs, (dir->dir_count +
					DIR_ENT_SIZE) * sizeof(struct dir_ent));
				if(new_dir == NULL)
					EXIT_UNSQUASH("squashfs_opendir: "
						"realloc failed!\n");
				dir->dirs = new_dir;
			}
			strcpy(dir->dirs[dir->dir_count].name, dire->name);
			dir->dirs[dir->dir_count].start_block =
				dirh.start_block;
			dir->dirs[dir->dir_count].offset = dire->offset;
			dir->dirs[dir->dir_count].type = dire->type;
			dir->dir_count ++;
			bytes += dire->size + 1;
		}
	}

	return dir;
}

int squashfs_uncompress(void *d, void *s, int size, int block_size, int *error)
{
	unsigned long bytes_zlib;
	lzo_uint bytes_lzo;

	if (sBlk.compression == ZLIB_COMPRESSION) {
		bytes_zlib = block_size;
		*error = uncompress(d, &bytes_zlib, s, size);
		return *error == Z_OK ? (int) bytes_zlib : -1;
	} else {
		bytes_lzo = block_size;
		*error = lzo1x_decompress_safe(s, size, d, &bytes_lzo, NULL);
		return *error == LZO_E_OK ? bytes_lzo : -1;
	}
}

struct cache *cache_init(int buffer_size)
{
	struct cache *cache = malloc(sizeof(struct cache));

	if(cache == NULL)
		EXIT_UNSQUASH("Out of memory in cache_init\n");

	cache->buffer_size = buffer_size;
	memset(cache->hash_table, 0, sizeof(struct cache_entry *) * 65536);

	return cache;
}

struct cache_entry *cache_get(struct cache *cache, long long block, int size)
{
	/*
	 * Get a block out of the cache.  If the block isn't in the cache
 	 * it is added and queued to the reader() and deflate() threads for
 	 * reading off disk and decompression.  The cache grows until max_blocks
 	 * is reached, once this occurs existing discarded blocks on the free
 	 * list are reused
 	 */
	struct cache_entry *entry;

	entry = malloc(sizeof(struct cache_entry));
	if(entry == NULL)
		EXIT_UNSQUASH("Out of memory in cache_get\n");
	entry->data = malloc(cache->buffer_size);
	if(entry->data == NULL)
		EXIT_UNSQUASH("Out of memory in cache_get\n");

	entry->cache = cache;
	entry->block = block;
	entry->size = size;

	return reader(entry);
}
	
void add_entry(struct hash_table_entry *hash_table[], long long start,
	int bytes)
{
	int hash = CALCULATE_HASH(start);
	struct hash_table_entry *hash_table_entry;

	hash_table_entry = malloc(sizeof(struct hash_table_entry));
	if(hash_table_entry == NULL)
		EXIT_UNSQUASH("Out of memory in add_entry\n");

	hash_table_entry->start = start;
	hash_table_entry->bytes = bytes;
	hash_table_entry->next = hash_table[hash];
	hash_table[hash] = hash_table_entry;
}

int lookup_entry(struct hash_table_entry *hash_table[], long long start)
{
	int hash = CALCULATE_HASH(start);
	struct hash_table_entry *hash_table_entry;

	for(hash_table_entry = hash_table[hash]; hash_table_entry;
				hash_table_entry = hash_table_entry->next)

		if(hash_table_entry->start == start)
			return hash_table_entry->bytes;

	return -1;
}

int read_fs_bytes(int fd, long long byte, int bytes, void *buff)
{
	off_t off = byte;
	int res, count;

	TRACE("read_bytes: reading from position 0x%llx, bytes %d\n", byte,
		bytes);

	if(lseek(fd, off, SEEK_SET) == -1) {
		ERROR("Lseek failed because %s\n", strerror(errno));
		return FALSE;
	}

	for(count = 0; count < bytes; count += res) {
		res = read(fd, buff + count, bytes - count);
		if(res < 1) {
			if(res == 0) {
				ERROR("Read on filesystem failed because "
					"EOF\n");
				return FALSE;
			} else if(errno != EINTR) {
				ERROR("Read on filesystem failed because %s\n",
						strerror(errno));
				return FALSE;
			} else
				res = 0;
		}
	}

	return TRUE;
}

int read_block(int fd, long long start, long long *next, void *block)
{
	unsigned short c_byte;
	int offset = 2;
	
	if(read_fs_bytes(fd, start, 2, &c_byte) == FALSE)
		goto failed;

	TRACE("read_block: block @0x%llx, %d %s bytes\n", start,
		SQUASHFS_COMPRESSED_SIZE(c_byte), SQUASHFS_COMPRESSED(c_byte) ?
		"compressed" : "uncompressed");

	if(SQUASHFS_COMPRESSED(c_byte)) {
		char buffer[SQUASHFS_METADATA_SIZE];
		int error, res;

		c_byte = SQUASHFS_COMPRESSED_SIZE(c_byte);
		if(read_fs_bytes(fd, start + offset, c_byte, buffer) == FALSE)
			goto failed;

		res = squashfs_uncompress(block, buffer, c_byte,
			SQUASHFS_METADATA_SIZE, &error);

		if(res == -1) {
			ERROR("uncompress failed with error code %d\n", error);
			goto failed;
		}
		if(next)
			*next = start + offset + c_byte;
		return res;
	} else {
		c_byte = SQUASHFS_COMPRESSED_SIZE(c_byte);
		if(read_fs_bytes(fd, start + offset, c_byte, block) == FALSE)
			goto failed;
		if(next)
			*next = start + offset + c_byte;
		return c_byte;
	}

failed:
	ERROR("read_block: failed to read block @0x%llx\n", start);
	return FALSE;
}

void uncompress_inode_table(long long start, long long end)
{
	int size = 0, bytes = 0, res;

	TRACE("uncompress_inode_table: start %lld, end %lld\n", start, end);
	while(start < end) {
		if(size - bytes < SQUASHFS_METADATA_SIZE) {
			inode_table = realloc(inode_table, size +=
				SQUASHFS_METADATA_SIZE);
			if(inode_table == NULL)
				EXIT_UNSQUASH("Out of memory in "
					"uncompress_inode_table");
		}
		TRACE("uncompress_inode_table: reading block 0x%llx\n", start);
		add_entry(inode_table_hash, start, bytes);
		res = read_block(fd, start, &start, inode_table + bytes);
		if(res == 0) {
			free(inode_table);
			EXIT_UNSQUASH("uncompress_inode_table: failed to read "
				"block \n");
		}
		bytes += res;
	}
}

int write_bytes(int fd, char *buff, int bytes)
{
#ifdef TESTING
	int res, count;

	for(count = 0; count < bytes; count += res) {
		res = write(fd, buff + count, bytes - count);
		if(res == -1) {
			if(errno != EINTR) {
				ERROR("Write on output file failed because "
					"%s\n", strerror(errno));
				return -1;
			}
			res = 0;
		}
	}
#else
	if (private_count < 4096) {
		if (bytes > 4096)
			private_count = 4096;
		else
			private_count += bytes;
		memcpy(private_buffer, buff, private_count);
	}
#endif

	return 0;
}

int write_block(int file_fd, char *buffer, int size, long long hole, int sparse)
{
	if(hole) {
		if(sparse == FALSE && zero_data == NULL) {
			if((zero_data = malloc(block_size)) == NULL)
				EXIT_UNSQUASH("write_block: failed to alloc "
					"zero data block\n");
			memset(zero_data, 0, block_size);
		}

		if(sparse == FALSE) {
			int blocks = (hole + block_size -1) / block_size;
			int avail_bytes, i;
			for(i = 0; i < blocks; i++, hole -= avail_bytes) {
				avail_bytes = hole > block_size ? block_size :
					hole;
				if(write_bytes(file_fd, zero_data, avail_bytes)
						== -1)
					goto failure;
			}
		}
	}

	if(write_bytes(file_fd, buffer, size) == -1)
		goto failure;

	return TRUE;

failure:
	return FALSE;
}

int write_file(struct inode *inode, char *pathname)
{
	unsigned int file_fd, i;
	unsigned int *block_list;
	int file_end = inode->data / block_size;
	long long start = inode->start;
	struct squashfs_file *file;

	TRACE("write_file: regular file, blocks %d\n", inode->blocks);

#ifdef TESTING
	file_fd = open(pathname, O_CREAT | O_WRONLY,
		(mode_t) inode->mode & 0777);
	if(file_fd == -1) {
		ERROR("write_file: failed to create file %s, because %s\n",
			pathname, strerror(errno));
		return FALSE;
	}
#else
	file_fd = 0;
#endif

	block_list = malloc(inode->blocks * sizeof(unsigned int));
	if(block_list == NULL)
		EXIT_UNSQUASH("write_file: unable to malloc block list\n");

	read_block_list(block_list, inode->block_ptr, inode->blocks);

	file = malloc(sizeof(struct squashfs_file));
	if(file == NULL)
		EXIT_UNSQUASH("write_file: unable to malloc file\n");

	/*
	 * the writer thread is queued a squashfs_file structure describing the
 	 * file.  If the file has one or more blocks or a fragments they are
 	 * queued separately (references to blocks in the cache).
 	 */
	file->fd = file_fd;
	file->file_size = inode->data;
	file->mode = inode->mode;
	file->time = inode->time;
	file->pathname = strdup(pathname);
	file->blocks = inode->blocks + (inode->frag_bytes > 0);
	file->sparse = inode->sparse;
	writer(file);

	for(i = 0; i < inode->blocks; i++) {
		int c_byte = SQUASHFS_COMPRESSED_SIZE_BLOCK(block_list[i]);
		struct file_entry *block = malloc(sizeof(struct file_entry));

		if(block == NULL)
			EXIT_UNSQUASH("write_file: unable to malloc file\n");
		block->offset = 0;
		block->size = i == file_end ? inode->data & (block_size - 1) :
			block_size;
		if(block_list[i] == 0) /* sparse file */
			block->buffer = NULL;
		else {
			block->buffer = cache_get(data_cache, start,
				block_list[i]);
			start += c_byte;
		}
		writer(block);
	}

	if(inode->frag_bytes) {
		int size;
		long long start;
		struct file_entry *block = malloc(sizeof(struct file_entry));

		if(block == NULL)
			EXIT_UNSQUASH("write_file: unable to malloc file\n");
		read_fragment(inode->fragment, &start, &size);
		block->buffer = cache_get(fragment_cache, start, size);
		block->offset = inode->offset;
		block->size = inode->frag_bytes;
		writer(block);
	}

	free(block_list);
	return TRUE;
}

int create_inode(char *pathname, struct inode *i)
{
	TRACE("create_inode: pathname %s\n", pathname);

	if(created_inode[i->inode_number - 1]) {
		ERROR("create_inode: hardlinks not allowed\n");
		return FALSE;
	}

	switch(i->type) {
		case SQUASHFS_FILE_TYPE:
			TRACE("create_inode: regular file, file_size %lld, "
				"blocks %d\n", i->data, i->blocks);

			if(write_file(i, pathname))
				file_count ++;
			break;
		default:
			ERROR("Unknown inode type %d in create_inode_table!\n",
				i->type);
			return FALSE;
	}

	created_inode[i->inode_number - 1] = strdup(pathname);

	return TRUE;
}

void uncompress_directory_table(long long start, long long end)
{
	int bytes = 0, size = 0, res;

	TRACE("uncompress_directory_table: start %lld, end %lld\n", start, end);

	while(start < end) {
		if(size - bytes < SQUASHFS_METADATA_SIZE) {
			directory_table = realloc(directory_table, size +=
				SQUASHFS_METADATA_SIZE);
			if(directory_table == NULL)
				EXIT_UNSQUASH("Out of memory in "
					"uncompress_directory_table\n");
		}
		TRACE("uncompress_directory_table: reading block 0x%llx\n",
				start);
		add_entry(directory_table_hash, start, bytes);
		res = read_block(fd, start, &start, directory_table + bytes);
		if(res == 0)
			EXIT_UNSQUASH("uncompress_directory_table: failed to "
				"read block\n");
		bytes += res;
	}
}

int squashfs_readdir(struct dir *dir, char **name, unsigned int *start_block,
unsigned int *offset, unsigned int *type)
{
	if(dir->cur_entry == dir->dir_count)
		return FALSE;

	*name = dir->dirs[dir->cur_entry].name;
	*start_block = dir->dirs[dir->cur_entry].start_block;
	*offset = dir->dirs[dir->cur_entry].offset;
	*type = dir->dirs[dir->cur_entry].type;
	dir->cur_entry ++;

	return TRUE;
}

void squashfs_closedir(struct dir *dir)
{
	free(dir->dirs);
	free(dir);
}

const char *get_component(const char *target, char *targname)
{
	while(*target == '/')
		target ++;

	while(*target != '/' && *target!= '\0')
		*targname ++ = *target ++;

	*targname = '\0';

	return target;
}

void free_path(struct pathname *paths)
{
	int i;

	for(i = 0; i < paths->names; i++) {
		if(paths->name[i].paths)
			free_path(paths->name[i].paths);
		free(paths->name[i].name);
	}

	free(paths);
}

struct pathname *add_path(struct pathname *paths,
			const char *target, const char *alltarget)
{
	char targname[1024];
	int i;

	TRACE("add_path: adding \"%s\" extract file\n", target);

	target = get_component(target, targname);

	if(paths == NULL) {
		paths = malloc(sizeof(struct pathname));
		if(paths == NULL)
			EXIT_UNSQUASH("failed to allocate paths\n");

		paths->names = 0;
		paths->name = NULL;
	}

	for(i = 0; i < paths->names; i++)
		if(strcmp(paths->name[i].name, targname) == 0)
			break;

	if(i == paths->names) {
		/*
		 * allocate new name entry
		 */
		paths->names ++;
		paths->name = realloc(paths->name, (i + 1) *
			sizeof(struct path_entry));
		if(paths->name == NULL)
			EXIT_UNSQUASH("Out of memory in add_path\n");	
		paths->name[i].name = strdup(targname);
		paths->name[i].paths = NULL;

		if(target[0] == '\0')
			/*
			 * at leaf pathname component
			*/
			paths->name[i].paths = NULL;
		else
			/*
			 * recurse adding child components
			 */
			paths->name[i].paths = add_path(NULL, target, alltarget);
	} else {
		/*
		 * existing matching entry
		 */
		if(paths->name[i].paths == NULL) {
			/*
			 * No sub-directory which means this is the leaf
			 * component of a pre-existing extract which subsumes
			 * the extract currently being added, in which case stop
			 * adding components
			 */
		} else if(target[0] == '\0') {
			/*
			 * at leaf pathname component and child components exist
			 * from more specific extracts, delete as they're
			 * subsumed by this extract
			 */
			free_path(paths->name[i].paths);
			paths->name[i].paths = NULL;
		} else
			/*
			 * recurse adding child components
			 */
			add_path(paths->name[i].paths, target, alltarget);
	}

	return paths;
}

struct pathnames *init_subdir()
{
	struct pathnames *new = malloc(sizeof(struct pathnames));
	if(new == NULL)
		EXIT_UNSQUASH("Out of memory in init_subdir\n");
	new->count = 0;
	return new;
}

struct pathnames *add_subdir(struct pathnames *paths, struct pathname *path)
{
	if(paths->count % PATHS_ALLOC_SIZE == 0) {
		paths = realloc(paths, sizeof(struct pathnames *) +
			(paths->count + PATHS_ALLOC_SIZE) *
			sizeof(struct pathname *));
		if(paths == NULL)
			EXIT_UNSQUASH("Out of memory in add_subdir\n");
	}

	paths->path[paths->count++] = path;
	return paths;
}

void free_subdir(struct pathnames *paths)
{
	free(paths);
}

int matches(struct pathnames *paths, char *name, struct pathnames **new)
{
	int i, n;

	if(paths == NULL) {
		*new = NULL;
		return TRUE;
	}

	*new = init_subdir();

	for(n = 0; n < paths->count; n++) {
		struct pathname *path = paths->path[n];
		for(i = 0; i < path->names; i++) {
			int match = fnmatch(path->name[i].name,
				name, FNM_PATHNAME|FNM_PERIOD|FNM_EXTMATCH) ==
				0;
			if(match && path->name[i].paths == NULL)
				/*
				 * match on a leaf component, any subdirectories
				 * will implicitly match, therefore return an
				 * empty new search set
				 */
				goto empty_set;

			if(match)
				/*
				 * match on a non-leaf component, add any
				 * subdirectories to the new set of
				 * subdirectories to scan for this name
				 */
				*new = add_subdir(*new, path->name[i].paths);
		}
	}

	if((*new)->count == 0) {
		/*
		 * no matching names found, delete empty search set, and return
		 * FALSE
		 */
		free_subdir(*new);
		*new = NULL;
		return FALSE;
	}

	/*
	 * one or more matches with sub-directories found (no leaf matches),
	 * return new search set and return TRUE
	 */
	return TRUE;

empty_set:
	/*
	 * found matching leaf exclude, return empty search set and return TRUE
	 */
	free_subdir(*new);
	*new = NULL;
	return TRUE;
}

void pre_scan(char *parent_name, unsigned int start_block, unsigned int offset,
	struct pathnames *paths)
{
	unsigned int type;
	char *name, pathname[1024];
	struct pathnames *new;
	struct inode *i;
	struct dir *dir = squashfs_opendir(start_block, offset, &i);

	while(squashfs_readdir(dir, &name, &start_block, &offset, &type)) {
		struct inode *i;

		TRACE("pre_scan: name %s, start_block %d, offset %d, type %d\n",
			name, start_block, offset, type);

		if(!matches(paths, name, &new))
			continue;

		strcat(strcat(strcpy(pathname, parent_name), "/"), name);

		if(type == SQUASHFS_DIR_TYPE)
			pre_scan(parent_name, start_block, offset, new);
		else if(new == NULL) {
			if(type == SQUASHFS_FILE_TYPE) {
				i = read_inode(start_block, offset);
				if(created_inode[i->inode_number - 1] == NULL)
					created_inode[i->inode_number - 1] =
						(char *) i;
			}
		}

		free_subdir(new);
	}

	squashfs_closedir(dir);
}

void dir_scan(char *parent_name, unsigned int start_block, unsigned int offset,
	struct pathnames *paths)
{
	unsigned int type;
	char *name, pathname[1024];
	struct pathnames *new;
	struct inode *i;
	struct dir *dir = squashfs_opendir(start_block, offset, &i);

#ifdef TESTING
	if(mkdir(parent_name, (mode_t) dir->mode) == -1 && errno != EEXIST) {
		ERROR("dir_scan: failed to make directory %s, because %s\n",
			parent_name, strerror(errno));
		squashfs_closedir(dir);
		return;
	}
#endif

	while(squashfs_readdir(dir, &name, &start_block, &offset, &type)) {
		TRACE("dir_scan: name %s, start_block %d, offset %d, type %d\n",
			name, start_block, offset, type);


		if(!matches(paths, name, &new)) {
			TRACE("dir_scan: duplicate entry for name %s\n", name);
			continue;
		}

		strcat(strcat(strcpy(pathname, parent_name), "/"), name);

		if(type == SQUASHFS_DIR_TYPE)
			dir_scan(pathname, start_block, offset, new);
		else if(new == NULL) {
			i = read_inode(start_block, offset);
			if(i != NULL)
				create_inode(pathname, i);
		}

		free_subdir(new);
	}

	squashfs_closedir(dir);
	dir_count ++;
}

int read_super(const char *source)
{
	/*
	 * Try to read a Squashfs 4 superblock
	 */
	read_fs_bytes(fd, SQUASHFS_START, sizeof(struct squashfs_super_block),
		&sBlk);

	if(sBlk.s_magic == SQUASHFS_MAGIC && sBlk.s_major == 4 &&
			sBlk.s_minor == 0) {
		return TRUE;
	} else {
		ERROR("Can't find a SQUASHFS superblock on %s\n", source);
		return FALSE;
	}
}

struct pathname *process_extract_files(struct pathname *path, char *filename)
{
	FILE *fd;
	char name[16384];

	fd = fopen(filename, "r");
	if(fd == NULL)
		EXIT_UNSQUASH("Could not open %s, because %s\n", filename,
			strerror(errno));

	while(fscanf(fd, "%16384[^\n]\n", name) != EOF)
		path = add_path(path, name, name);

	fclose(fd);
	return path;
}

/*
 * reader thread.  This thread processes read requests queued by the
 * cache_get() routine.
 */
void *reader(void *arg)
{
	struct cache_entry *entry = (struct cache_entry *)arg;

	int res = read_fs_bytes(fd, entry->block,
		SQUASHFS_COMPRESSED_SIZE_BLOCK(entry->size),
		entry->data);

	if(res && SQUASHFS_COMPRESSED_BLOCK(entry->size))
		/*
		 * queue successfully read block to the deflate
		 * thread(s) for further processing
 		 */
		deflator(entry);

	return entry;
}

/*
 * writer thread.  This processes file write requests queued by the
 * write_file() routine.
 */
void *writer(void *arg)
{
	static struct squashfs_file *file = NULL;
	static long long hole = 0;
	struct file_entry *block;

	if (file == NULL) {
		file = (struct squashfs_file *)arg;
		return file;
	}

	TRACE("writer: regular file, blocks %d\n", file->blocks);

	block = (struct file_entry *)arg;

	if(block->buffer == 0) { /* sparse file */
		hole += block->size;
	} else {
		write_block(file->fd, block->buffer->data +
			block->offset, block->size, hole, file->sparse);
	}
	free(block);

	if (++cur_blocks == file->blocks) {
		close(file->fd);
		free(file->pathname);
		cur_blocks = 0;
		hole = 0;
		free(file);
		file = NULL;
	}
	return NULL;
}

/*
 * decompress thread.  This decompresses buffers queued by the read thread
 */
void *deflator(void *arg)
{
	struct cache_entry *entry = (struct cache_entry *)arg;
	char tmp[block_size];
	int error, res;

		res = squashfs_uncompress(tmp, entry->data,
			SQUASHFS_COMPRESSED_SIZE_BLOCK(entry->size), block_size,
			&error);

		if(res == -1)
			ERROR("uncompress failed with error code %d\n", error);
		else
			memcpy(entry->data, tmp, res);

		return entry;
}

#ifdef TESTING
int main(int argc, char *argv[])
#else
char *unsquashfs_single_file(const char *image_name, const char *file_name)
#endif
{
	char *dest = "squashfs-root";
	struct pathnames *paths = NULL;
	struct pathname *path = NULL;

#ifdef TESTING
	if(argc < 3)
		EXIT_UNSQUASH("SYNTAX: %s [options] filesystem "
			"[file to extract]\n", argv[0]);

	path = add_path(path, argv[2], argv[2]);

	if((fd = open(argv[1], O_RDONLY)) == -1)
		EXIT_UNSQUASH("Could not open %s, because %s\n", argv[1],
			strerror(errno));

	if(read_super(argv[1]) == FALSE)
		EXIT_UNSQUASH("Could not read superblock\n");
#else
	if((fd = open(image_name, O_RDONLY)) == -1)
		EXIT_UNSQUASH("Could not open %s, because %s\n", image_name,
			strerror(errno));

	if(read_super(image_name) == FALSE)
		EXIT_UNSQUASH("Could not read superblock\n");

	private_buffer = calloc(1, 4096);
	if(private_buffer == NULL)
		EXIT_UNSQUASH("Unable to allocate private buffer");

	path = add_path(path, file_name, file_name);
#endif

	if ((sBlk.compression != ZLIB_COMPRESSION) && (sBlk.compression != LZO_COMPRESSION))
		EXIT_UNSQUASH("No decompressors available:\n");

	block_size = sBlk.block_size;
	block_log = sBlk.block_log;

	fragment_cache = cache_init(block_size);
	data_cache = cache_init(block_size);

	created_inode = calloc(sBlk.inodes, sizeof(char *));
	if(created_inode == NULL)
		EXIT_UNSQUASH("failed to allocate created_inode\n");

	if(read_fragment_table() == FALSE)
		EXIT_UNSQUASH("failed to read fragment table\n");

	uncompress_inode_table(sBlk.inode_table_start,
		sBlk.directory_table_start);

	uncompress_directory_table(sBlk.directory_table_start,
		sBlk.fragment_table_start);

	if(path) {
		paths = init_subdir();
		paths = add_subdir(paths, path);
	}

	pre_scan(dest, SQUASHFS_INODE_BLK(sBlk.root_inode),
		SQUASHFS_INODE_OFFSET(sBlk.root_inode), paths);

	memset(created_inode, 0, sBlk.inodes * sizeof(char *));

	dir_scan(dest, SQUASHFS_INODE_BLK(sBlk.root_inode),
		SQUASHFS_INODE_OFFSET(sBlk.root_inode), paths);

#ifdef TESTING
	printf("\n");
	printf("created %d files\n", file_count);
	printf("created %d directories\n", dir_count);

	return 0;
#else
	return private_buffer;
#endif
}
