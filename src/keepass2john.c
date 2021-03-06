/* keepass2john utility (modified KeeCracker) written in March of 2012
 * by Dhiru Kholia. keepass2john processes input KeePass 1.x and 2.x
 * database files into a format suitable for use with JtR. This software
 * is Copyright (c) 2012, Dhiru Kholia <dhiru.kholia at gmail.com> and it
 * is hereby released under GPL license.
 *
 * KeePass 2.x support is based on KeeCracker - The KeePass 2 Database
 * Cracker, http://keecracker.mbw.name/
 *
 * KeePass 1.x support is based on kppy -  A Python-module to provide
 * an API to KeePass 1.x files. http://gitorious.org/kppy/kppy
 * Copyright (C) 2012 Karsten-Kai König <kkoenig@posteo.de>
 *
 * Keyfile support for Keepass 1.x and Keepass 2.x was added by Fist0urs
 * <eddy.maaalou at gmail.com>
 *
 * kppy is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or at your option) any later version.
 *
 * kppy is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * kppy. If not, see <http://www.gnu.org/licenses/>. */

#if AC_BUILT
#include "autoconfig.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#ifdef _MSC_VER
#include "missing_getopt.h"
#endif
#include <errno.h>
// needs to be above sys/types.h and sys/stat.h for mingw, if -std=c99 used.
#include "jumbo.h"
#include <sys/stat.h>
#include <sys/types.h>
#if  (!AC_BUILT || HAVE_UNISTD_H) && !_MSC_VER
#include <unistd.h>	// getopt defined here for unix
#endif
#include "params.h"
#include "memory.h"
#include "memdbg.h"

#include "sha2.h"
#include "base64.h"

const char *extension[] = {".kdbx"};
static char *keyfile = NULL;

// KeePass 1.x signature
uint32_t FileSignatureOld1 = 0x9AA2D903;
uint32_t FileSignatureOld2 = 0xB54BFB65;
/// <summary>
/// File identifier, first 32-bit value.
/// </summary>
uint32_t FileSignature1 = 0x9AA2D903;
/// <summary>
/// File identifier, second 32-bit value.
/// </summary>
uint32_t FileSignature2 = 0xB54BFB67;
// KeePass 2.x pre-release (alpha and beta) signature
uint32_t FileSignaturePreRelease1 = 0x9AA2D903;
uint32_t FileSignaturePreRelease2 = 0xB54BFB66;
uint32_t FileVersionCriticalMask = 0xFFFF0000;
/// <summary>
/// File version of files saved by the current <c>Kdb4File</c> class.
/// KeePass 2.07 has version 1.01, 2.08 has 1.02, 2.09 has 2.00,
/// 2.10 has 2.02, 2.11 has 2.04, 2.15 has 3.00.
/// The first 2 bytes are critical (i.e. loading will fail, if the
/// file version is too high), the last 2 bytes are informational.
/// </summary>
uint32_t FileVersion32 = 0x00030000;

enum Kdb4HeaderFieldID
{
	EndOfHeader = 0,
	MasterSeed = 4,
	TransformSeed = 5,
	TransformRounds = 6,
	EncryptionIV = 7,
	StreamStartBytes = 9,
};

static off_t get_file_size(char * filename)
{
	struct stat sb;
	if (stat(filename, & sb) != 0) {
		fprintf(stderr, "! %s : stat failed, %s\n", filename, strerror(errno));
		exit(-2);
	}
	return sb.st_size;
}

static void print_hex(unsigned char *str, int len)
{
	int i;
	for (i = 0; i < len; ++i)
		printf("%02x", str[i]);
}

static uint64_t BytesToUInt64(unsigned char * s, const int s_size)
{
	int i;
	uint64_t v = 0;

	for (i = 0; i < 8 && i < s_size; i++)
		v |= (uint64_t)s[i] << 8 * i;
	return v;
}

static uint32_t fget32(FILE * fp)
{
	uint32_t v = fgetc(fp);
	v |= fgetc(fp) << 8;
	v |= fgetc(fp) << 16;
	v |= fgetc(fp) << 24;
	return v;
}

static uint16_t fget16(FILE * fp)
{
	uint32_t v = fgetc(fp);
	v |= fgetc(fp) << 8;
	return v;
}

static void warn_exit(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (fmt != NULL)
		vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");

	exit(EXIT_FAILURE);
}

/* process KeePass 1.x databases */
static void process_old_database(FILE *fp, char* encryptedDatabase)
{
	uint32_t enc_flag;
	uint32_t version;
	unsigned char final_randomseed[16];
	unsigned char enc_iv[16];
	unsigned char contents_hash[32];
	unsigned char transf_randomseed[32];
	uint32_t num_groups;
	uint32_t num_entries;
	uint32_t key_transf_rounds;
	unsigned char *buffer;
	long long filesize = 0;
	long long datasize;
	int algorithm = -1;
	char *dbname;
	FILE *kfp = NULL;

	/* specific to keyfile handling */
	long long filesize_keyfile = 0;
	SHA256_CTX ctx;
	unsigned char hash[32];
	int counter;

	enc_flag = fget32(fp);
	version = fget32(fp);

	if (fread(final_randomseed, 16, 1, fp) != 1)
		warn_exit("%s: Error: read failed: %s.", encryptedDatabase,
			strerror(errno));
	if (fread(enc_iv, 16, 1, fp) != 1)
		warn_exit("%s: Error: read failed: %s.", encryptedDatabase,
			strerror(errno));

	num_groups = fget32(fp);
	num_entries = fget32(fp);
	(void)num_groups;
	(void)num_entries;

	if (fread(contents_hash, 32, 1, fp) != 1)
		warn_exit("%s: Error: read failed: %s.", encryptedDatabase,
			strerror(errno));
	if (fread(transf_randomseed, 32, 1, fp) != 1)
		warn_exit("%s: Error: read failed: %s.", encryptedDatabase,
			strerror(errno));

	key_transf_rounds = fget32(fp);
	/* Check if the database is supported */
	if ((version & 0xFFFFFF00) != (0x00030002 & 0xFFFFFF00)) {
		fprintf(stderr, "! %s : Unsupported file version!\n", encryptedDatabase);
		return;
	}
	/* src/Kdb3Database.cpp from KeePass 0.4.3 is authoritative */
	if (enc_flag & 2) {
		algorithm = 0; // AES
	} else if (enc_flag & 8) {
		algorithm = 1; // Twofish
	} else {
		fprintf(stderr, "! %s : Unsupported file encryption!\n", encryptedDatabase);
		return;
	}

	/* keyfile processing */
	if (keyfile) {
		kfp = fopen(keyfile, "rb");
		if (!kfp) {
			fprintf(stderr, "! %s : %s\n", keyfile, strerror(errno));
			return;
		}
		filesize_keyfile = (long long)get_file_size(keyfile);
	}

	dbname = strip_suffixes(basename(encryptedDatabase), extension, 1);
	// offset (124) field below is not used, we hijack it to convey the
	// algorithm.
	// printf("%s:$keepass$*1*%d*%d*", dbname, key_transf_rounds, 124);
	printf("%s:$keepass$*1*%d*%d*", dbname, key_transf_rounds, algorithm);
	print_hex(final_randomseed, 16);
	printf("*");
	print_hex(transf_randomseed, 32);
	printf("*");
	print_hex(enc_iv, 16);
	printf("*");
	print_hex(contents_hash, 32);
	filesize = (long long)get_file_size(encryptedDatabase);
	datasize = filesize - 124;

	buffer = (unsigned char*) mem_alloc (datasize * sizeof(char));

	/* we inline the content with the hash */
	fprintf(stderr, "Inlining %s\n", encryptedDatabase);
	printf("*1*"LLd"*", datasize);
	fseek(fp, 124, SEEK_SET);
	if (fread(buffer, datasize, 1, fp) != 1)
		warn_exit("%s: Error: read failed: %s.",
		          encryptedDatabase, strerror(errno));

	print_hex(buffer, datasize);
	MEM_FREE(buffer);

	if (keyfile) {
		buffer = (unsigned char*) mem_alloc (filesize_keyfile * sizeof(char));
		printf("*1*64*"); /* inline keyfile content */
		if (fread(buffer, filesize_keyfile, 1, kfp) != 1)
			warn_exit("%s: Error: read failed: %s.",
				encryptedDatabase, strerror(errno));

		/* as in Keepass 1.x implementation:
		 *  if filesize_keyfile == 32 then assume byte_array
		 *  if filesize_keyfile == 64 then assume hex(byte_array)
		 *  else byte_array = sha256(keyfile_content)
		 */

		if (filesize_keyfile == 32)
			print_hex(buffer, filesize_keyfile);
		else if (filesize_keyfile == 64){
			for (counter = 0; counter <64; counter++)
				printf ("%c", buffer[counter]);
		}
		else{
		  /* precompute sha256 to speed-up cracking */
		  SHA256_Init(&ctx);
		  SHA256_Update(&ctx, buffer, filesize_keyfile);
		  SHA256_Final(hash, &ctx);
		  print_hex(hash, 32);
		}
		MEM_FREE(buffer);
	}
	printf("\n");
}


static void process_database(char* encryptedDatabase)
{
	long dataStartOffset;
	unsigned long transformRounds = 0;
	unsigned char *masterSeed = NULL;
	int masterSeedLength = 0;
	unsigned char *transformSeed = NULL;
	int transformSeedLength = 0;
	unsigned char *initializationVectors = NULL;
	int initializationVectorsLength = 0;
	unsigned char *expectedStartBytes = NULL;
	int endReached, expectedStartBytesLength = 0;
	uint32_t uSig1, uSig2, uVersion;
	FILE *fp;
	unsigned char out[32];
	char *dbname;

	/* specific to keyfile handling */
	unsigned char *buffer;
	long long filesize_keyfile = 0;
	char *p;
	char *data;
	char b64_decoded[64];
	FILE *kfp = NULL;
	SHA256_CTX ctx;
	unsigned char hash[32];
	int counter;

	fp = fopen(encryptedDatabase, "rb");
	if (!fp) {
		fprintf(stderr, "! %s : %s\n", encryptedDatabase, strerror(errno));
		return;
	}
	uSig1 = fget32(fp);
	uSig2 = fget32(fp);
	if ((uSig1 == FileSignatureOld1) && (uSig2 == FileSignatureOld2)) {
		process_old_database(fp, encryptedDatabase);
		fclose(fp);
		return;
	}
	if ((uSig1 == FileSignature1) && (uSig2 == FileSignature2)) {
	}
	else if ((uSig1 == FileSignaturePreRelease1) && (uSig2 == FileSignaturePreRelease2)) {
	}
	else {
		fprintf(stderr, "! %s : Unknown format: File signature invalid\n", encryptedDatabase);
		fclose(fp);
		return;
	}
	uVersion = fget32(fp);
	if ((uVersion & FileVersionCriticalMask) > (FileVersion32 & FileVersionCriticalMask)) {
		fprintf(stderr, "! %s : Unknown format: File version unsupported\n", encryptedDatabase);
		fclose(fp);
		return;
	}
	endReached = 0;
	while (!endReached)
	{
		unsigned char btFieldID = fgetc(fp);
		uint16_t uSize = fget16(fp);
		enum Kdb4HeaderFieldID kdbID;
		unsigned char *pbData = NULL;

		if (uSize > 0)
		{
			pbData = (unsigned char*)mem_alloc(uSize);
			if (fread(pbData, uSize, 1, fp) != 1) {
				fprintf(stderr, "error reading pbData\n");
				MEM_FREE(pbData);
				goto bailout;
			}
		}
		kdbID = btFieldID;
		switch (kdbID)
		{
			case EndOfHeader:
				endReached = 1;  // end of header
				MEM_FREE(pbData);
				break;

			case MasterSeed:
				if (masterSeed)
					MEM_FREE(masterSeed);
				masterSeed = pbData;
				masterSeedLength = uSize;
				break;

			case TransformSeed:
				if (transformSeed)
					MEM_FREE(transformSeed);

				transformSeed = pbData;
				transformSeedLength = uSize;
				break;

			case TransformRounds:
				if (!pbData) {
					fprintf(stderr, "! %s : parsing failed (pbData is NULL), please open a bug if target is valid KeepPass database.\n", encryptedDatabase);
					goto bailout;
				}
				else {
					transformRounds = BytesToUInt64(pbData, uSize);
					MEM_FREE(pbData);
				}
				break;

			case EncryptionIV:
				if (initializationVectors)
					MEM_FREE(initializationVectors);
				initializationVectors = pbData;
				initializationVectorsLength = uSize;
				break;

			case StreamStartBytes:
				if (expectedStartBytes)
					MEM_FREE(expectedStartBytes);
				expectedStartBytes = pbData;
				expectedStartBytesLength = uSize;
				break;

			default:
				MEM_FREE(pbData);
				break;
		}
	}
	dataStartOffset = ftell(fp);
	if (transformRounds == 0) {
		fprintf(stderr, "! %s : transformRounds can't be 0\n", encryptedDatabase);
		goto bailout;
	}
#ifdef KEEPASS_DEBUG
	fprintf(stderr, "%d, %d, %d, %d\n", masterSeedLength, transformSeedLength, initializationVectorsLength, expectedStartBytesLength);
#endif
	if (!masterSeed || !transformSeed || !initializationVectors || !expectedStartBytes) {
		fprintf(stderr, "! %s : parsing failed, please open a bug if target is valid KeepPass database.\n", encryptedDatabase);
		goto bailout;
	}

	if (keyfile) {
		kfp = fopen(keyfile, "rb");
		if (!kfp) {
			fprintf(stderr, "! %s : %s\n", keyfile, strerror(errno));
			return;
		}
		filesize_keyfile = (long long)get_file_size(keyfile);
	}

	dbname = strip_suffixes(basename(encryptedDatabase),extension, 1);
	printf("%s:$keepass$*2*%ld*%ld*",dbname, transformRounds, dataStartOffset);
	print_hex(masterSeed, masterSeedLength);
	printf("*");
	print_hex(transformSeed, transformSeedLength);
	printf("*");
	print_hex(initializationVectors, initializationVectorsLength);
	printf("*");
	print_hex(expectedStartBytes, expectedStartBytesLength);
	if (fread(out, 32, 1, fp) != 1) {
		fprintf(stderr, "error reading encrypted data!\n");
		goto bailout;
	}
	printf("*");
	print_hex(out, 32);

	if (keyfile) {
		buffer = (unsigned char*) mem_alloc (filesize_keyfile * sizeof(char));
		printf("*1*64*"); /* inline keyfile content */
		if (fread(buffer, filesize_keyfile, 1, kfp) != 1)
			warn_exit("%s: Error: read failed: %s.",
				encryptedDatabase, strerror(errno));

		/* as in Keepass 2.x implementation:
		 *  if keyfile is an xml, get <Data> content
		 *  if filesize_keyfile == 32 then assume byte_array
		 *  if filesize_keyfile == 64 then assume hex(byte_array)
		 *  else byte_array = sha256(keyfile_content)
		 */

		if (!memcmp((char *) buffer, "<?xml", 5)
			&& ((p = strstr((char *) buffer, "<Key>")) != NULL)
			&& ((p = strstr(p, "<Data>")) != NULL)
			)
		{
			p += strlen("<Data>");
			data = p;
			p = strstr(p, "</Data>");
			base64_decode(data, p - data, b64_decoded);
			print_hex((unsigned char *) b64_decoded, 32);
		}
		else if (filesize_keyfile == 32)
			print_hex(buffer, filesize_keyfile);
		else if (filesize_keyfile == 64)
		{
			for (counter = 0; counter <64; counter++)
				printf ("%c", buffer[counter]);
		}
		else
		{
		  /* precompute sha256 to speed-up cracking */

		  SHA256_Init(&ctx);
		  SHA256_Update(&ctx, buffer, filesize_keyfile);
		  SHA256_Final(hash, &ctx);
		  print_hex(hash, 32);
		}
		MEM_FREE(buffer);
	}
	printf("\n");

bailout:
	MEM_FREE(masterSeed);
	MEM_FREE(transformSeed);
	MEM_FREE(initializationVectors);
	MEM_FREE(expectedStartBytes);
	fclose(fp);
}

static int usage(char *name)
{
	fprintf(stderr, "Usage: %s [-k <keyfile>] <.kdbx database(s)>\n", name);

	return EXIT_FAILURE;
}

int keepass2john(int argc, char **argv)
{
	int c;

	errno = 0;
	/* Parse command line */
	while ((c = getopt(argc, argv, "k:")) != -1) {
		switch (c) {
		case 'k':
			keyfile = (char *)mem_alloc(strlen(optarg) + 1);
			strcpy(keyfile, optarg);
			break;
		case '?':
		default:
			return usage(argv[0]);
		}
	}
	argc -= optind;
	if (argc == 0)
		return usage(argv[0]);
	argv += optind;

	while(argc--)
		process_database(*argv++);

	MEMDBG_PROGRAM_EXIT_CHECKS(stderr);
	return 0;
}
