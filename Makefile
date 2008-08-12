CFLAGS  += $(CDEBUG) -D_REENTRANT -O2 -D_FILE_OFFSET_BITS=64 -Wall -D_POSIX_C_SOURCE=200112L -D_POSIX_SOURCE -D_SVID_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=500 -g
LDFLAGS += $(LDEBUG) -lz -lbz2 -llzma -llzo2

all:
	@echo "specify \"make release\", \"make debug\""

help:
	@echo "specify \"make release\", \"make debug\""

%.d: %.c
	@set -e; rm -f $@; $(CC) -M $(CFLAGS) $< > $@.$$$$; sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; rm -f $@.$$$$

sources_common := compress_bz2.c compress_gz.c compress_lzma.c compress_lzo.c compress_null.c file.c globals.c minilzo/lzo.c
sources_fs := background_compress.c compress.c direct_compress.c fusecompress.c log.c
sources_fsck := tools/fsck.c
sources_offline := tools/offline.c

objects_common := $(sources_common:.c=.o)
objects_fs := $(sources_fs:.c=.o)
objects_fsck := $(sources_fsck:.c=.o)
objects_offline := $(sources_offline:.c=.o)

depends := $(sources_common:.c=.d) $(sources_fsck:.c=.d) $(sources_fs:.c=.d) $(sources_offline:.c=.d)

-include $(depends)

debug: fusecompress fsck.fusecompress fusecompress_offline
debug: CDEBUG=-DDEBUG
debug: LDEBUG=

release: fusecompress fsck.fusecompress fusecompress_offline
release: CDEBUG=-DNDEBUG

fusecompress: $(objects_common) $(objects_fs)
	$(CC) $(CFLAGS) -o $@ $(objects_common) $(objects_fs) -lpthread -lfuse $(LDFLAGS)

fsck.fusecompress: $(objects_common) $(objects_fsck)
	$(CC) $(CFLAGS) -o $@ $(objects_common) $(objects_fsck) $(LDFLAGS)

fusecompress_offline: $(objects_common) $(objects_offline)
	$(CC) $(CFLAGS) -o $@ $(objects_common) $(objects_offline) $(LDFLAGS)

clean:
	rm -f $(objects_common) $(objects_fs) $(objects_fsck) $(objects_offline) tools/*.o
	rm -f $(depends)
	rm -f fusecompress fsck

test: debug
	(cd test ; sh run_tests)
