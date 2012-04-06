/*
 * mkfs.vmufat.c - make a linux (VMUFAT) filesystem
 *
 * Copyright (c) 2012 Adrian McMenamin adrianmcmenamin@gmail.com
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

static const int BLOCKSIZE = 512;
static const int BLOCKSHIFT = 9;

struct vmuparam {
	unsigned int size;
	unsigned int rootblock;
	unsigned int fatstart;
	unsigned int fatsize;
	unsigned int dirstart;
	unsigned int dirsize;
};

struct badblocklist {
	int number;
	struct badblocklist *next;
};

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
	printf("Usage: mkfs.vmufat [-c|-l filename] [-N number-of-blocks]\n");
	printf("\t[-B log2-number-of-blocks] [-v] device [number-of-blocks]\n");
}

static int checkmount(const char *device_name)
{
	FILE *f;
	struct mntent *mnt;

	if ((f = setmntent(_PATH_MOUNTED, "r")) == NULL)
		return;
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

static int calculate_vmuparams(int device_numb, struct vmuparam *param,
	int blocknum, int verbose)
{
	int error = 0;
	off_t size;
	struct stat devstat;
	
	if (fstat(device_numb, &devstat) < 0) {
		printf("Could not stat device.\n");
		error = -1;
		goto out;
	}
	size = devstat.st_size;
	if ((size < BLOCKSIZE * 4) ||(blocknum > 0 && blocknum < 4)) {
		printf("Device just %lu octets in size. Too small for"
			" VMUFAT volume\n", size);
		error = -1;
		goto out;
	}
	else if (size < blocknum * BLOCKSIZE) {
		printf("Device only %lu octets in size. Too small for"
			" your request of %i blocks\n", size, blocknum);
		error = -1;
		goto out;
	}
	else if (blocknum)
		size = blocknum * BLOCKSIZE;
	param->size = _round_down(size >> BLOCKSHIFT) << BLOCKSHIFT;
	param->rootblock = (param->size >> BLOCKSHIFT) - 1;
	param->fatstart = param->rootblock - 1;
	param->fatsize = (2 * (param->size >> BLOCKSHIFT)) >> BLOCKSHIFT;
	param->dirstart = param->fatstart - param->fatsize;
	/* Remainder divided in ratio 16:1 between user blocks and directory */
	param->dirsize = ((param->size >> BLOCKSHIFT)
		- (1 + param->fatsize)) / 17;
	if (verbose) {
		printf("VMUFAT file system: Root block at %i\n",
			param->rootblock);
		printf("\tFAT of length %i begins at %i\n", param->fatsize,
			param->fatstart);
		printf("\tDirectory of length %i begins at %i\n",
			param->dirsize, param->dirstart);
	}
out:
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
		buf[i] = __cpu_to_le16(0xFFFC);

	if (param->fatsize > 1) {
		for (i = param->fatstart - 1;
			i > param->fatstart - param->fatsize; i--) {
			if (pwrite(device_numb, buffer, BLOCKSIZE,
				i * BLOCKSIZE) < BLOCKSIZE)
				goto badwrite;
		}
	}

	/* Mark for the FAT and Directory */
	start = 2 * (param->fatsize + param->dirsize) /  BLOCKSIZE + 1;
	for (j = param->rootblock - start; j < param->rootblock; j++) {
		k = (j - param->dirstart - 1) * BLOCKSIZE;
		for (i = 0; i < BLOCKSIZE; i = i + 2) {
			/* FAT */
			if ((k + i) / 2 > 1 + param->fatstart - param->fatsize)
				buf[i / 2] = __cpu_to_le16((k + i) / 2 - 1);
			else if ((k + i) / 2 == 1 + param->fatstart
				- param->fatsize)
				buf[i / 2] = __cpu_to_le16(0xFFFA);
			/* Directory */
			else if ((k + i) / 2 >  1 + param->dirstart 
				- param->dirsize)
				buf[i / 2] = __cpu_to_le16((k + i) / 2 - 1);
			else if ((k + i) / 2 == 1 + param->dirstart
				- param->dirsize)
				buf[i / 2] = __cpu_to_le16(0xFFFA);
		}
		if (start > 1) {
			if (pwrite(device_numb, buffer, BLOCKSIZE, j * BLOCKSIZE) < BLOCKSIZE)
				goto badwrite;
		}
	}
	buf[BLOCKSIZE / 2 - 1] = __cpu_to_le16(0xFFFA); /*Root block*/
	if (pwrite(device_numb, buffer, BLOCKSIZE,
		(j - 1) * BLOCKSIZE) < BLOCKSIZE)
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
		

static void _fill_root_block(char *buf, const struct vmuparam *param)
{
	int i;
	time_t rawtime;
	struct tm *ptm;
	char century, year, month, day, hour, minute, second, weekday;
	uint16_t *wordbuf;

	wordbuf = (uint16_t *)buf;

	for (i = 0; i < 0x10; i++)
		buf[i] = 0x55;

	time(&rawtime);
	ptm = gmtime(&rawtime);
	if (!ptm)
		return;
	century = _i2bcd(19 + ptm->tm_year / 100);
	year = _i2bcd(ptm->tm_year - (ptm->tm_year /100) * 100);
	month = _i2bcd(ptm->tm_mon + 1);
	day = _i2bcd(ptm->tm_mday);
	hour = _i2bcd(ptm->tm_hour);
	minute = _i2bcd(ptm->tm_min);
	second = _i2bcd(ptm->tm_sec);
	weekday = _i2bcd(ptm->tm_wday);

	buf[0x30] = century;
	buf[0x31] = year;
	buf[0x32] = month;
	buf[0x33] = day;
	buf[0x34] = hour;
	buf[0x35] = minute;
	buf[0x36] = second;
	buf[0x37] = weekday;

	wordbuf[0x20] = __cpu_to_le16(param->rootblock);
	wordbuf[0x22] = __cpu_to_le16(param->rootblock);
	wordbuf[0x23] = __cpu_to_le16(param->fatstart);
	wordbuf[0x24] = __cpu_to_le16(param->fatsize);
	wordbuf[0x25] = __cpu_to_le16(param->dirstart);
	wordbuf[0x26] = __cpu_to_le16(param->dirsize);
	/* 32 octets per directory entry */
	wordbuf[0x27] = __cpu_to_le16(param->dirsize * 8);
}	

static int mark_root_block(int device_numb, const struct vmuparam *param, int verbose)
{
	char *zilches;
	int i, error = 0;
	zilches = malloc(BLOCKSIZE);
	if (!zilches) {
		printf("Memory allocation failed.\n");
		error = -1;
		goto out;
	}

	memset(zilches, '\0', BLOCKSIZE);
	_fill_root_block(zilches, param);
	if (pwrite(device_numb, zilches, BLOCKSIZE,
		param->rootblock * BLOCKSIZE) < BLOCKSIZE) {
		printf("Could not write root block\n");
		error = -1;
		goto clean;
	}
	if (verbose) {
		printf("Root block written to block %i\n", param->rootblock);
		printf("BCD string: %c %c %c %c %c %c %c %c\n",
			zilches[0x30], zilches[0x31], zilches[0x32],
			zilches[0x33], zilches[0x34], zilches[0x35],
			zilches[0x36], zilches[0x37]);
	}
	
clean:
	free(zilches);
out:
	return error;
}

static int zero_blocks(int device_numb, const struct vmuparam *param, int verbose)
{
	char *zilches;
	int i, error = -1;
	zilches = malloc(BLOCKSIZE);
	if (!zilches) {
		printf("Memory allocation failed.\n");
		goto out;
	}

	memset(zilches, '\0', BLOCKSIZE);

	for (i = 0; i <= param->dirstart; i++) {
		if (pwrite(device_numb, zilches, BLOCKSIZE,
			i * BLOCKSIZE) < BLOCKSIZE) {
			printf("Write failed on device\n");
			goto clean;
		}
	}
	error = 0;
	if (verbose)
		printf("Other blocks zeroed\n");
clean:
	free(zilches);
out:
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
		goto out;
	}
	
	if (fstat(device_numb, &devstat) < 0) {
		printf("Could not stat device.\n");
		error = -1;
		goto clean;
	}

	size = devstat.st_size;

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
				goto clean;
			}
			else if (*root == NULL)
				*root = lastbadblock;
		}
	}
clean:
	free(buffer);
out:
	return error;
}

static int _mark_block_bad(int device_numb, int badblock,
	const struct vmuparam *param)
{
	int error = -1;
	int fatblock;
	char *buffer;
	uint16_t *buf;
	buffer = malloc(BLOCKSIZE);
	if (!buffer) {
		printf("Memory allocation failed\n");
		goto out;
	}

	fatblock = (2 * badblock) / BLOCKSIZE + param->dirstart + 1;
	if (pread(device_numb, buffer, BLOCKSIZE, fatblock * BLOCKSIZE) != BLOCKSIZE)
		goto clean;
	buf = (uint16_t *)buffer;
	buf[badblock % (BLOCKSIZE / 2)] = __cpu_to_le16(0xFFFA);
	if (pwrite(device_numb, buffer, BLOCKSIZE,
		fatblock * BLOCKSIZE) != BLOCKSIZE)
		goto clean;
	error = 0;
clean:
	free(buffer);
out:
	return error;

}

static int mark_bad_blocks(int device_numb, struct badblocklist *root, 
	const struct vmuparam *param, int verbose)
{
	int error = 0;
	int nobadblock;
	struct badblocklist *next;
	

	if (!root)
		goto out;

	next = root;
	while (next) {
		nobadblock = next->number;
		if (nobadblock < 0 || nobadblock > param->rootblock)
			goto advance;
		if (nobadblock >= param->dirstart
			&& nobadblock <= param->rootblock) {
			printf("Format fails as system block is bad\n");
			error = -1;
			goto out;
		}
		if (_mark_block_bad(device_numb, nobadblock, param)
			< 0) {
			printf("Format fails as cannot mark FAT"
				" for bad block.\n");
			error = -1;
			goto out;
		}
advance:
		next = next->next;
	}

	if (verbose)
		printf("Bad blocks now marked off in FAT.\n");

out:
	return error;
}	

int main(int argc, char* argv[])
{
	int blocknum = 0;
	int i;
	int verbose = 0, scanbadblocks = 0, useblocklist = 0;
	int error = 1, device_numb;
	char *blocklistfnm = NULL;
	char *device_name = NULL;
	struct stat statbuf;
	struct badblocklist* lstbadblocks = NULL;
	struct vmuparam params;

	if (argc < 2) {
		usage();
		goto out;
	}

	opterr = 0;
	while ((i = getopt(argc, argv, "cl:N:B:v")) != -1)
		switch (i) {
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
		default:
			usage();
			goto out;
		}
	argc -= optind;
	argv += optind;
	if (!argc > 0) {
		usage();
		goto out;
	}
	device_name = argv[0];
	argc--;
	argv++;

	if (argc) {
		blocknum = atoi(argv[0]);
		if (argc > 1)
			usage();
	}
	if (checkmount(device_name) < 0)
		goto out;

	if (stat(device_name, &statbuf) < 0) {
		printf("Cannot get status of %s\n", device_name);
		goto out;
	}
	if (!S_ISBLK(statbuf.st_mode)) {
		printf("%s must be a block device\n", device_name);
		goto out;
	}
	device_numb = open(device_name, O_RDWR|O_EXCL);
	if (device_numb < 0) {
		printf("Attempting to open %s fails with error %i\n",
			device_name, device_numb);
		goto out;
	}
	
	if (scanbadblocks > 0) {
		if (scanforbad(device_numb, &lstbadblocks, verbose) < 0)
			goto close;
	}
	else if (useblocklist > 0) {
		if (readforbad(&lstbadblocks, blocklistfnm, verbose) < 0)
			goto close;
	}

	if (calculate_vmuparams(device_numb, &params, blocknum, verbose) < 0)
		goto close;

	if (mark_root_block(device_numb, &params, verbose) < 0)
		goto close;

	if (mark_fat(device_numb, &params, verbose) < 0)
		goto close;

	if (zero_blocks(device_numb, &params, verbose) < 0)
		goto close;

	if (mark_bad_blocks(device_numb, lstbadblocks, &params, verbose) < 0)
		goto close;

	if (verbose)
		printf("VMUFAT volume created on %s\n", device_name);
		
	error = 0;
close:
	close(device_numb);
	if (lstbadblocks)
		clean_blocklist(lstbadblocks);
out:
	return error;
}
