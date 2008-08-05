/*
    FuseCompress
    Copyright (C) 2005 Milan Svoboda <milan.svoboda@centrum.cz>
*/

extern int min_filesize_background;
extern int min_filesize_direct;

extern pthread_t pt_comp;

extern pthread_mutexattr_t locktype;

extern compressor_t *compressor_default;
extern compressor_t *compressors[4];
extern char *uncompressible[];

extern database_t database;
extern database_t comp_database;

void *thread_compress(void *arg);

#define TEMP "._.tmp"		/* Template is: ._.tmpXXXXXX */
#define FUSE ".fuse_hidden"	/* Temporary FUSE file */

#define COMPRESSLEVEL_BACKGROUND "wb9" /* See above, this is for background compress */

// Gcc optimizations
//
#if __GNUC__ >= 3
# define likely(x)	__builtin_expect (!!(x), 1)
# define unlikely(x)	__builtin_expect (!!(x), 0)
#else
# define likely(x)	(x)
# define unlikely(x)	(x)
#endif
