/*
    FuseCompress
    Copyright (C) 2005 Milan Svoboda <milan.svoboda@centrum.cz>
    (C) 2009, 2011 Ulrich Hecht <uli@suse.de>
*/

#include "config.h"

#include <pthread.h>
#include <sys/types.h>
#include <assert.h>

#include "structs.h"
#include "compress.h"

pthread_t           pt_comp;	/* compress thread */
pthread_mutexattr_t locktype;

// Files smaller than this are not compressed
//
int min_filesize_background;

int read_only;	/* set if mounted read-only to avoid temporary decompression of files */
int cache_decompressed_data;
int decomp_cache_size;
int max_decomp_cache_size;

int dedup_enabled;

compressor_t *compressor_default = NULL;

// Table of supported compressors. This is array and
// it is vital to sort modules according it's type. E.g. module_null
// has type 0x0 or module_gzip has type 0x02.
//
compressor_t *compressors[5] = {
	&module_null,
#ifdef HAVE_BZIP2
	&module_bz2,
#else
	NULL,
#endif
#ifdef HAVE_ZLIB
	&module_gzip,
#else
	NULL,
#endif
#ifdef HAVE_LZO2
	&module_lzo,
#else
	NULL,
#endif
#ifdef HAVE_LZMA
	&module_lzma,
#else
	NULL,
#endif
};

char *incompressible[] = {
    "mp3", "ogg",
    "avi", "mov", "mpg", "mp4", "mkv", "asf", "flv",
    "gz", "bz2", "zip", "tgz", "lzo", "lzma", "rar", "ace", "7z", "lha",
    "lzh", "chm", "lrz",
    "jpg", "png", "gif",
    "rpm", "deb",
    NULL
};

char **user_incompressible = NULL;

int root_fs;	/* set if you do not want to compress shared objects or binaries in mmapped_dirs[] */
char *mmapped_dirs[] = {
    "bin/", "sbin/", "usr/bin/", "usr/sbin/", NULL
};

size_t dont_compress_beyond; /* maximum size of files to compress in the bg compress thread */

database_t database = {
	.head = LIST_HEAD_INIT(database.head),
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,		// Not used
	.entries = 0,
};

database_t comp_database = {
	.head = LIST_HEAD_INIT(comp_database.head),
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,		// When new item is added to the list
	.entries = 0,
};

#ifdef WITH_DEDUP
database_t dedup_database = {
	.head = LIST_HEAD_INIT(dedup_database.head),
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,		// When new item is added to the list
	.entries = 0,
};
#endif

#ifdef DEBUG
int _debug_on = 1;
#endif
