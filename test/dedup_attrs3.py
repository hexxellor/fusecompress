# make sure attributes from attr file are preserved when deduping

# This can happen if a file was a duplicate before but is now the only
# instance remaining, and then a new file with the same content is created.
# If this new file is found first, the old file will be dedup-linked to
# the new one. In this case, it is important that the attributes from
# the attribute file are preserved, not the ones from the actual file

import os
import shutil
import sys
import time
import stat

if not 'deduplication' in os.popen('../fsck.fusecompress --help 2>&1').read():
  sys.exit(2)

os.mkdir('test')

text = 'foobar'

open('test/1', 'w').write(text)
os.chmod('test/1', 0647)
open('test/2', 'w').write(text)
os.chmod('test/2', 0600)
open('test/._fCat_2', 'w')	# attr file
os.chmod('test/._fCat_2', 0666)

assert(os.system('strace -f ../fsck.fusecompress -rlv test') == 0)

# make sure deduping has been done
assert(os.lstat('test/1').st_nlink == 2)
assert(os.lstat('test/2').st_nlink == 2)

# check that attr file for 2 has not been changed
stat1 = os.lstat('test/1')
stat2 = os.lstat('test/._fCat_2')
assert(stat.S_IMODE(stat1.st_mode) == 0647)
assert(stat.S_IMODE(stat2.st_mode) == 0666)

shutil.rmtree('test')
sys.exit(0)
