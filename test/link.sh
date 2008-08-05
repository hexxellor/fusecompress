#!/bin/bash -e
cc -o link link.c
mkdir test
../fusecompress test
cd test
../link
cd ..
fusermount -u test
rm -fr test
