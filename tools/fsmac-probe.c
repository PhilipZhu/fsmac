/*
 * fsmac-probe: identifies the filesystem on a device or image and prints a
 * one-line JSON description, e.g.
 *
 *   {"type":"ext4","label":"data","uuid":"...","clean":true,...}
 *
 * Exit status: 0 recognised, 1 not recognised, 2 error.
 * The label of NTFS volumes is only known after a (read-only) mount, which
 * is attempted unless -q is given.
 */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fsmac/fsc.h>

static void json_str(const char *key, const char *s, bool comma)
{
	printf("\"%s\":\"", key);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\')
			printf("\\%c", *p);
		else if (*p < 0x20)
			printf("\\u%04x", *p);
		else
			putchar(*p);
	}
	printf("\"%s", comma ? "," : "");
}

static void uuid_str(const uint8_t u[16], char out[37])
{
	snprintf(out, 37,
		 "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
		 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
		 u[11], u[12], u[13], u[14], u[15]);
}

/* Reads ext2/3/4 superblock state flags. */
static void ext_state(fsc_dev *dev)
{
	unsigned char sb[1024];
	unsigned state, incompat;

	if (fsc_dev_pread(dev, sb, sizeof(sb), 1024))
		return;
	state = sb[58] | (sb[59] << 8);
	incompat = sb[96] | (sb[97] << 8) | (sb[98] << 16) | ((unsigned)sb[99] << 24);
	printf(",\"clean\":%s,\"errors\":%s,\"needs_recovery\":%s",
	       (state & 1) ? "true" : "false", (state & 2) ? "true" : "false",
	       (incompat & 0x4) ? "true" : "false");
}

int main(int argc, char **argv)
{
	bool quick = false;
	int ch, err;
	char label[256], uuid[37];
	uint8_t u[16];
	const char *type;
	fsc_dev *dev;

	fsc_set_log_level(FSC_LOG_ERROR);
	while ((ch = getopt(argc, argv, "q")) != -1) {
		if (ch == 'q')
			quick = true;
		else
			return 2;
	}
	if (optind != argc - 1) {
		fprintf(stderr, "usage: fsmac-probe [-q] <device|image>\n");
		return 2;
	}
	dev = fsc_dev_open_path(argv[optind], true, NULL, &err);
	if (!dev) {
		fprintf(stderr, "fsmac-probe: %s: %s\n", argv[optind], strerror(-err));
		return 2;
	}
	type = fsc_probe(dev, label, sizeof(label), u);
	if (!type) {
		fsc_dev_close(dev);
		printf("{\"type\":null}\n");
		return 1;
	}
	if (!strcmp(type, "ntfs") && !quick) {
		fsc_volume *vol;
		fsc_mount_opts o;

		memset(&o, 0, sizeof(o));
		o.readonly = true;
		if (fsc_mount(dev, &o, &vol) == 0) {
			fsc_volinfo vi;
			fsc_get_volinfo(vol, &vi);
			snprintf(label, sizeof(label), "%s", vi.label);
			fsc_unmount(vol); /* closes dev */
			dev = NULL;
		}
	}
	uuid_str(u, uuid);
	printf("{");
	json_str("type", type, true);
	json_str("label", label, true);
	json_str("uuid", uuid, false);
	if (!strcmp(type, "ext4")) {
		if (!dev)
			dev = fsc_dev_open_path(argv[optind], true, NULL, &err);
		if (dev)
			ext_state(dev);
	}
	printf("}\n");
	if (dev)
		fsc_dev_close(dev);
	return 0;
}
