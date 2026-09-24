// SPDX-License-Identifier: GPL-2.0-only
/* Q1000K factory volume reader. All input is opened read-only.
 * Explicit regular-file inputs allow analysis of offline factory/DSD backups.
 * Raw vendor MTD fallback is intentionally excluded until its BBT/BMT mapping
 * can be verified. Never turn an unverified NAND offset into an identity.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FACTORY_SIZE 0x10000
#define DSD_SIZE 0x12201
#define CAL_SIZE 513

struct identity {
	char serial[13];
	uint8_t mac[6];
	uint8_t cal[CAL_SIZE];
};

static bool valid_calibration(const uint8_t *cal)
{
	bool nonzero = false, nonff = false;
	/* Check the payload, not just the OEM's trailing record byte. */
	for (size_t i = 0; i < CAL_SIZE - 1; i++) {
		nonzero |= cal[i] != 0;
		nonff |= cal[i] != 0xff;
	}
	return nonzero && nonff;
}

static bool valid(struct identity *id)
{
	bool mac = false;
	size_t i;
	if (id->serial[12])
		return false;
	for (i = 0; i < 12; i++)
		if (i < 4 ? !isalnum((unsigned char)id->serial[i]) :
		    !isxdigit((unsigned char)id->serial[i]))
			return false;
	for (i = 0; i < 6; i++)
		mac |= id->mac[i] != 0;
	if (!mac || (id->mac[0] & 1))
		return false;
	return valid_calibration(id->cal);
}

static int read_image(const char *path, uint8_t *buf, size_t len, bool regular)
{
	struct stat st;
	size_t done = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || (regular ? !S_ISREG(st.st_mode) : !S_ISCHR(st.st_mode)) ||
	    (regular && len == CAL_SIZE && st.st_size != CAL_SIZE))
		goto fail;
	while (done < len) {
		ssize_t n = pread(fd, buf + done, len - done, (off_t)done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			goto fail;
		done += n;
	}
	close(fd);
	return 0;
fail:
	close(fd);
	return -1;
}

static int field(const uint8_t *buf, size_t len, const char *key, char *out, size_t size)
{
	size_t pos = 0, klen = strlen(key);
	bool found = false;
	while (pos < len && buf[pos] && buf[pos] != 0xff) {
		size_t start = pos, n;
		while (pos < len && buf[pos] && buf[pos] != 0xff &&
		       buf[pos] != '\n' && buf[pos] != '\r')
			pos++;
		n = pos - start;
		if (n > klen && !memcmp(buf + start, key, klen) && buf[start + klen] == '=') {
			if (found || n - klen - 1 >= size)
				return -1;
			memcpy(out, buf + start + klen + 1, n - klen - 1);
			out[n - klen - 1] = 0;
			found = true;
		}
		while (pos < len && (buf[pos] == '\n' || buf[pos] == '\r'))
			pos++;
	}
	return found ? 0 : -1;
}

static int load(const char *path, bool dsd, bool regular, struct identity *id)
{
	uint8_t *buf = calloc(1, dsd ? DSD_SIZE : FACTORY_SIZE);
	int ret = -1;
	if (!buf || read_image(path, buf, dsd ? DSD_SIZE : FACTORY_SIZE, regular))
		goto out;
	memset(id, 0, sizeof(*id));
	if (dsd) {
		char mac[18];
		unsigned int b[6];
		if (field(buf, 0x4000, "fsan", id->serial, sizeof(id->serial)) ||
		    field(buf, 0x4000, "wan_mac", mac, sizeof(mac)) || strlen(mac) != 17)
			goto out;
		for (size_t i = 0; i < 17; i++)
			if (i % 3 == 2 ? mac[i] != ':' : !isxdigit((unsigned char)mac[i]))
				goto out;
		if (sscanf(mac, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
			goto out;
		for (size_t i = 0; i < 6; i++)
			id->mac[i] = b[i];
		memcpy(id->cal, buf + 0x12000, CAL_SIZE);
	} else {
		memcpy(id->serial, buf + 0x9000, sizeof(id->serial));
		memcpy(id->mac, buf + 0x5000, sizeof(id->mac));
		memcpy(id->cal, buf + 0xb000, CAL_SIZE);
	}
	ret = valid(id) ? 0 : -1;
out:
	free(buf);
	return ret;
}

static bool board_ok(void)
{
	char buf[256];
	FILE *f = fopen("/proc/device-tree/compatible", "r");
	size_t n;
	if (!f)
		return false;
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	return memmem(buf, n, "quantum,q1000k-ubi\0", sizeof("quantum,q1000k-ubi")) != NULL;
}

static int discover(char *dev, size_t size)
{
	glob_t g;
	int found = 0;
	if (!board_ok() || glob("/sys/class/ubi/ubi[0-9]*_[0-9]*/name", 0, NULL, &g))
		return -1;
	for (size_t i = 0; i < g.gl_pathc; i++) {
		char name[32], volume[32], path[128], partition[32];
		unsigned int ubi, id, mtd;
		FILE *f = fopen(g.gl_pathv[i], "r");
		if (!f)
			continue;
		bool match = fgets(name, sizeof(name), f) && !strcmp(name, "factory\n");
		fclose(f);
		if (!match || sscanf(g.gl_pathv[i], "/sys/class/ubi/ubi%u_%u/name", &ubi, &id) != 2)
			continue;
		snprintf(path, sizeof(path), "/sys/class/ubi/ubi%u/mtd_num", ubi);
		f = fopen(path, "r");
		if (!f)
			continue;
		match = fscanf(f, "%u", &mtd) == 1;
		fclose(f);
		if (!match)
			continue;
		snprintf(path, sizeof(path), "/sys/class/mtd/mtd%u/name", mtd);
		f = fopen(path, "r");
		if (!f)
			continue;
		match = fgets(partition, sizeof(partition), f) && !strcmp(partition, "ubi\n");
		fclose(f);
		if (!match)
			continue;
		snprintf(volume, sizeof(volume), "ubi%u_%u", ubi, id);
		snprintf(dev, size, "/dev/%s", volume);
		found++;
	}
	globfree(&g);
	return found == 1 ? 0 : -1;
}

int main(int argc, char **argv)
{
	struct identity id;
	char dev[64];
	const char *path, *source = "factory";
	bool dsd = false, regular = false, cal;
	if (argc < 2 || (strcmp(argv[1], "inspect") && strcmp(argv[1], "calibration")))
		goto usage;
	cal = !strcmp(argv[1], "calibration");
	if (cal && argc == 4 && !strcmp(argv[2], "--calibration-file")) {
		if (read_image(argv[3], id.cal, CAL_SIZE, true) || !valid_calibration(id.cal))
			goto unavailable;
		return fwrite(id.cal, 1, CAL_SIZE, stdout) != CAL_SIZE || fflush(stdout) ? 1 : 0;
	}
	if (argc == 4 && (!strcmp(argv[2], "--factory-file") || !strcmp(argv[2], "--dsd-file"))) {
		path = argv[3];
		regular = true;
		dsd = !strcmp(argv[2], "--dsd-file");
		source = dsd ? "dsd-image" : "factory-image";
	} else if (argc == 2) {
		if (discover(dev, sizeof(dev)))
			goto unavailable;
		path = dev;
	} else {
		goto usage;
	}
	if (load(path, dsd, regular, &id))
		goto unavailable;
	if (cal)
		return fwrite(id.cal, 1, sizeof(id.cal), stdout) != sizeof(id.cal) || fflush(stdout) ? 1 : 0;
	printf("{\"available\":true,\"source\":\"%s\",\"serial\":\"%s\","
	       "\"wan_mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"calibration_bytes\":%u}\n",
	       source, id.serial, id.mac[0], id.mac[1], id.mac[2], id.mac[3], id.mac[4], id.mac[5], CAL_SIZE);
	return fflush(stdout) ? 1 : 0;
unavailable:
	if (!cal)
		puts("{\"available\":false,\"error\":\"Factory identity or XGS-PON calibration unavailable or invalid\"}");
	else
		fputs("Factory identity or XGS-PON calibration unavailable or invalid\n", stderr);
	return 1;
usage:
	fputs("Usage: q1000k-pon-factory inspect|calibration [--factory-file FILE|--dsd-file FILE]\n"
	      "       q1000k-pon-factory calibration --calibration-file FILE\n", stderr);
	return 2;
}
