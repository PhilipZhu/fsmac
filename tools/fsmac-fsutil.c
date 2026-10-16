/*
 * fsmac_ext.util - Disk Arbitration probe tool for ext2/3/4.
 *
 * Installed inside /Library/Filesystems/fsmac_ext.fs so that macOS
 * recognises Linux volumes instead of offering to erase them ("The disk you
 * attached was not readable by this computer"). FSMac's helper then mounts
 * them. The tool only reads the superblock; it is run as root by
 * diskarbitrationd on untrusted media, so it is deliberately tiny.
 *
 *   fsmac_ext.util -p <disk> [removable|fixed] [readonly|writable]
 *       prints the volume name, exits FSUR_RECOGNIZED or FSUR_UNRECOGNIZED
 *   fsmac_ext.util -k <disk>
 *       prints the volume UUID, exits FSUR_IO_SUCCESS
 *
 * Installed a second time as mount_fsmac_ext: macOS itself cannot mount
 * these volumes, so that entry point only explains why.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* <sys/loadable_fs.h> */
#define FSUR_RECOGNIZED   (-1)
#define FSUR_UNRECOGNIZED (-3)
#define FSUR_IO_SUCCESS   (-4)
#define FSUR_IO_FAIL      (-5)
#define FSUR_INVAL        (-7)

#define EXT_MAGIC 0xEF53

static int read_superblock(const char *disk, uint8_t sb[1024])
{
	char path[256];
	uint8_t buf[4096];
	int fd;

	/* Accept "disk4s1", "/dev/disk4s1" and "/dev/rdisk4s1". */
	if (!strncmp(disk, "/dev/", 5))
		disk += 5;
	if (!strncmp(disk, "rdisk", 5))
		disk += 1;
	if (strncmp(disk, "disk", 4) || strchr(disk, '/') || strlen(disk) > 64)
		return FSUR_INVAL;
	snprintf(path, sizeof(path), "/dev/r%s", disk);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return FSUR_IO_FAIL;
	/* Raw devices need whole-sector reads; 4096 covers all sector sizes. */
	if (pread(fd, buf, sizeof(buf), 0) != (ssize_t)sizeof(buf)) {
		close(fd);
		return FSUR_IO_FAIL;
	}
	close(fd);
	memcpy(sb, buf + 1024, 1024);
	return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int is_ext(const uint8_t *sb)
{
	uint32_t log_bsize = le32(sb + 0x18);

	return le16(sb + 0x38) == EXT_MAGIC && log_bsize <= 6 &&
	       le32(sb + 0x20) != 0;         /* blocks per group */
}

static void print_label(const uint8_t *sb)
{
	char label[17];
	size_t n;

	memcpy(label, sb + 0x78, 16);
	label[16] = 0;
	n = strlen(label);
	/* Keep printable bytes only; the label is untrusted. */
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)label[i] < 0x20 || label[i] == 0x7f)
			label[i] = '_';
	fputs(n ? label : "Linux", stdout);
}

static int usage(void)
{
	fprintf(stderr, "usage: fsmac_ext.util -p|-k disk [removable|fixed] [readonly|writable]\n");
	return FSUR_INVAL;
}

int main(int argc, char **argv)
{
	const char *self = strrchr(argv[0], '/');
	uint8_t sb[1024];
	int rc;

	self = self ? self + 1 : argv[0];
	if (!strcmp(self, "mount_fsmac_ext")) {
		fprintf(stderr, "mount_fsmac_ext: Linux volumes are mounted by FSMac; "
			"open FSMac.app if the volume does not appear.\n");
		return 69; /* EX_UNAVAILABLE */
	}
	if (argc < 3 || argv[1][0] != '-')
		return usage();

	switch (argv[1][1]) {
	case 'p':
		rc = read_superblock(argv[2], sb);
		if (rc)
			return rc == FSUR_INVAL ? rc : FSUR_UNRECOGNIZED;
		if (!is_ext(sb))
			return FSUR_UNRECOGNIZED;
		print_label(sb);
		return FSUR_RECOGNIZED;
	case 'k': {
		const uint8_t *u;

		rc = read_superblock(argv[2], sb);
		if (rc)
			return rc;
		if (!is_ext(sb))
			return FSUR_UNRECOGNIZED;
		u = sb + 0x68;
		printf("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
		       u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9],
		       u[10], u[11], u[12], u[13], u[14], u[15]);
		return FSUR_IO_SUCCESS;
	}
	default:
		return usage();
	}
}
