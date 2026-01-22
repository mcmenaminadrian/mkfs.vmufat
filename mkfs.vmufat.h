#ifndef MKFS_VMUFAT_H

#define K1 1024
static const int BLOCKSIZE = 512;
static const int STRICTSIZE = 128;
static const int BLOCKSHIFT = 9;
static const int RATIO_DIR_TO_USERBLOCKS = 16;
static const char VMUFAT_MAGIC[16] = "UUUUUUUUUUUUUUUU";
static const int VMUFAT_MAGIC_OFFSET = 0x10;
static const int BCDOFFSET = 0x30;
static const int SIZEOFFSET = 0x20;
static const int HIDDENFIRST = 200u;
static const int HIDDENSIZE = 31u;
static const int ROOTBLOCK_STRICT = 255;
static const int FATSTART_STRICT = 254;
static const int FATSIZE_STRICT = 4;
static const int DIRSTART_STRICT = 250;
static const int DIRSIZE_STRICT = 16;

struct vmudate {
    unsigned char century;
    unsigned char year;
    unsigned char month;
    unsigned char day;
    unsigned char hour;
    unsigned char minute;
    unsigned char second;
    unsigned char weekday;
};



#define FATFREE     0xFFFC
#define FATBAD      0xFFFF
#define FATEND      0xFFFA
#endif //MKFS_VMUFAT_H