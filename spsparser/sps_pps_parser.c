// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020 Broadcom.
 * sps_pps_parser - Parsing a binary dump of the SPS and PPS NAL units of
 * H.264 video, and logging the syntax elements contained. This uses the
 * h264bitstream library.
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include "h264_stream.h"

#define MIN(x, y) \
({ typeof(X) x_ = (X); \
	typeof(Y) y_ = (Y); \
	(x_ < y_) ? x_ : y_; })
#define MAX(x, y) \
({ typeof(X) x_ = (X); \
	typeof(Y) y_ = (Y); \
	(x_ > y_) ? x_ : y_; })

#if (defined(__GNUC__))
#define HAVE_GETOPT_LONG

#include <getopt.h>

static struct option long_options[] = {
	{"output",  required_argument, NULL, 'o'},
	{"help", no_argument, NULL, 'h'},
	{"verbose", required_argument, NULL, 'v'},
};
#endif

static char options[] =
"\t-o <output_file>, defaults to STDOUT\n"
"\t-v <verbose_level>, print more info\n"
"\t-h print this message and exit\n";

void usage(void)
{
	fprintf(stderr, "sps/pps parser version 0.1\n");
	fprintf(stderr, "Analyse the NAL units of SPS and PPS which have been extracted\n");
	fprintf(stderr, "from h264 bitstreams in Annex B format\n");
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "sps_pps_parser [options] <bitstream_log>\noptions:\n%s\n", options);
}

int main(int argc, char *argv[])
{
	FILE *infile;
	char *prefix = "Raw SPS/PPS: ";
	char *line;
	char *bin_string;
	size_t len = 0;

	if (argc < 2) {
		usage();
		return EXIT_FAILURE;
	}

	int opt_verbose = 1;

#ifdef HAVE_GETOPT_LONG
	int c;
	int long_options_index;
	extern char *optarg;
	extern int optind;

	while ((c = getopt_long(argc, argv, "o:hv:", long_options, &long_options_index)) != -1) {
		switch (c) {
		case 'o':
			if (h264_dbgfile == NULL)
				h264_dbgfile = fopen(optarg, "wt");
			break;
		case 'v':
			opt_verbose = atoi(optarg);
			break;
		case 'h':
		default:
			usage();
			return 1;
		}
	}
#else
	int optind = 1;
#endif
	infile = fopen(argv[optind], "rb");

	if (h264_dbgfile == NULL)
		h264_dbgfile = stdout;
	if (infile == NULL) {
		fprintf(stderr, "!! Error: could not open file %s: %s\n",
			argv[optind], strerror(errno));
		exit(EXIT_FAILURE);
	}

	h264_stream_t *h = h264_new();
	int prefixes_located = 0;

	while (getline(&line, &len, infile) != -1) {
		/* Locate prefix */
		bin_string = strstr(line, prefix);

		if (bin_string) {
			prefixes_located++;
			/* Skip over prefix */
			bin_string = bin_string + strlen(prefix);
			char *start = bin_string;
			/*
			 * For every byte there are at least 4 characters in the string: eg:
			 * "\xAB"
			 */
			uint8_t *buf = (uint8_t *)malloc(1 + strlen(bin_string) / 4);
			int rsz = 0;

			while (start) {
				char single_byte[2];
				char *end = strstr(start + 1, "\\x");
				/* The first two characters are always \x, skip them" */
				memcpy(single_byte, start + 2, 2);
				buf[rsz++] = strtol(single_byte, NULL, 16);
				start = end;
			}

			size_t sz = 0;
			int64_t off = 0;
			uint8_t *p = buf;
			int nal_start, nal_end;

			nal_start = 0;
			nal_end = 0;
			sz += rsz;
			int nal_units_found = 0;
			/* Iterate through all included NAL units, namely SPS and PPS here. */
			while (find_nal_unit(p, rsz, &nal_start, &nal_end) > 0) {
				if (opt_verbose > 0) {
					fprintf(h264_dbgfile,
						"!! Found NAL at offset %lld (0x%04llX), size %lld (0x%04llX)\n",
						(long long)(off + (p - buf) + nal_start),
						(long long)(off + (p - buf) + nal_start),
						(long long)(nal_end - nal_start),
						(long long)(nal_end - nal_start));
					fprintf(h264_dbgfile, "XX ");
					/* Show at most the first 16 bytes */
					debug_bytes(p + nal_start - 4,
						(nal_end - nal_start + 4 >= 16) ? 16 :
						(nal_end - nal_start + 4));
				}
				/*
				 * Always use the read_debug_nal_unit function instead of the
				 * read_nal_unit function to log contents
				 */
				p += nal_start;
				read_debug_nal_unit(h, p, nal_end - nal_start);
				p += (nal_end - nal_start);
				sz -= nal_end;
				nal_units_found++;
			}
			if (!nal_units_found) {
				fprintf(stderr, "!! Error: No NAL units found in string %s\n",
					bin_string);
				exit(EXIT_FAILURE);
			}
			free(buf);
		}
	}
	if (!prefixes_located) {
		fprintf(stderr, "!! Error: Expected prefix \"%s\" not in input file\n",
			prefix);
		exit(EXIT_FAILURE);
	}
	fclose(infile);
	h264_free(h);
	if (line)
		free(line);
}
