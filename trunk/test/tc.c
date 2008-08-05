#include "structs.h"
#include "compress_lzma.h"
#include <fcntl.h>
#include <stdlib.h>

#define BUF_SIZE 4096
int compress_testcancel(void* v)
{
  return 0;
}
int main(int argc, char** argv)
{
  int a = open("in",O_RDONLY);
  int b = open("out",O_WRONLY|O_CREAT|O_TRUNC,0644); 
  char buf[BUF_SIZE];
  void* bf = module_lzma.open(b,"wb");
  int r;
  while((r = read(a, buf, BUF_SIZE)) > 0) {
    if ((r = module_lzma.write(bf, buf, r)) < 0)
      exit(r);
  }
  module_lzma.close(bf);
  close(b);
  close(a);
  
  a = open("out", O_RDONLY);
  b = open("back_in", O_WRONLY|O_CREAT|O_TRUNC,0644);
  void* af = module_lzma.open(a,"rb");
  while((r = module_lzma.read(bf, buf, BUF_SIZE)) > 0) {
    write(b, buf, r);
  }
  close(b);
  module_lzma.close(af);
  close(a);

  a = open("back_in", O_RDONLY);
  b = open("back_out", O_WRONLY|O_CREAT|O_TRUNC,0644);
  module_lzma.compress(NULL,a,b);
  close(a);
  close(b);
  
  a = open("back_out", O_RDONLY);
  b = open("and_in_again", O_WRONLY|O_CREAT|O_TRUNC,0644);
  module_lzma.decompress(a, b);
  close(a);
  close(b);
  
  return 0;
}
