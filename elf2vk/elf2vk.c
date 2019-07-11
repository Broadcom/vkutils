// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Broadcom.
 */
#include <elf.h>
#include <err.h>
#include <gelf.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libelf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Convert elf file to VK binary of load records of form:
 * MAGIC  (U64) - VK Image indicator
 * LENGTH (U64) - Length of remainder of image (including MAGIC at end)
 * --------------- Processor 0 Section Info --------
 * ADDR0 (U64)  - Section 0 load address
 * SIZE0 (U32)  - Section 0 size
 * DATA0[] (U8) - Section 0 bytes to load
 * ..
 * ..
 * ADDR0 (U64)  - Section N load address
 * SIZE0 (U32)  - Section N size
 * DATA0[] (U8) - Section N bytes to load
 * ENTRY_ADDR0 (U64) - Higher 16bits contains processor number ID
 *                   - Lower 48bits contains the address to jump to
 * SIZE0 (U32)  - Entry size always zero
 * ---------------- Processor X Section Info -------
 * ADDRX (U64)  - Section 0 load address
 * SIZEX (U32)  - Section 0 size
 * DATAX[] (U8) - Section 0 bytes to load
 * ..
 * ..
 * ADDRX (U64)  - Section N load address
 * SIZEX (U32)  - Section N size
 * DATAX[] (U8) - Section N bytes to load
 * ENTRY_ADDRX (U64) - Higher 16bits contains processor number ID
 *                   - Lower 48bits contains the address to jump to
 * SIZEX (U32)  - Entry size always zero
 * -------------------------------------------------
 * MAGIC (U64) - VK Image Indicator
 */

/* MAGIC field placed at start/end of binary */
#define MAGIC 0x1234567800000000
#define MAX_NPROCESSOR		8
#define MAX_FILENAME_LEN	256
#define PROCESSOR_ID_SHIFT	48
#define BIT_ULL(x)		(1ULL << (x))
#define ENTRY_ADDR_MASK		(BIT_ULL(PROCESSOR_ID_SHIFT) - 1)

static void usage(const char *prog)
{
	errx(EXIT_FAILURE, "%s: %s\n"
	     "\t -i infile1 -p processorID1\n"
	     "\t ...\n"
	     "\t -i infile%d -p processorID%d\n"
	     "\t -o outfile", __func__, prog, MAX_NPROCESSOR, MAX_NPROCESSOR);
}

int main(int argc, char **argv)
{
	int fd_in;
	FILE *fd_out;
	Elf *elf_in;
	GElf_Ehdr ehdr;
	GElf_Shdr shdr;
	Elf_Scn *scn;
	Elf_Data *data;
	uint8_t *buf;
	uint32_t sz;
	uint64_t addr;
	uint64_t entry_addr;
	uint64_t magic = MAGIC;
	uint64_t length;
	char infiles[MAX_NPROCESSOR][MAX_FILENAME_LEN];
	uint16_t processorid[MAX_NPROCESSOR];
	char outfile[MAX_FILENAME_LEN];
	int c;

	int verbose = 1;
	uint16_t nfiles = 0;
	uint16_t nprocessors = 0;
	int outfile_flag = 0;
	int x, y;

	while ((c = getopt(argc, argv, "i:p:o:")) != -1) {
		switch (c) {
		case 'i':
			if (strlen(optarg) >= MAX_FILENAME_LEN)
				errx(EXIT_FAILURE,
				     "ERROR: Input filename length exceeded %zu > %d\n",
				     strlen(optarg), MAX_FILENAME_LEN);
			strcpy(&infiles[nfiles++][0], optarg);
			break;
		case 'p':
			if (atoi(optarg) > UINT16_MAX)
				errx(EXIT_FAILURE,
				    "ERROR: Processor ID exceeds UINT16_MAX\n");
			processorid[nprocessors++] = atoi(optarg);
			break;
		case 'o':
			if (strlen(optarg) >= MAX_FILENAME_LEN)
				errx(EXIT_FAILURE,
				     "ERROR: Output filename length exceeded %zu > %d\n",
				     strlen(optarg), MAX_FILENAME_LEN);
			strcpy(&outfile[0], optarg);
			outfile_flag = 1;
			break;
		case '?':
			usage(argv[0]);
		}
	}

	/* Input files must have equal no of processor IDs to associate with */
	if (nfiles != nprocessors || nfiles == 0 || outfile_flag == 0)
		usage(argv[0]);

	/* Check if processor IDs are unique */
	for (x = 0; x < nprocessors; x++) {
		for (y = 0; y < x; y++) {
			if (processorid[x] == processorid[y])
				errx(EXIT_FAILURE,
				     "ERROR: Processor ID must be unique!\n");
		}
	}

	if (elf_version(EV_CURRENT) == EV_NONE)
		errx(EXIT_FAILURE, "libelf init failed: %s", elf_errmsg(-1));

	/* open output file */
	fd_out = fopen(outfile, "w");
	if (fd_out == NULL)
		errx(EXIT_FAILURE, "open \"%s\" failed", outfile);

	if (verbose)
		printf("MAGIC=0x%jx\n", MAGIC);

	/* Write magic indicator at start of file */
	fwrite(&magic, sizeof(magic), 1, fd_out);

	/* Write 0 to length in file - will be updated later */
	length = 0;
	fwrite(&length, sizeof(length), 1, fd_out);

	for (c = 0; c < nfiles; c++) {
		/* open elf input file */
		fd_in = open(infiles[c], O_RDONLY, 0);
		if (fd_in < 0)
			errx(EXIT_FAILURE, "open \%s\" failed", infiles[c]);

		elf_in = elf_begin(fd_in, ELF_C_READ, NULL);
		if (elf_in == NULL)
			errx(EXIT_FAILURE, "elf_begin() failed: %s.",
			     elf_errmsg(-1));

		if (elf_kind(elf_in) != ELF_K_ELF)
			errx(EXIT_FAILURE, "%s must be an ELF file.\n",
			     infiles[c]);

		if (gelf_getehdr(elf_in, &ehdr) == NULL)
			errx(EXIT_FAILURE, "getehdr() failed: %s",
			     elf_errmsg(-1));

		scn = NULL;

		while ((scn = elf_nextscn(elf_in, scn)) != NULL) {
			if (gelf_getshdr(scn, &shdr) != &shdr)
				errx(EXIT_FAILURE,
				     "getshdr() failed %s.", elf_errmsg(-1));

			if (!(shdr.sh_flags & SHF_ALLOC)) {
				/* Section not allocated, so nothing to write */
				continue;
			}

			data = NULL;
			data = elf_getdata(scn, data);
			if (data == NULL)
				data = elf_rawdata(scn, data);
			if (data == NULL)
				errx(EXIT_FAILURE,
				     "getdata failed %s.", elf_errmsg(-1));

			if ((data->d_size <= 0) || (data->d_buf == NULL)) {
				/* No data to write in section */
				continue;
			}

			addr = shdr.sh_addr;
			sz = data->d_size;
			buf = data->d_buf;

			/* Debug info printing section generation information */
			if (verbose) {
				size_t shstrndx;
				char *name;

				printf("ADDR=0x%016" PRIx64 " SZ=0x%8.8x ",
				       addr, sz);

				if (elf_getshdrstrndx(elf_in, &shstrndx) != 0)
					errx(EXIT_FAILURE,
					     "getshdrstrndx failed %s.",
					     elf_errmsg(-1));

				name = elf_strptr(elf_in, shstrndx,
						  shdr.sh_name);
				if (name == NULL)
					errx(EXIT_FAILURE,
					     "strptr() failed %s.",
					     elf_errmsg(-1));

				printf("Section %-4.4jd %s\n",
				       elf_ndxscn(scn), name);
			}

			/* Write ADDR, SZ, and DATA to binary file */
			fwrite(&addr, sizeof(addr), 1, fd_out);
			length += sizeof(addr);
			fwrite(&sz, sizeof(sz), 1, fd_out);
			length += sizeof(sz);
			fwrite(buf, sizeof(*buf), sz, fd_out);
			length += sizeof(*buf) * sz;
		}

		/* Write processor ID and entry addr */
		entry_addr = processorid[c];
		entry_addr = (entry_addr << PROCESSOR_ID_SHIFT) | ehdr.e_entry;
		if (verbose)
			printf("ProcessorID = %u ENTR_ADDR=0x%llx\n",
			       (uint16_t)(entry_addr >> PROCESSOR_ID_SHIFT),
			       (entry_addr & ENTRY_ADDR_MASK));

		fwrite(&entry_addr, sizeof(entry_addr), 1, fd_out);
		length += sizeof(entry_addr);

		/* Write zero size for entry addr */
		sz = 0;
		fwrite(&sz, sizeof(sz), 1, fd_out);
		length += sizeof(sz);

		/* Cleanup */
		elf_end(elf_in);
		close(fd_in);
	}

	/* Write magic at end of file */
	fwrite(&magic, sizeof(magic), 1, fd_out);
	length += sizeof(magic);

	if (verbose)
		printf("MAGIC=0x%jx\n", MAGIC);

	/* Update length field near start of file */
	fseek(fd_out, sizeof(magic), SEEK_SET);
	fwrite(&length, sizeof(length), 1, fd_out);

	fclose(fd_out);

	printf("Binary file %s generation complete.\n", outfile);
	exit(EXIT_SUCCESS);
}
