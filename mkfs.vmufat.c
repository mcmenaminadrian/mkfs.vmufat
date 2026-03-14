/*
 * mkfs.vmufat.c - make a linux (VMUFAT) filesystem
 *
 * Copyright (c) 2012, 2026 Adrian McMenamin adrianmcmenamin@gmail.com
 * Licensed under Version 2 of the GNU General Public Licence
 *
 * Parts shamelessly copied from other mkfs code
 * copyright Linus Torvalds and others
 */

#include <stdio.h>
#include <stdlib.h>
#include <asm/byteorder.h>
#include <fcntl.h>
#include <mntent.h>
#include <paths.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "mkfs.vmufat.h"

static struct badblocklist* _add_badblock(struct badblocklist *root, int block)
{
	struct badblocklist *nextone;

	nextone = malloc(sizeof(struct badblocklist));
	if (!nextone)
		return NULL;
	nextone->next = NULL;
	nextone->number = block;

	if (root)
		root->next = nextone;
	return nextone;
}

static void clean_blocklist(struct badblocklist *nextblock)
{
	if (!nextblock)
		return;
	clean_blocklist(nextblock->next);
	free(nextblock);
}

static void usage(void)
{
	printf("Create a VMUFAT filesystem.\n");
	printf("Usage: mkfs.vmufat [-s|-c|-l filename] [-N number-of-blocks]\n");
	printf("\t[-B log2-number-of-blocks] [-v] [-f]\n");
	printf("[-L label]");
	printf(" device [number-of-blocks]\n");
	printf("==============================\n");
	printf("-s applies strict compatibility with SEGA VMU hardware (hidden blocks).\n");
	printf("-c scans for bad blocks on device.\n");
	printf("-l filename reads bad block list from filename.\n");
	printf("-v verbose output.\n");
}

static int checkmount(const char *device_name)
{
	FILE *f;
	struct mntent *mnt;

	if ((f = setmntent(_PATH_MOUNTED, "r")) == NULL) {
		printf("Failure. Cannot open file system description file for reading.\n");
		return -1;
	}
	while ((mnt = getmntent(f)) != NULL)
		if (strcmp(device_name, mnt->mnt_fsname) == 0)
			break;
	endmntent(f);
	if (!mnt)
		return 0;
	printf("%s already mounted - will not format as VMUFAT\n", device_name);
	return -1;
}

static int readforbad(struct badblocklist** root, const char* filename, int verbose)
{
	int error = 0;
	FILE *listfile;
	int badblocks = 0;
	unsigned long blockno;
	struct badblocklist *lastbadblock = NULL;

	listfile = fopen(filename, "r");
	if (listfile == NULL) {
		printf("Could not open %s\n", filename);
		error = -1;
		goto out;
	}

	while (!feof(listfile)) {
		if (fscanf(listfile, "%lu\n", &blockno) != 1) {
			printf("Cannot parse %s at line %i\n", filename,
				badblocks + 1);
			error = -1;
			goto close;
		}
		lastbadblock = _add_badblock(lastbadblock, blockno);
		if (!lastbadblock) {
			printf("Memory failure\n");
			error = -1;
			goto close;
		}
		else if (*root == NULL)
			*root = lastbadblock;
		if (verbose)
			printf("Bad block at %lu noted.\n", blockno);
		badblocks++;
	}	

close:
	fclose(listfile);
out:
	return error;
}

static unsigned int _round_down(unsigned int x)
{
	unsigned int y = 0x80000000;
	while (y > x)
		y = y >> 1;
	return y;
}

static void _set_vmuparams(struct vmuparam *param, off_t size)
{
	param->size = _round_down(size >> BLOCKSHIFT) << BLOCKSHIFT;
	param->rootblock = (param->size >> BLOCKSHIFT) - 1;
	param->fatsize = (2 * (param->size >> BLOCKSHIFT)) >> BLOCKSHIFT;
	if (param->fatsize < 1)
		param->fatsize = 1;
	param->fatstart = param->rootblock - param->fatsize;
	/* Remainder divided in ratio 16:1 between user blocks and directory */
	param->dirsize = ((param->size >> BLOCKSHIFT)
		- (1u + param->fatsize)) / (RATIO_DIR_TO_USERBLOCKS + 1u);
	if (param->dirsize < 1)
		param->dirsize = 1;
	param->dirstart = param->fatstart - param->dirsize;
}

static void _set_vmuparams_strict(struct vmuparam *param)
{
	/* For strict VMUFAT, we have fixed sizes */
	param->size = STRICTSIZE; /* 128K VMU size */
	param->rootblock = ROOTBLOCK_STRICT;
	param->fatstart = FATSTART_STRICT;
	param->fatsize = FATSIZE_STRICT;
	param->dirstart = DIRSTART_STRICT;
	param->dirsize = DIRSIZE_STRICT;
}

static int calculate_vmuparams(int device_numb, struct vmuparam *param,
	int blocknum, int strict_compat, int verbose)
{
	int error = 0;
	off_t size;
	struct stat devstat;

	size = lseek(device_numb, 0, SEEK_END);
	if (strict_compat)
	{
		if (size > STRICTSIZE) {
			size = STRICTSIZE; /* 128K VMU size */
			if (verbose)
				printf("Using strict VMUFAT size of 128Kb\n");
		}
		else if (size < STRICTSIZE) {
			printf("Device only %lu octets in size. Too small for"
				" strict VMUFAT volume\n", size);	
			error = -1;
		}
	}
	else
	{
		if ((size < BLOCKSIZE * 4) ||(blocknum > 0 && blocknum < 4)) {
		printf("Device just %lu octets in size. Too small for"
			" VMUFAT volume\n", size);
		error = -1;
		}
		else if (size < blocknum * BLOCKSIZE) {
			printf("Device only %lu octets in size. Too small for"
				" your request of %i blocks\n", size, blocknum);
			error = -1;
		}
		if (blocknum) {
			size = blocknum * BLOCKSIZE;
		}
	}

	if (error == 0) {
		if (strict_compat)
			_set_vmuparams_strict(param);
		else
			_set_vmuparams(param, size);
		if (verbose) {
			printf("VMUFAT file system: Root block at %i\n",
				param->rootblock);
			printf("\tFAT of length %i begins at %i\n", param->fatsize,
				param->fatstart);
			printf("\tDirectory of length %i begins at %i\n",
				param->dirsize, param->dirstart);
		}
	}
	return error;
}

static char _i2bcd(unsigned int i)
{
	char bcd;
	unsigned int digit;
	digit = i/10;
	bcd = digit << 4;
	digit = i % 10;
	bcd += digit;
	return bcd;
}

/* Write out a totally empty FAT section as default*/
static int _mark_fat_fatsize(int device_numb, const struct vmuparam *param,
	const char* buffer)
{
	int error = 0;
	for (uint i = param->fatstart;
		i < (param->fatstart + param->fatsize); i++) {
		if (pwrite(device_numb, buffer, BLOCKSIZE,
			i * BLOCKSIZE) < BLOCKSIZE) {
				error = -1;
				break;
			}
	}
	return error;
}

/* mark the superblock in FAT */
static int mark_root_block_in_fat(const int device_numb, const struct vmuparam *param,
	uint16_t *buf)
{
	int error = 0;
	int rootblock = (param->rootblock)%BLOCKSIZE;
	buf[rootblock] = __cpu_to_le16(FATEND);
	if (pwrite(device_numb, (char *)buf, BLOCKSIZE,
		(param->fatstart + param->fatsize - 1) * BLOCKSIZE)
		< BLOCKSIZE) {
		error = -1;
	}
	return error;
}


static void clean_buf(uint16_t* buf)
{
	for (int i = 0; i < 128; i++)
	{
		buf[i] = __cpu_to_le16(FATFREE);
	}
}

static int mark_massive_fat_in_fat(const int device_numb, const struct vmuparam *param,
	uint16_t *buf)
{
	int error = 0;
	for (int i = 0; i < 127; i++)
	{
		buf[i] = __cpu_to_le16(i + 1);
	}
	if (pwrite(device_numb, (char *)buf, BLOCKSIZE,
			param->fatstart * BLOCKSIZE) < BLOCKSIZE) {
		error = -1;
	}
	if (error == 0) {
		int top_of_fat = (param->fatstart - param->fatsize + 1);
		int fat_blocks = param->fatsize / 128 - 1;
		int first_fat_block = top_of_fat % 128;
		clean_buf(buf);
		for (int i = 0; i < fat_blocks; i++)
		{
			for (int j = first_fat_block; j < 128; j++)
			{
				int next_block = j + 1 + top_of_fat + (i * 128);
				buf[j] = __cpu_to_le16(next_block);
			}
			first_fat_block = 0;
			if (pwrite(device_numb, (char *)buf, BLOCKSIZE,
			(top_of_fat + i) * BLOCKSIZE) < BLOCKSIZE) {
				error = -1;
				break;
			}
		}
	}
}

/* Handle FAT bigger than one block */
static int mark_big_fat_in_fat(const int device_numb, const struct vmuparam *param,
	uint16_t *buf)
{
	int error = 0;
	int i, j;
	if (param->fatsize > 128) {
		return mark_massive_fat_in_fat(device_numb, param, buf);
	} else {
		int top_of_fat = param->fatstart + param->fatsize;
		for (i = param->fatstart; i < top_of_fat; i++)
		{
			for (int j = 0; j < 128; j++)
			{
				buf[j] = __cpu_to_le16(i + j + 1);
			}
			if (pwrite(device_numb, (char *)buf, BLOCKSIZE,
				i * BLOCKSIZE) < BLOCKSIZE) {
				error = -1;
			}

		}
		buf[j - 2] = __cpu_to_le16(FATEND);
		buf[j - 1] = __cpu_to_le16(FATEND);
	}
	return error;
}

/* mark out the FAT itself in FAT */
static int mark_fat_in_fat(const int device_numb, const struct vmuparam *param,
	uint16_t *buf)
{
	int error = 0;
	int i;
	if (param->fatsize > 1) {
		error = mark_big_fat_in_fat(device_numb, param, buf);
	} else {
		/* most FATs will be single block */
		for (i = param->fatstart; i < (param->fatstart + param->fatsize - 1); i++) {
			buf[i] = __cpu_to_le16(i);
		}
		buf[i] = __cpu_to_le16(FATEND);
		buf[i + 1] = __cpu_to_le16(FATEND);
		if (pwrite(device_numb, (char *)buf, BLOCKSIZE,
			param->fatstart * BLOCKSIZE) < BLOCKSIZE) {
			error = -1;
		}
	}
	return error;
}

static int _mark_fat(int device_numb, const struct vmuparam *param,
	uint16_t *buf)
{
	return mark_fat_in_fat(device_numb, param, buf);
}


static int mark_fat(int device_numb, const struct vmuparam *param, int verbose)
{
	char *buffer;
	uint16_t *buf;
	int i, j, k, start;
	int error = -1;
	
	buffer = malloc(BLOCKSIZE);
	if (!buffer)
		goto nomem;
	
	buf = (uint16_t *)buffer;
	for (i = 0; i < BLOCKSIZE / 2; i++)
		buf[i] = __cpu_to_le16(FATFREE);

	error = _mark_fat_fatsize(device_numb, param, buffer);
	if (error < 0)
		goto badwrite;

	error = _mark_fat(device_numb, param, buf);
	if (error < 0)
		goto badwrite;

	if (verbose)
		printf("FAT written\n");
	error = 0;
	goto clean;

nomem:
	printf("Memory allocation failed.\n");		
	goto out;
badwrite:
	printf("FAT write failed.\n");
clean:
	free(buffer);
out:
	return error;
}
		
static struct vmudate _get_current_vmudate(void)
{
	time_t rawtime;
	struct tm *ptm;
	struct vmudate vmudate;

	time(&rawtime);
	ptm = gmtime(&rawtime);
	if (!ptm) {
		vmudate.century = 0;
		vmudate.year = 0;
		vmudate.month = 0;
		vmudate.day = 0;
		vmudate.hour = 0;
		vmudate.minute = 0;
		vmudate.second = 0;
		vmudate.weekday = 0;
	} else {
		vmudate.century = _i2bcd(19 + ptm->tm_year / 100u);
		vmudate.year = _i2bcd(ptm->tm_year - (ptm->tm_year /100u) * 100u);
		vmudate.month = _i2bcd(ptm->tm_mon + 1u);
		vmudate.day = _i2bcd(ptm->tm_mday);
		vmudate.hour = _i2bcd(ptm->tm_hour);
		vmudate.minute = _i2bcd(ptm->tm_min);
		vmudate.second = _i2bcd(ptm->tm_sec);
		// VMU week day counts from Monday and not Sunday
		vmudate.weekday = _i2bcd((ptm->tm_wday + 6u)%7u);
	}

	return vmudate;
}

static void _set_vmudate(struct vmudate *vmudate, unsigned char *buf)
{
	buf[0] = vmudate->century;
	buf[1] = vmudate->year;
	buf[2] = vmudate->month;
	buf[3] = vmudate->day;
	buf[4] = vmudate->hour;
	buf[5] = vmudate->minute;
	buf[6] = vmudate->second;
	buf[7] = vmudate->weekday;
}

static void _put_vmuparams(const struct vmuparam *param, uint16_t *wordbuf,
	int strict_compat)
{
	wordbuf[0] = __cpu_to_le16(param->size >> BLOCKSHIFT);
	wordbuf[2] = __cpu_to_le16(param->rootblock);
	wordbuf[3] = __cpu_to_le16(param->fatstart);
	wordbuf[4] = __cpu_to_le16(param->fatsize);
	wordbuf[5] = __cpu_to_le16(param->dirstart);
	wordbuf[6] = __cpu_to_le16(param->dirsize);
	if (strict_compat) {
		wordbuf[8] = __cpu_to_le16(HIDDENFIRST);
		wordbuf[9] = __cpu_to_le16(HIDDENSIZE);
	}
}	

static void _fill_root_block(unsigned char *buffer,
	const struct vmuparam *param, int strict_compat)
{
	int i;
	time_t rawtime;
	struct tm *ptm;
	struct vmudate vmudate;

	uint16_t *wordbuf;

	wordbuf = (uint16_t *)buffer;
	memcpy(buffer, VMUFAT_MAGIC, 16u);
	vmudate = _get_current_vmudate();
	_set_vmudate(&vmudate, &buffer[BCDOFFSET]);
	_put_vmuparams(param, (uint16_t *)(&buffer[SIZEOFFSET]), strict_compat);
}

static int mark_root_block(int device_numb, const struct vmuparam *param,
	int strict_compat, int verbose)
{
	unsigned char *zilches;
	int i, error = 0;
	zilches = malloc(BLOCKSIZE);
	if (!zilches) {
		printf("Memory allocation failed.\n");
		error = -1;
	}
	else {
		memset(zilches, '\0', BLOCKSIZE);
		_fill_root_block(zilches, param, strict_compat);
		if (pwrite(device_numb, zilches, BLOCKSIZE,
			param->rootblock * BLOCKSIZE) < BLOCKSIZE)
		{
			printf("Could not write root block\n");
			error = -1;
		}
		else if (verbose) {
			printf("Root block written to block %i\n", param->rootblock);
			printf("BCD string: %c %c %c %c %c %c %c %c\n",
				zilches[BCDOFFSET], zilches[BCDOFFSET + 1], zilches[BCDOFFSET + 2],
				zilches[BCDOFFSET + 3], zilches[BCDOFFSET + 4],
				zilches[BCDOFFSET + 5], zilches[BCDOFFSET + 6],
				zilches[BCDOFFSET + 7]);
		}
		free(zilches);
	}
	return error;
}

static int zero_blocks(int device_numb, const struct vmuparam *param, int verbose)
{
	unsigned char *zilches;
	int i, error = -1;
	zilches = malloc(BLOCKSIZE);
	if (!zilches) {
		printf("Memory allocation failed.\n");
	} else {
		memset(zilches, '\0', BLOCKSIZE);
		for (i = 0u; i <= param->dirstart; i++) {
			error = 0;
			if (pwrite(device_numb, zilches, BLOCKSIZE,
				i * BLOCKSIZE) < BLOCKSIZE) {
				printf("Write failed on device\n");
				error = -1;
				break;
			}
		}
		if (verbose && error == 0)
			printf("Other blocks zeroed\n");
		free(zilches);
	}
	return error;
}
		

static int scanforbad(int device_numb, struct badblocklist** root, int verbose)
{
	int error = 0, i;
	struct badblocklist *lastbadblock = NULL;
	off_t size;
	long got;
	struct stat devstat; 
	char *buffer = malloc(BLOCKSIZE);
	if (!buffer) {
		error = -1;
		printf("Memory allocation failed.\n");
	} else {
	
		size = lseek(device_numb, 0, SEEK_END);

		for (i = 0; i < size/BLOCKSIZE; i++)
		{
			if (verbose > 0)
				printf("Testing block %i\n", i);
			got = pread(device_numb, buffer, BLOCKSIZE, i * BLOCKSIZE);
			if (got != BLOCKSIZE) {
				printf("Block %i gives bad read\n", i);
				lastbadblock = _add_badblock(lastbadblock, i);
				if (!lastbadblock) {
					printf("Memory failure\n");
					error = -1;
					break;
				}
			}
			else if (*root == NULL)
				*root = lastbadblock;
		}
		free(buffer);
	}
	return error;
}

static int _mark_block_bad(int device_numb, int badblock,
	const struct vmuparam *param)
{
	int error = -1;
	int fatblock;
	char *buffer = NULL;
	uint16_t *buf;
	buffer = malloc(BLOCKSIZE);
	if (!buffer) {
		printf("Memory allocation failed\n");
	}
	fatblock = (2 * badblock) / BLOCKSIZE + param->dirstart + 1;
	if (buffer && pread(device_numb, buffer, BLOCKSIZE, fatblock * BLOCKSIZE)
		== BLOCKSIZE) {
		buf = (uint16_t *)buffer;
		buf[badblock % (BLOCKSIZE / 2)] = __cpu_to_le16(FATBAD);
		if (pwrite(device_numb, buffer, BLOCKSIZE,
			fatblock * BLOCKSIZE) == BLOCKSIZE)
			error = 0;
	}
	free(buffer);
	return error;

}

static int mark_bad_blocks(int device_numb, struct badblocklist *root, 
	const struct vmuparam *param, int verbose)
{
	int error = 0;
	int nobadblock;
	struct badblocklist *next;

	if (root) {
		next = root;
		while (next) {
			nobadblock = next->number;
			if (nobadblock < 0 || nobadblock > param->rootblock) {
				next = next->next;
				continue;
			}
			if (nobadblock >= param->dirstart
				&& nobadblock <= param->rootblock) {
				printf("Format fails as system block is bad\n");
				error = -1;
				break;
			}
			if (_mark_block_bad(device_numb, nobadblock, param)
				< 0) {
				printf("Format fails as cannot mark FAT"
					" for bad block.\n");
				error = -1;
				break;
			}
			next = next->next;
		}
	}
	if (verbose && error == 0)
		printf("Bad blocks now marked off in FAT.\n");
	return error;
}

static int verifydevice(const char *device_name)
{
	int device_numb = -1;
	if (checkmount(device_name) == 0) {
		device_numb = open(device_name, O_RDWR);
		if (device_numb < 0) {
			printf("Attempting to open %s fails with error %i\n",
				device_name, device_numb);
		}
	}
	return device_numb;
}

static int verifyblock(char *device_name)
{
	int error = -1;
	struct stat statbuf;
	if (stat(device_name, &statbuf) < 0) {
		printf("Cannot get status of %s\n", device_name);
	}
	else {
		if (!S_ISBLK(statbuf.st_mode)) {
			printf("%s must be a block device\n", device_name);
		}			
		else {
			error = 0;
		}
	}
	return error;
}

int main(int argc, char* argv[])
{
	int blocknum = 0;
	int i;
	int strict_compat = 0;
	int verbose = 0, scanbadblocks = 0, useblocklist = 0, allowfile = 0;
	int error = 0, device_numb;
	char *blocklistfnm = NULL;
	char *device_name = NULL;
	char *label = NULL;
	struct badblocklist* lstbadblocks = NULL;
	struct vmuparam params;

	if (argc < 2) {
		usage();
		return error;
	}

	opterr = 0;
	while ((i = getopt(argc, argv, "scl:N:B:L:vf")) != -1)
		switch (i) {
		case 's':
			strict_compat = 1;
			break;
		case 'c':
			scanbadblocks = 1;
			break;
		case 'l':
			useblocklist = 1;
			blocklistfnm = optarg;
			break;
		case 'N':
			blocknum = atoi(optarg);
			break;
		case 'B':
			blocknum = 1 << atoi(optarg);
			break;
		case 'v':
			verbose = 1;
			break;
		case 'f':
			allowfile = 1;
			break;
		case 'L':
			label = optarg;
			/* Label handling not implemented yet */
			printf("Label option not implemented yet.\n");
			usage();
			return error;
		default:
			usage();
			return error;
		}
	argc -= optind;
	argv += optind;
	if (!argc > 0) {
		usage();
		return error;
	}
	device_name = argv[0];
	argc--;
	argv++;

	if (argc) {
		blocknum = atoi(argv[0]);
		if (argc > 1)
			usage();
	}

	device_numb = verifydevice(device_name);
	if (device_numb >= 0)
	{
		error = verifyblock(device_name);
		if (error == 0) {
			if (scanbadblocks > 0) {
				error = scanforbad(device_numb, &lstbadblocks, verbose);
			} else if (useblocklist > 0) {
				error = readforbad(&lstbadblocks, blocklistfnm, verbose);
			}
		}
	} else {
		error = -1;
	}

	if (error == 0) {
		error = calculate_vmuparams(device_numb, &params, blocknum,
			strict_compat, verbose);
		if (error == 0) {
			error = mark_root_block(device_numb, &params, strict_compat,
				verbose);
		}
		if (error == 0) {
			error = mark_fat(device_numb, &params, verbose);
		}
		if (error == 0) {
			error = zero_blocks(device_numb, &params, verbose);
		}
		if (error == 0) {
			error = mark_bad_blocks(device_numb, lstbadblocks, &params, verbose);
		}
		close(device_numb);
	}
	if (error == 0 && verbose)
		printf("VMUFAT volume created on %s\n", device_name);

	if (lstbadblocks)
		clean_blocklist(lstbadblocks);
	
	return error;
}
