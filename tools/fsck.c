#include "../structs.h"
#include "../compress.h"
#include "../compress_lzo.h"
#include "../compress_lzma.h"
#include "../compress_bz2.h"
#include "../compress_gz.h"
#include "../file.h"

#include <ftw.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#define OK 0
#define BROKEN_HEADER 1
#define FAIL_OPEN 2
#define FAIL_READ 3
#define FAIL_OPEN_DECOMP 4
#define FAIL_READ_DECOMP 5
#define SHORT_READ_DECOMP 6
#define FAIL_CLOSE_DECOMP 7

const char compresslevel[] = "wbx";
int compress_testcancel(void* x) {
  return 0;
}

const unsigned char magic[] = {037, 0135, 0211};

int verbose = 0;
int unlink_enabled = 0;
int errors_found = 0;
int errors_fixed = 0;

void do_unlink(const char* fpath)
{
    if(unlink_enabled) {
      fprintf(stderr, "removing %s\n",fpath);
      unlink(fpath);
      errors_fixed++;
    }
    else if(verbose)
      fprintf(stderr, "not removing %s (disabled)\n", fpath);
}

int fix(int fd, const char* fpath, int error)
{
    errors_found++;
    switch(error) {
      case BROKEN_HEADER:
        fprintf(stderr,"%s: broken header\n", fpath);
        do_unlink(fpath);
        break;

      case FAIL_READ_DECOMP:
      case SHORT_READ_DECOMP:
        fprintf(stderr,"%s: read error while decompressing\n", fpath);
        do_unlink(fpath);
        break;
        
      default:
        fprintf(stderr,"unknown error %d, ignored\n",error);
        break;
    }
    close(fd);
    return 0;
}

int checkfile(const char* fpath, const struct stat* sb, int typeflag)
{
    int fd, res;
    unsigned char m[3];
    compressor_t* compr;
    off_t size;
    void* handle;
    char buf[131072];
    
    if(typeflag != FTW_F) return 0;	/* no regular file */
    
    if(verbose) fprintf(stderr,"checking file %s\n", fpath);
    
    fd = open(fpath, O_RDONLY);
    if(fd < 0) {
      return fix(fd, fpath, FAIL_OPEN);
    }
    if(read(fd, m, 3) < 0) {
      return fix(fd, fpath, FAIL_READ);
    }
    if(!memcmp(m, magic, 3)) {
      /* compressed file */
      lseek(fd, 0, SEEK_SET);
      res = file_read_header_fd(fd, &compr, &size);
      if(res == FAIL) {
        return fix(fd, fpath, BROKEN_HEADER);
      }
      handle = compr->open(fd, "r");
      if(!handle) {
        return fix(fd, fpath, FAIL_OPEN_DECOMP);
      }
      while(size) {
        res = compr->read(handle, buf, size > 131072 ? 131072 : size);
        if(res < 0) {
          return fix(fd, fpath, FAIL_READ_DECOMP);
        }
        if(res == 0 && size) {
          return fix(fd, fpath, SHORT_READ_DECOMP);
        }
        size -= res;
      }
      if(compr->close(handle) < 0) {
        return fix(fd, fpath, FAIL_CLOSE_DECOMP);
      }
    }
    else if(verbose) fprintf(stderr,"uncompressed file, skipping\n");
    
    close(fd);
    return 0;
}

void usage(char* n) {
    fprintf(stderr,"Usage: %s [-dv] directory\n\n", n);
    fprintf(stderr," -d\tRemove broken files\n");
    fprintf(stderr," -v\tBe verbose\n");
    exit(1);
}

int main(int argc, char** argv)
{
    int next_option;
    do {
        next_option = getopt(argc, argv, "dv");
        switch(next_option) {
          case 'd':
            unlink_enabled = 1;
            break;
          case 'v':
            verbose = 1;
            break;
          case -1:
            break;
          default:
            usage(argv[0]);
            break;
        }
    } while(next_option != -1);
    
    if(optind >= argc) {
      usage(argv[0]);
    }
    
    if(ftw(argv[optind], checkfile, 400) < 0) {
      perror("ftw");
      exit(1);
    }
    if(!errors_found) return 0;	/* no errors found */
    if(errors_found > errors_fixed) return 4;	/* some errors unfixed */
    return 1;	/* errors found and fixed */
}
