
/**
 * Copyright © 2016 - 2017 Tino Reichardt
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You can contact the author at:
 * - zstdmt source repository: https://github.com/mcmilk/zstdmt
 */

/**
 * gzip compatible wrapper for the compression types of this library
 */

#include "platform.h"

#define MODE_COMPRESS    1	/* -z (default) */
#define MODE_DECOMPRESS  2	/* -d */
#define MODE_LIST        3	/* -l */
#define MODE_TEST        4	/* -t */
static int opt_mode = MODE_COMPRESS;

/* for the -i option */
#define MAX_ITERATIONS   1000
static int opt_iterations = 1;

/* exit codes */
#define E_OK      0
#define E_ERROR   1
#define E_WARNING 2
static int exit_code = E_OK;

static int opt_stdout = 0;
static int opt_level = 3;
static int opt_force = 0;
static int opt_keep = 0;
static int opt_threads;

/* 0 = quiet | 1 = normal | >1 = verbose */
static int opt_verbose = 1;
static int opt_bufsize = 0;
static int opt_timings = 0;
static int opt_nocrc = 0;

static char *progname;
static char *opt_suffix = SUFFIX;
static const char *errmsg = 0;

/* pointer to current infile, outfile */
static FILE *fin = NULL;
static FILE *fout = NULL;
static size_t bytes_read = 0;
static size_t bytes_written = 0;

/* when set, do not change fout */
static int global_fout = 0;

static MT_CCtx *cctx = 0;
static MT_DCtx *dctx = 0;

/* for -l with verbose > 1 */
static time_t mtime;
static unsigned int crc = 0;
static unsigned int crc32_table[1][256];
static unsigned int crc32(const unsigned char *buf, size_t size,
			  unsigned int crc);

static void panic(const char *msg)
{
	if (opt_verbose)
		fprintf(stderr, "%s\n", msg);
	exit(1);
}

static void version(void)
{
	printf("%s version %s, zstdmt v0.4\n"
	       "\nCopyright © 2016 - 2017 Tino Reichardt" "\n"
	       "\n", progname, VERSION);
	exit(0);
}

static void license(void)
{
	printf("\n %s version %s\n"
	       "\n Copyright © 2016 - 2017 Tino Reichardt"
	       "\n "
	       "\n This program is free software; you can redistribute it and/or modify"
	       "\n it under the terms of the GNU General Public License Version 2, as"
	       "\n published by the Free Software Foundation."
	       "\n "
	       "\n This program is distributed in the hope that it will be useful,"
	       "\n but WITHOUT ANY WARRANTY; without even the implied warranty of"
	       "\n MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the"
	       "\n GNU General Public License for more details."
	       "\n "
	       "\n Report bugs to: https://github.com/mcmilk/zstdmt/issues"
	       "\n", progname, VERSION);
	exit(0);
}

static void usage(void)
{
	printf("\n Usage: %s [OPTION]... [FILE]..."
	       "\n Compress or uncompress FILEs (by default, compress FILES in-place)."
	       "\n"
	       "\n Standard Options:"
	       "\n  -#    Set compression level to # (%d-%d, default:%d)."
	       "\n  -c    Force write to standard output."
	       "\n  -d    Use decompress mode."
	       "\n  -z    Use compress mode."
	       "\n  -f    Force overwriting files and/or compression."
	       "\n  -h    Display a help screen and quit."
	       "\n  -k    Keep input files after compression or decompression."
	       "\n  -l    List information for the specified compressed files."
	       "\n  -L    Display License and quit."
	       "\n  -q    Be quiet: suppress all messages."
	       "\n  -S X  Use suffix 'X' for compressed files. Default: \"%s\""
	       "\n  -t    Test the integrity of each file leaving any files intact."
	       "\n  -v    Be more verbose."
	       "\n  -V    Show version information and quit."
	       "\n"
	       "\n Additional Options:"
	       "\n  -T N  Set number of (de)compression threads (def: #cores)."
	       "\n  -b N  Set input chunksize to N MiB (default: auto)."
	       "\n  -i N  Set number of iterations for testing (default: 1)."
	       "\n  -H    Print headline for the timing values and quit."
	       "\n  -B    Print timings and memory usage to stderr."
	       "\n  -C    Disable crc32 calculation in verbose listing mode."
	       "\n"
	       "\n If invoked as '%s', default action is to compress."
	       "\n             as '%s',  default action is to decompress."
	       "\n             as '%s', then: force decompress to stdout."
	       "\n"
	       "\n With no FILE, or when FILE is -, read standard input."
	       "\n"
	       "\n Report bugs to: https://github.com/mcmilk/zstdmt/issues"
	       "\n",
	       PROGNAME, LEVEL_MIN, LEVEL_MAX, LEVEL_DEF, SUFFIX,
	       PROGNAME, UNZIP, ZCAT);

	exit(0);
}

static void headline(void)
{
	if (opt_mode == MODE_LIST || opt_mode == MODE_TEST)
		fprintf(stderr,
			"Level;Threads;Real;User;Sys;MaxMem\n");
	else
		fprintf(stderr,
			"Level;Threads;InSize;OutSize;Frames;Real;User;Sys;MaxMem\n");
	exit(0);
}

static int ReadData(void *arg, MT_Buffer * in)
{
	FILE *fd = (FILE *) arg;
	size_t done = fread(in->buf, 1, in->size, fd);
	in->size = done;
	
	if (opt_verbose > 1)
		bytes_read += done;

	return 0;
}

static int WriteData(void *arg, MT_Buffer * out)
{
	FILE *fd = (FILE *) arg;
	ssize_t done = fwrite(out->buf, 1, out->size, fd);

	/* generate crc32 of uncompressed file */
	if (opt_mode == MODE_LIST && opt_verbose > 1)
		crc = crc32(out->buf, out->size, crc);
	/* printf("crc for %zu bytes, %8x\n", out->size, crc); */

	out->size = done;

	if (opt_verbose > 1)
		bytes_written += done;

	return 0;
}

/**
 * compress() - compress data from fin to fout
 *
 * return: 0 for ok, or errmsg on error
 */
static const char *do_compress(FILE * in, FILE * out)
{
	static int first = 1;
	MT_RdWr_t rdwr;
	size_t ret;

	/* 1) setup read/write functions */
	rdwr.fn_read = ReadData;
	rdwr.fn_write = WriteData;
	rdwr.arg_read = (void *)in;
	rdwr.arg_write = (void *)out;

	/* 2) create compression context */
	cctx = MT_createCCtx(opt_threads, opt_level, opt_bufsize);
	if (!cctx)
		return "Allocating compression context failed!";

	/* 3) compress */
	ret = MT_compressCCtx(cctx, &rdwr);
	if (MT_isError(ret))
		return MT_getErrorString(ret);

	/* 4) get statistic - XXX, find a better place ... */
	if (first && opt_timings) {
		fprintf(stderr, "%d;%d;%lu;%lu;%lu",
			opt_level, opt_threads,
			(unsigned long)MT_GetInsizeCCtx(cctx),
			(unsigned long)MT_GetOutsizeCCtx(cctx),
			(unsigned long)MT_GetFramesCCtx(cctx));
		first = 0;
	}

	MT_freeCCtx(cctx);

	return 0;
}

/**
 * decompress() - decompress data from fin to fout
 *
 * return: 0 for ok, or errmsg on error
 */
static const char *do_decompress(FILE * in, FILE * out)
{
	static int first = 1;
	MT_RdWr_t rdwr;
	size_t ret;

	/* 1) setup read/write functions */
	rdwr.fn_read = ReadData;
	rdwr.fn_write = WriteData;
	rdwr.arg_read = (void *)in;
	rdwr.arg_write = (void *)out;

	/* 2) create compression context */
	dctx = MT_createDCtx(opt_threads, opt_bufsize);
	if (!dctx)
		return "Allocating decompression context failed!";

	/* 3) compress */
	ret = MT_decompressDCtx(dctx, &rdwr);
	if (MT_isError(ret))
		return MT_getErrorString(ret);

	/* 4) get statistic - XXX, find better place */
	if (first && opt_timings) {
		fprintf(stderr, "%d;%d;%lu;%lu;%lu",
			0, opt_threads,
			(unsigned long)MT_GetInsizeDCtx(dctx),
			(unsigned long)MT_GetOutsizeDCtx(dctx),
			(unsigned long)MT_GetFramesDCtx(dctx));
		first = 0;
	}

	MT_freeDCtx(dctx);

	return 0;
}

static int has_suffix(const char *filename, const char *suffix)
{
	int flen = strlen(filename);
	int xlen = strlen(suffix);

	if (flen < xlen)
		return 0;

	if (strcmp(filename + flen - xlen, suffix) == 0)
		return 1;

	return 0;
}

static char *add_suffix(const char *filename)
{
	int flen = strlen(filename);
	int xlen = strlen(opt_suffix);
	char *newname = malloc(flen + xlen + 1);

	if (!newname)
		panic("nomem!");

	strcpy(newname, filename);
	strcat(newname, opt_suffix);

	return newname;
}

static char *remove_suffix(const char *filename)
{
	int flen = strlen(filename);
	int xlen = strlen(opt_suffix);
	char *newname = malloc(flen + xlen + 5);

	if (!newname)
		panic("nomem!");

	if (has_suffix(filename, opt_suffix)) {
		/* just remove the suffix */
		strcpy(newname, filename);
		newname[flen - xlen] = 0;
	} else {
		/* append .out to filename */
		strcpy(newname, filename);
		strcat(newname, ".out");
	}

	return newname;
}

static unsigned int crc32(const unsigned char *buf, size_t size,
			  unsigned int crc)
{
	static int initdone = 0;

	if (opt_nocrc)
		return 0;

	if (!initdone) {
		unsigned int b, i, r, poly32 = 0xEDB88320U;

		for (b = 0; b < 256; ++b) {
			r = b;
			for (i = 0; i < 8; ++i) {
				if (r & 1)
					r = (r >> 1) ^ poly32;
				else
					r >>= 1;
			}

			crc32_table[0][b] = r;
		}
		initdone++;
	}
	crc = ~crc;

	while (size != 0) {
		crc = crc32_table[0][*buf++ ^ (crc & 0xFF)] ^ (crc >> 8);
		--size;
	}

	return ~crc;
}

static void print_listmode(int headline, const char *filename)
{
	if (headline && opt_verbose > 1)
		printf("%8s %8s %10s %8s %20s %20s %7s %s\n",
		       "method", "crc32", "date", "time", "compressed",
		       "uncompressed", "ratio", "uncompressed_name");
	else if (headline)
		printf("%20s %20s %7s %s\n",
		       "compressed", "uncompressed", "ratio",
		       "uncompressed_name");

	if (errmsg) {
		if (opt_verbose == 1)
			printf("%20s %20s %7s %s\n", "-", "-", "-", filename);
		else if (opt_verbose > 1)
			printf("%8s %8s %10s %8s %20s %20s %7s %s\n",
			       "-", "-", "-", "-", "-", "-", "-", filename);
	}

	if (opt_verbose == 1) {
		printf("%20lu %20lu %6.2f%% %s\n",
		       (unsigned long)bytes_read,
		       (unsigned long)bytes_written,
		       100 - (double)bytes_read * 100 / bytes_written,
		       filename);
	} else if (opt_verbose > 1) {
		char buf[30];
		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S",
			 localtime(&mtime));
		printf("%8s %08x %12s %20lu %20lu %6.2f%% %s\n", METHOD, crc,
		       buf, (unsigned long)bytes_read,
		       (unsigned long)bytes_written,
		       100 - (double)bytes_read * 100 / bytes_written,
		       filename);
	}

	return;
}

static void print_testmode(const char *filename)
{
	printf(PROGNAME ": %s: %s\n", filename, errmsg ? errmsg : "OK");
}

static void check_stdout(void)
{
	if (IS_CONSOLE(fout) && !opt_force)
		panic("Data not written to terminal. Use -f to force!");
}

/**
 * check_infile() - check if file can be compressed
 *
 * return: zero on success, or errmsg
 */
static char *check_infile(const char *filename)
{
	struct stat s;
	int r;

	r = stat(filename, &s);
	if (r == -1)
		return strerror(errno);

	if (S_ISDIR(s.st_mode))
		return "Is a directory";

	if (S_ISREG(s.st_mode)) {
		mtime = s.st_mtime;
		return 0;
	}

	return "Is not regular file";
}

/**
 * check_overwrite() - check if file exists
 *
 * return: zero on success, or errmsg
 */
static char *check_overwrite(const char *filename)
{
	struct stat s;
	int r, c, yes = -1;

	/* force, so always okay */
	if (opt_force)
		return 0;

	/* test it ... */
	r = stat(filename, &s);
	if (r == -1 && errno == ENOENT)
		return 0;	/* ok, not found */

	if (r == -1)
		return strerror(errno);

	/* when we are here, we ask the user what to do */
	for (;;) {
		printf("%s: '%s' already exists. Overwrite (y/N) ? ",
		       progname, filename);
		c = getchar();

		if (c == 'y' || c == 'Y')
			yes = 1;
		if (c == 'n' || c == 'N')
			yes = 0;

		while (c != '\n' && c != EOF)
			c = getchar();

		if (yes != -1)
			break;
	}

	if (yes == 0 && opt_verbose) {
		return "not overwriting.";
	}

	remove(filename);
	return 0;
}

static void treat_stdin()
{
	const char *filename = "(stdin)";

	/* setup fin and fout */
	fin = stdin;
	if (!fout) {
		fout = stdout;
		check_stdout();
	}

	/* do some work */
	if (opt_mode == MODE_COMPRESS)
		errmsg = do_compress(fin, fout);
	else
		errmsg = do_decompress(fin, fout);

	/* remember, that we had some error */
	if (errmsg)
		exit_code = E_ERROR;

	/* listing mode */
	if (opt_mode == MODE_LIST)
		print_listmode(1, filename);

	/* testing mode */
	if (opt_mode == MODE_TEST && opt_verbose > 1)
		print_testmode(filename);

	return;
}

static void treat_file(char *filename)
{
	static int first = 1;
	FILE *local_fout = NULL;
	char *fn2 = 0;

	/* reset counter */
	bytes_written = bytes_read = 0;

	/* reset errmsg */
	errmsg = 0;
	crc = 0;

	/* setup fin stream */
	if (strcmp(filename, "-") == 0) {
		fin = stdin;
	} else {
		errmsg = check_infile(filename);
		if (errmsg && opt_verbose)
			fprintf(stderr, "%s: %s: %s\n",
				progname, filename, errmsg);
		if (errmsg)
			return;
		fin = fopen(filename, "rb");
	}

	/* setup fout stream */
	if (global_fout) {
		local_fout = fout;
		check_stdout();
	} else {
		/* setup input / output */
		switch (opt_mode) {
		case MODE_COMPRESS:
			if (has_suffix(filename, opt_suffix) && !opt_force) {
				fprintf(stderr,
					"%s already has %s suffix -- unchanged\n",
					filename, opt_suffix);
				return;
			}
			fn2 = add_suffix(filename);
			errmsg = check_overwrite(fn2);
			if (!errmsg)
				local_fout = fopen(fn2, "wb");
			break;
		case MODE_DECOMPRESS:
			fn2 = remove_suffix(filename);
			errmsg = check_overwrite(fn2);
			if (!errmsg)
				local_fout = fopen(fn2, "wb");
			break;
		}
	}

	if (!errmsg && fin == NULL)
		errmsg = "Opening source file failed.";

	if (!errmsg && local_fout == NULL)
		errmsg = "Opening destination file failed.";

	if (errmsg) {
		fprintf(stderr, "%s: %s: %s\n", progname, filename, errmsg);
		exit_code = E_WARNING;
		if (fn2)
			free(fn2);
		return;
	}

	/* do some work */
	if (!errmsg && opt_mode == MODE_COMPRESS)
		errmsg = do_compress(fin, local_fout);
	else
		errmsg = do_decompress(fin, local_fout);

	/* remember, that we had some error */
	if (errmsg)
		exit_code = E_ERROR;

	/* close instream */
	if (fin && fin != stdin)
		if (fclose(fin) != 0 && opt_verbose)
			fprintf(stderr, "Closing infile failed.");

	/* close outstream */
	if (!global_fout && local_fout != stdout)
		if (fclose(local_fout) != 0 && opt_verbose)
			fprintf(stderr, "Closing outfile failed.");

	/* listing mode */
	if (opt_mode == MODE_LIST)
		print_listmode(first, filename);

	/* testing mode */
	if (opt_mode == MODE_TEST && opt_verbose > 1)
		print_testmode(filename);

	/* remove input file */
	if (!errmsg && !opt_keep)
		remove(filename);

	/* remove outfile with errors */
	if (errmsg && !global_fout) {
		fprintf(stderr, errmsg);
		remove(fn2);
	}

	/* free, if allocated */
	if (fn2)
		free(fn2);

	first = 0;
	return;
}

int main(int argc, char **argv)
{
	/* default options: */
	struct rusage ru;
	struct timeval tms, tme, tm;
	int opt;		/* for getopt */
	int files;		/* number of files in cmdline */
	int levelnumbers = 0;

	/* get programm name */
	progname = strrchr(argv[0], '/');
	if (progname)
		++progname;
	else
		progname = argv[0];

	/* change defaults, if needed */
	if (strcmp(progname, UNZIP) == 0) {
		opt_mode = MODE_DECOMPRESS;
	} else if (strcmp(progname, ZCAT) == 0) {
		opt_mode = MODE_DECOMPRESS;
		opt_stdout = 1;
		opt_force = 1;
	}

	/* default: thread count = # cpu's */
	opt_threads = getcpucount();

	/* same order as in help option -h */
	while ((opt =
		getopt(argc, argv,
		       "1234567890cdzfhklLqrS:tvVT:b:i:HBC")) != -1) {
		switch (opt) {

			/* 1) Gzip Like Options: */
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			if (levelnumbers == 0)
				opt_level = 0;
			else
				opt_level *= 10;
			opt_level += ((int)opt - 48);
			levelnumbers++;
			break;

		case 'c':	/* force to stdout */
			opt_stdout = 1;
			break;

		case 'd':	/* mode = decompress */
			opt_mode = MODE_DECOMPRESS;
			break;

		case 'z':	/* mode = compress */
			opt_mode = MODE_COMPRESS;
			break;

		case 'f':	/* force overwriting */
			opt_force = 1;
			break;

		case 'h':	/* show help */
			usage();
			/* not reached */

		case 'k':	/* keep old files */
			opt_keep = 1;
			break;

		case 'l':	/* listing */
			opt_mode = MODE_LIST;
			opt_keep = 1;
			break;

		case 'L':	/* show License */
			license();
			/* not reached */

		case 'q':	/* be quiet */
			opt_verbose = 0;
			break;

		case 'S':	/* use specified suffix */
			opt_suffix = optarg;
			break;

		case 't':	/* testing */
			opt_mode = MODE_TEST;
			opt_keep = 1;
			break;

		case 'v':	/* be more verbose */
			opt_verbose++;
			break;

		case 'V':	/* version */
			version();
			/* not reached */

			/* 2) additional options */
		case 'T':	/* threads */
			opt_threads = atoi(optarg);
			break;

		case 'b':	/* input buffer in MB */
			opt_bufsize = atoi(optarg);
			break;

		case 'i':	/* iterations */
			opt_iterations = atoi(optarg);
			break;

		case 'H':	/* headline */
			headline();
			/* not reached */

		case 'B':	/* print timings */
			opt_timings = 1;
			break;

		case 'C':	/* disable crc32 in verbose listing */
			opt_nocrc = 1;
			break;

		default:
			usage();
			/* not reached */
		}
	}

	/**
	 * generic check of parameters
	 */

	/* make opt_level valid */
	if (opt_level < LEVEL_MIN)
		opt_level = LEVEL_MIN;
	else if (opt_level > LEVEL_MAX)
		opt_level = LEVEL_MAX;

	/* opt_threads = 1..THREAD_MAX */
	if (opt_threads < 1)
		opt_threads = 1;
	else if (opt_threads > THREAD_MAX)
		opt_threads = THREAD_MAX;

	/* opt_iterations = 1..MAX_ITERATIONS */
	if (opt_iterations < 1)
		opt_iterations = 1;
	else if (opt_iterations > MAX_ITERATIONS)
		opt_iterations = MAX_ITERATIONS;

	/* opt_bufsize is in MiB */
	if (opt_bufsize > 0)
		opt_bufsize *= 1024 * 1024;

	/* number of args, which are not options */
	files = argc - optind;

	/* -c was used */
	if (opt_stdout) {
		fout = stdout;
		global_fout = 1;
	}

	/* -l or -t given, then we write to /dev/null */
	if (opt_mode == MODE_LIST || opt_mode == MODE_TEST) {
		fout = fopen(DEVNULL, "wb");
		if (!fout)
			panic("Opening output file failed!");
		global_fout = 1;
	}

	/* begin timing */
	if (opt_timings)
		gettimeofday(&tms, NULL);

	/* main work */
	if (files == 0) {
		if (opt_iterations != 1)
			panic
			    ("You can not use stdin together with the -i option.");

		/* use stdin */
		treat_stdin();
	} else {
		/* use input files */
		for (;;) {
			files = optind;
			while (files < argc) {
				treat_file(argv[files++]);
			}
			opt_iterations--;
			if (opt_iterations == 0)
				break;
		}
	}

	/* show timings */
	if (opt_timings) {
		gettimeofday(&tme, NULL);
		timersub(&tme, &tms, &tm);
		getrusage(RUSAGE_SELF, &ru);
		fprintf(stderr, ";%ld.%ld;%ld.%ld;%ld.%ld;%ld\n",
			tm.tv_sec, tm.tv_usec / 1000,
			ru.ru_utime.tv_sec, ru.ru_utime.tv_usec / 1000,
			ru.ru_stime.tv_sec, ru.ru_stime.tv_usec / 1000,
			(long unsigned)ru.ru_maxrss);
	}

	/* exit should flush stdout / stderr */
	exit(exit_code);
}
