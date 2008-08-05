#!/bin/bash -e
gcc -o truncorrupt truncorrupt.c
mkdir test
../fusecompress -c lzma test
cd test
../truncorrupt
cd ..
fusermount -u test
rm -fr test
