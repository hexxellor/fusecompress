## Process this file with automake to produce Makefile.in

AM_CPPFLAGS =  -D_REENTRANT -D_FILE_OFFSET_BITS=64 -D_POSIX_SOURCE -D_POSIX_C_SOURCE=199309L -D_SVID_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700 -D_GNU_SOURCE
if CONFIG_OSX
AM_CPPFLAGS += -D_DARWIN_C_SOURCE -D__FreeBSD__=10
endif

bin_PROGRAMS = fusecompress fusecompress_offline fsck.fusecompress

common_sources = compress_null.c globals.c file.c log.c
if HAVE_ZLIB
common_sources += compress_gz.c
endif
if HAVE_BZIP2
common_sources += compress_bz2.c
endif
if HAVE_LZMA
common_sources += compress_lzma.c
endif
if HAVE_LZO2
common_sources += compress_lzo.c minilzo/lzo.c
endif
if DEBUG
AM_CPPFLAGS += -DDEBUG
else
AM_CPPFLAGS += -DNDEBUG
endif
if DEDUP
common_sources += dedup.c
AM_CPPFLAGS += -DWITH_DEDUP
endif

fusecompress_SOURCES = $(common_sources) background_compress.c fusecompress.c compress.c direct_compress.c
fusecompress_offline_SOURCES = $(common_sources) tools/offline.c
fsck_fusecompress_SOURCES = $(common_sources) tools/fsck.c

#fusecompress_LDADD = $(FUSECOMPRESS_LIBS)
#fusecompress_CFLAGS = $(FUSECOMPRESS_CFLAGS)

EXTRA_DIST = BUGS

indent:
	astyle -t8 --brackets=stroustrup --break-closing-brackets --indent-switches --pad-oper --pad-header --unpad-paren --align-pointer=name *.c */*.c *.h */*.h
