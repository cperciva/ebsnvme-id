/*-
 * Copyright 2020, 2026 Colin Percival
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

#include <sys/endian.h>
#include <sys/ioctl.h>

#include <dev/nvme/nvme.h>

#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AMZN_NVME_VID 0x1d0f
#define AMZN_NVME_STATS_LOGPAGE_ID 0xd0
#define AMZN_NVME_STATS_MAGIC 0x3c23b510
#define AMZN_NVME_EBS_MN "Amazon Elastic Block Store"
#define AMZN_NVME_ISTORE_MN "Amazon EC2 NVMe Instance Storage"

#define CVT_LE32TOH(x)	((x) = le32toh(x))
#define CVT_LE64TOH(x)	((x) = le64toh(x))

struct nvme_histogram_bin {
	uint64_t lower;
	uint64_t upper;
	uint32_t count;
	uint32_t reserved0;
};

struct ebs_nvme_histogram {
	uint64_t num_bins;
	struct nvme_histogram_bin bins[64];
};

struct nvme_amzn_stats_data {
	uint32_t magic;
	uint32_t reserved0;
	uint64_t total_read_ops;
	uint64_t total_write_ops;
	uint64_t total_read_bytes;
	uint64_t total_write_bytes;
	/* Next 6 fields are time in microseconds. */
	uint64_t total_read_time;
	uint64_t total_write_time;
	uint64_t ebs_volume_performance_exceeded_iops;
	uint64_t ebs_volume_performance_exceeded_tp;
	uint64_t ec2_instance_ebs_performance_exceeded_iops;
	uint64_t ec2_instance_ebs_performance_exceeded_tp;
	uint64_t volume_queue_length;
	uint8_t reserved1[416];
	struct ebs_nvme_histogram read_io_latency_histogram;
	struct ebs_nvme_histogram write_io_latency_histogram;
	uint8_t reserved2[496];
};
_Static_assert(sizeof(struct nvme_amzn_stats_data) == 4096,
    "stats page not packed");

static int opt_bu = 0;
static int opt_m = 0;
static int opt_s = 0;
static int opt_v = 0;

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
ebsnvme_id(const char * devname, const struct nvme_controller_data * d)
{
	char * s_sn;
	char * s_mn;
	char * s_linuxname;

	/* If mode not provided, default to -b -v as in Amazon Linux. */
	if ((opt_bu | opt_m | opt_s | opt_v) == 0) {
		opt_bu = 1;
		opt_v = 1;
	}

	/* Extract serial number, model number, and block device name. */
	s_sn = extract(d->sn, NVME_SERIAL_NUMBER_LENGTH);
	s_mn = extract(d->mn, NVME_MODEL_NUMBER_LENGTH);
	s_linuxname = extract(d->vs, 32);

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
}

static void
print_histogram(const char * s, const struct ebs_nvme_histogram * h)
{
	size_t i;

	printf("%s\n", s);
	printf("Number of bins: %" PRIu64 "\n"
	    "=================================\n"
	    "Lower       Upper        IO Count\n"
	    "=================================\n",
	    h->num_bins);
	for (i = 0; i < h->num_bins && i < 64; i++)
		printf("[%-8" PRIu64 " - %-8" PRIu64 "] => %" PRIu32 "\n",
		    h->bins[i].lower,
		    h->bins[i].upper,
		    h->bins[i].count);
}

static void
ebsnvme_stats_print(const struct nvme_amzn_stats_data * stats)
{

	printf("Total Ops\n"
	    "  Read: %" PRIu64 "\n"
	    "  Write: %" PRIu64 "\n"
	    "Total Bytes\n"
	    "  Read: %" PRIu64 "\n"
	    "  Write: %" PRIu64 "\n"
	    "Total Time (us)\n"
	    "  Read: %" PRIu64 "\n"
	    "  Write: %" PRIu64 "\n"
	    "EBS Volume Performance Exceeded (us)\n"
	    "  IOPS: %" PRIu64 "\n"
	    "  Throughput: %" PRIu64 "\n"
	    "EC2 Instance EBS Performance Exceeded (us)\n"
	    "  IOPS: %" PRIu64 "\n"
	    "  Throughput: %" PRIu64 "\n"
	    "Queue Length (point in time): %" PRIu64 "\n",
	    stats->total_read_ops,
	    stats->total_write_ops,
	    stats->total_read_bytes,
	    stats->total_write_bytes,
	    stats->total_read_time,
	    stats->total_write_time,
	    stats->ebs_volume_performance_exceeded_iops,
	    stats->ebs_volume_performance_exceeded_tp,
	    stats->ec2_instance_ebs_performance_exceeded_iops,
	    stats->ec2_instance_ebs_performance_exceeded_tp,
	    stats->volume_queue_length);
	printf("\n");
	print_histogram("Read IO Latency Histogram (us)",
	    &stats->read_io_latency_histogram);
	printf("\n");
	print_histogram("Write IO Latency Histogram (us)",
	    &stats->write_io_latency_histogram);
}

static void
le_histogram_toh(struct ebs_nvme_histogram * h)
{
	size_t i;

	CVT_LE64TOH(h->num_bins);
	for (i = 0; i < 64; i++) {
		CVT_LE64TOH(h->bins[i].lower);
		CVT_LE64TOH(h->bins[i].upper);
		CVT_LE32TOH(h->bins[i].count);
	}
}

static void
ebsnvme_stats_read(int fd, const char * devname,
    struct nvme_amzn_stats_data * stats)
{
	struct nvme_pt_command c;

	/* Log page request */
	memset(&c, 0, sizeof(c));
	memset(stats, 0, sizeof(*stats));
	c.cmd.opc = NVME_OPC_GET_LOG_PAGE;
	c.cmd.nsid = htole32(1);
	c.cmd.cdw10 = htole32(AMZN_NVME_STATS_LOGPAGE_ID | (1023 << 16));
	c.buf = stats;
	c.len = sizeof(*stats);
	c.is_read = 1;
	if (ioctl(fd, NVME_PASSTHROUGH_CMD, &c))
		err(1, "NVME_OPC_GET_LOG_PAGE failed");
	if (nvme_completion_is_error(&c.cpl))
		errx(1, "log page request returned error");

	/* Convert statistics from little-endian byte order. */
	CVT_LE32TOH(stats->magic);
	CVT_LE64TOH(stats->total_read_ops);
	CVT_LE64TOH(stats->total_write_ops);
	CVT_LE64TOH(stats->total_read_bytes);
	CVT_LE64TOH(stats->total_write_bytes);
	CVT_LE64TOH(stats->total_read_time);
	CVT_LE64TOH(stats->total_write_time);
	CVT_LE64TOH(stats->ebs_volume_performance_exceeded_iops);
	CVT_LE64TOH(stats->ebs_volume_performance_exceeded_tp);
	CVT_LE64TOH(stats->ec2_instance_ebs_performance_exceeded_iops);
	CVT_LE64TOH(stats->ec2_instance_ebs_performance_exceeded_tp);
	CVT_LE64TOH(stats->volume_queue_length);
	le_histogram_toh(&stats->read_io_latency_histogram);
	le_histogram_toh(&stats->write_io_latency_histogram);

	/* Check magic. */
	if (stats->magic != AMZN_NVME_STATS_MAGIC)
		errx(1, "Not an EBS device: %s", devname);
}

static void
ebsnvme_stats(int fd, const char * devname)
{
	struct nvme_amzn_stats_data stats;

	ebsnvme_stats_read(fd, devname, &stats);
	ebsnvme_stats_print(&stats);
}

static void
usage(void)
{

	fprintf(stderr,
	    "usage: ebsnvme id [-b] [-m] [-s] [-u] [-v] device\n"
	    "       ebsnvme-id [-b] [-m] [-s] [-u] [-v] device\n"
	    "       ebsnvme stats device\n"
	    "       ebsnvme-stats device\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct nvme_get_nsid nsid;
	struct nvme_pt_command c;
	struct nvme_controller_data d;
	int ch;
	const char * devname;
	char * s;
	int fd;
	const char * progname;
	const char * cmd = NULL;
	char * mn;

	/* Decide which ebsnvme tool we are. */
	if ((argc == 0) || (argv[0] == NULL))
		errx(1, "don't know who I am");
	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		progname++;
	if (strcmp(progname, "ebsnvme-id") == 0)
		cmd = "id";
	else if (strcmp(progname, "ebsnvme-stats") == 0)
		cmd = "stats";
	else if (strcmp(progname, "ebsnvme") == 0) {
		if (argc == 1)
			usage();
		if (strcmp(argv[1], "id") == 0)
			cmd = "id";
		else if (strcmp(argv[1], "stats") == 0)
			cmd = "stats";
		if (cmd == NULL)
			usage();
		argv++;
		argc--;
	}
	if (cmd == NULL)
		errx(1, "don't know who I am");

	/* Process command line. */
	while ((ch = getopt(argc, argv, "bmsuv")) != -1) {
		switch (ch) {
		case 'b':	/* id */
		case 'u':	/* id */
			/*
			 * The Amazon Linux ebsnvme-id tool has two options
			 * which behave identically: "Return block device
			 * mapping", and "Output data in format suitable for
			 * udev rules".
			 */
			opt_bu = 1;
			break;
		case 'm':	/* id */
			/* FreeBSD-specific option: Output Model Number. */
			opt_m = 1;
			break;
		case 's':	/* id */
			/*
			 * FreeBSD-specific option: Output Serial Number.
			 * Unlike the -v option, this applies to Instance
			 * Storage disks too.
			 */
			opt_s = 1;
			break;
		case 'v':	/* id */
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

	/* Check unmatched options. */
	if ((strcmp(cmd, "stats") == 0) &&
	    (opt_bu || opt_m || opt_s || opt_v))
		usage();

	/* We should have one option left -- the device name. */
	if (argc != 1)
		usage();
	devname = argv[0];

	/* Strip leading "/dev/" if provided. */
	if (strncmp(devname, _PATH_DEV, strlen(_PATH_DEV)) == 0)
		devname = &devname[strlen(_PATH_DEV)];

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
	memset(&d, 0, sizeof(d));
	c.cmd.opc = NVME_OPC_IDENTIFY;
	c.cmd.cdw10 = htole32(1);
	c.buf = &d;
	c.len = sizeof(struct nvme_controller_data);
	c.is_read = 1;
	if (ioctl(fd, NVME_PASSTHROUGH_CMD, &c))
		err(1, "NVME_OPC_IDENTIFY failed");
	if (nvme_completion_is_error(&c.cpl))
		errx(1, "identify request returned error");

	/* The vendor should be Amazon for any disks we're looking at. */
	if (le16toh(d.vid) != AMZN_NVME_VID)
		errx(1, "Not an EC2 disk: %s", devname);

	/*
	 * Extract the model number and sanity-check.  If the model number
	 * doesn't match EBS volumes or Instance Storage disks, something
	 * weird is going on; throw an error, since it probably means this
	 * utility needs to be updated.
	 */
	mn = extract(d.mn, NVME_MODEL_NUMBER_LENGTH);
	if (strcmp(mn, AMZN_NVME_EBS_MN) &&
	    strcmp(mn, AMZN_NVME_ISTORE_MN))
		errx(1, "Not an EBS or Instance Storage disk: %s", devname);

	if (strcmp(cmd, "id") == 0)
		ebsnvme_id(devname, &d);
	else if (strcmp(cmd, "stats") == 0)
		ebsnvme_stats(fd, devname);

	/* Close the device descriptor. */
	close(fd);

	/* Success! */
	exit(0);
}
