/*-
 * Copyright 2020 Colin Percival
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/ioctl.h>

#include <dev/nvme/nvme.h>

#include <err.h>
#include <fcntl.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AMZN_NVME_VID 0x1d0f
#define AMZN_NVME_EBS_MN "Amazon Elastic Block Store"
#define AMZN_NVME_ISTORE_MN "Amazon EC2 NVMe Instance Storage"

/* Extract a string from NVMe metadata. */
static char *
extract(const uint8_t * buf, size_t buflen)
{
	char * s;

	/* Advance past leading spaces. */
	while ((buflen > 0) && (buf[0] == ' ')) {
		buf++;
		buflen--;
	}

	/* Remove trailing NULs and spaces. */
	while (buflen > 0) {
		if ((buf[buflen - 1] == '\0') || (buf[buflen - 1] == ' '))
			buflen--;
		else
			break;
	}

	/* Duplicate what's left. */
	if ((s = malloc(buflen + 1)) == NULL)
		err(1, "malloc");
	memcpy(s, buf, buflen);
	s[buflen] = '\0';

	/* Return extracted string. */
	return (s);
}

static void
usage(void)
{

	fprintf(stderr,
	    "usage: ebsnvme-id [-b] [-m] [-s] [-u] [-v] device\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct nvme_get_nsid nsid;
	struct nvme_pt_command c;
	struct nvme_controller_data d;
	int opt_bu = 0;
	int opt_m = 0;
	int opt_s = 0;
	int opt_v = 0;
	int ch;
	const char * devname;
	char * s;
	int fd;
	char * s_sn;
	char * s_mn;
	char * s_linuxname;

	/* Process command line. */
	while ((ch = getopt(argc, argv, "bmsuv")) != -1) {
		switch (ch) {
		case 'b':
		case 'u':
			/*
			 * The Amazon Linux ebsnvme-id tool has two options
			 * which behave identically: "Return block device
			 * mapping", and "Output data in format suitable for
			 * udev rules".
			 */
			opt_bu = 1;
			break;
		case 'm':
			/* FreeBSD-specific option: Output Model Number. */
			opt_m = 1;
			break;
		case 's':
			/*
			 * FreeBSD-specific option: Output Serial Number.
			 * Unlike the -v option, this applies to Instance
			 * Storage disks too.
			 */
			opt_s = 1;
			break;
		case 'v':
			/* Return volume ID. */
			opt_v = 1;
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	/* We should have one option left -- the device name. */
	if (argc != 1)
		usage();
	devname = argv[0];

	/* Strip leading "/dev/" if provided. */
	if (strncmp(devname, _PATH_DEV, strlen(_PATH_DEV)) == 0)
		devname = &devname[strlen(_PATH_DEV)];

	/* If mode not provided, default to -b -v as in Amazon Linux. */
	if ((opt_bu | opt_m | opt_s | opt_v) == 0) {
		opt_bu = 1;
		opt_v = 1;
	}

	/* Construct path to device and open it. */
	if (asprintf(&s, "%s%s", _PATH_DEV, devname) == -1)
		err(1, "asprintf");
	if ((fd = open(s, O_RDONLY)) == -1)
		err(1, "could not open %s", s);
	free(s);

	/* Find the underlying NVMe device. */
	if (ioctl(fd, NVME_GET_NSID, &nsid))
		err(1, "NVME_GET_NSID failed");
	if (nsid.nsid) {
		/* Close the disk and open the NVMe device. */
		close(fd);
		if (asprintf(&s, "%s%s", _PATH_DEV, nsid.cdev) == -1)
			err(1, "asprintf");
		if ((fd = open(s, O_RDONLY)) == -1)
			err(1, "could not open %s", s);
		free(s);
	}

	/* Ask the NVMe controller to identify itself. */
	memset(&c, 0, sizeof(c));
	c.cmd.opc = NVME_OPC_IDENTIFY;
	c.cmd.cdw10 = htole32(1);
	c.buf = &d;
	c.len = sizeof(struct nvme_controller_data);
	c.is_read = 1;
	if (ioctl(fd, NVME_PASSTHROUGH_CMD, &c))
		err(1, "NVME_OPC_IDENTIFY failed");
	if (nvme_completion_is_error(&c.cpl))
		errx(1, "identify request returned error");
	close(fd);

	/* The vendor should be Amazon for any disks we're looking at. */
	if (le16toh(d.vid) != AMZN_NVME_VID)
		errx(1, "Not an EC2 disk: %s", devname);

	/* Extract serial number, model number, and block device name. */
	s_sn = extract(d.sn, NVME_SERIAL_NUMBER_LENGTH);
	s_mn = extract(d.mn, NVME_MODEL_NUMBER_LENGTH);
	s_linuxname = extract(d.vs, 32);

	/*
	 * If this isn't an EBS volume or an Instance Storage disk,
	 * something weird is going on; throw an error, since it probably
	 * means this utility needs to be updated.
	 */
	if (strcmp(s_mn, AMZN_NVME_EBS_MN) &&
	    strcmp(s_mn, AMZN_NVME_ISTORE_MN))
		errx(1, "Not an EBS or Instance Storage disk: %s", devname);

	/* Output the desired information. */
	if (opt_v) {
		/* Print the volume ID; EBS volumes only. */
		if (strcmp(s_mn, AMZN_NVME_EBS_MN))
			errx(1, "Not an EBS device: %s", devname);
		printf("Volume ID: ");
		if ((strncmp(s_sn, "vol", 3) == 0) &&
		    (strncmp(s_sn, "vol-", 4) != 0))
			printf("vol-%s\n", &s_sn[3]);
		else
			printf("%s\n", s_sn);
	}
	if (opt_bu) {
		/* Print the linux device name; EBS volumes only. */
		if (strcmp(s_mn, AMZN_NVME_EBS_MN))
			errx(1, "Not an EBS device: %s", devname);
		if (strncmp(s_linuxname, "/dev/", 5) == 0)
			printf("%s\n", &s_linuxname[5]);
		else
			printf("%s\n", s_linuxname);
	}
	if (opt_m) {
		/* Print the Model Number, even for non-EBS disks. */
		printf("%s\n", s_mn);
	}
	if (opt_s) {
		/* Print the Serial Number, even for non-EBS disks. */
		printf("%s\n", s_sn);
	}

	/* Success! */
	exit(0);
}
