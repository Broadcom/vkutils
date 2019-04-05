// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Broadcom.
 */
#include <elf.h>
#include <err.h>
#include <gelf.h>
#include <fcntl.h>
#include <libelf.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * Convert elf file to VK binary of load records of form:
 * MAGIC  (U64) - VK Image indicator
 * LENGTH (U64) - Length of remainder of image (including MAGIC at end)
 * ADDR0 (U32)  - Section 0 load address
 * SIZE0 (U32)  - Section 0 size
 * DATA0[] (U8) - Section 0 bytes to load
 * ..
 * ..
 * ADDR0 (U32)  - Section N load address
 * SIZE0 (U32)  - Section N size
 * DATA0[] (U8) - Section N bytes to load
 * ENTRY_ADDR (U32) - Entry Address to jump to
 * MAGIC (U64) - VK Image Indicator
 */

/* MAGIC field placed at start/end of binary */
#define MAGIC 0x1234567800000000

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
	uint32_t addr;
	uint32_t entry_addr;
	uint64_t magic = MAGIC;
	uint64_t length;

	int verbose = 1;

	if (argc != 3)
		errx(EXIT_FAILURE, "usage: %s infile outfile", argv[0]);

	if (elf_version(EV_CURRENT) == EV_NONE)
		errx(EXIT_FAILURE, "libelf init failed: %s", elf_errmsg(-1));

	/* open elf input file */
	fd_in = open(argv[1], O_RDONLY, 0);
	if (fd_in < 0)
		errx(EXIT_FAILURE, "open \%s\" failed", argv[1]);

	elf_in = elf_begin(fd_in, ELF_C_READ, NULL);
	if (elf_in == NULL)
		errx(EXIT_FAILURE, "elf_begin() failed: %s.", elf_errmsg(-1));

	if (elf_kind(elf_in) != ELF_K_ELF)
		errx(EXIT_FAILURE, "%s must be an ELF file.\n", argv[1]);

	if (gelf_getehdr(elf_in, &ehdr) == NULL)
		errx(EXIT_FAILURE, "getehdr() failed: %s", elf_errmsg(-1));

	/* open output file */
	fd_out = fopen(argv[2], "w");
	if (fd_out == NULL)
		errx(EXIT_FAILURE, "open \"%s\" failed", argv[2]);

	if (verbose)
		printf("MAGIC=0x%jx\n", MAGIC);

	/* Write magic indicator at start of file */
	fwrite(&magic, sizeof(magic), 1, fd_out);

	/* Write 0 to length in file - will be updated later */
	length = 0;
	fwrite(&length, sizeof(length), 1, fd_out);

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

		if ((data->d_size <= 0) | (data->d_buf == NULL)) {
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

			printf("ADDR=0x%8.8x SZ=0x%8.8x ", addr, sz);

			if (elf_getshdrstrndx(elf_in, &shstrndx) != 0)
				errx(EXIT_FAILURE,
				     "getshdrstrndx failed %s.",
				     elf_errmsg(-1));

			name = elf_strptr(elf_in, shstrndx, shdr.sh_name);
			if (name == NULL)
				errx(EXIT_FAILURE,
				     "strptr() failed %s.", elf_errmsg(-1));

			printf("Section %-4.4jd %s\n", elf_ndxscn(scn), name);
		}

		/* Write ADDR, SZ, and DATA to binary file */
		fwrite(&addr, sizeof(addr), 1, fd_out);
		length += sizeof(addr);
		fwrite(&sz, sizeof(sz), 1, fd_out);
		length += sizeof(sz);
		fwrite(buf, sizeof(*buf), sz, fd_out);
		length += sizeof(*buf) * sz;
	}

	/* Write entry addr */
	entry_addr = ehdr.e_entry;
	if (verbose)
		printf("ENTR_ADDR=0x%8.8x\n", entry_addr);
	fwrite(&entry_addr, sizeof(entry_addr), 1, fd_out);
	length += sizeof(entry_addr);

	/* Write magic at end of file */
	fwrite(&magic, sizeof(magic), 1, fd_out);
	length += sizeof(magic);

	if (verbose)
		printf("MAGIC=0x%jx\n", MAGIC);

	/* Update length field near start of file */
	fseek(fd_out, sizeof(magic), SEEK_SET);
	fwrite(&length, sizeof(length), 1, fd_out);

	/* Cleanup */
	elf_end(elf_in);
	close(fd_in);
	fclose(fd_out);

	printf("Binary file %s generation complete.\n", argv[2]);
	exit(EXIT_SUCCESS);
}
