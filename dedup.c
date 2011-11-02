/* Data deduplication for fusecompress.
 *
 * Copyright (C) 2011 Ulrich Hecht <uli@suse.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "utils.h"
#include "dedup.h"
#include "log.h"
#include "globals.h"
#include "file.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utime.h>
#include <mhash.h>
#include <fcntl.h>

/** Hard-link filename to a file from the dedup database with the same MD5
 * hash.
 * @param md5 file content's 128-bit MD5 hash
 * @param filename file name
 */
void hardlink_file(unsigned char *md5, const char *filename)
{
  DEBUG_("looking for '%s' in md5 database", filename);
  /* search for entry with matching MD5 hash */
  LOCK(&dedup_database.lock);
  dedup_t* dp;

  int i;
  for (i = 0; i < DATABASE_HASH_SIZE; i++) {
    list_for_each_entry(dp, &dedup_database.head[i], list) {
      if (memcmp(md5, dp->md5, 16) == 0) {
        /* Check if this entry points to the file itself. */
        /* XXX: This is something that should not actually happen, although
           should at worst be a performance problem. */
        if(strcmp(filename, dp->filename) == 0) {
          DEBUG_("second run for '%s', ignoring", filename);
          UNLOCK(&dedup_database.lock);
          return;
        }

        DEBUG_("duping it up with the '%s' man", dp->filename);

        /* We cannot just unlink the duplicate file because some filesystems
           (Btrfs, NTFS) have severe limits on the number of hardlinks per
           directory or per inode; we therefore rename it first and only delete
           it once the link has actually been created. */
        char *tmpname = malloc(strlen(filename) + 12);
        sprintf(tmpname, "%s.%d", filename, getpid());
        if (rename(filename, tmpname)) {
          DEBUG_("renaming '%s' to '%s' failed", filename, tmpname);
          free(tmpname);
          UNLOCK(&dedup_database.lock);
          return;
        }

        /* Try to create the link. */
        if (link(dp->filename, filename)) {
          DEBUG_("creating hardlink from '%s' to '%s' failed", dp->filename, filename);
          if (rename(tmpname, filename)) {
            ERR_("failed to move original file back");
          }
        }
        else {
          /* Made it, now we can actually unlink the duplicate. */
          if (unlink(tmpname)) {
            ERR_("failed to unlink original file at '%s'", tmpname);
          }
        }
        free(tmpname);
        UNLOCK(&dedup_database.lock);
        return;
      }
    }
  }
  
  /* If we reach this point, we haven't found any duplicate for this file,
     so we add it as a new entry into the dedup database. */
  DEBUG_("unique file '%s', adding to dedup DB", filename);
  dp = (dedup_t *)malloc(sizeof(dedup_t));
  memcpy(dp->md5, md5, 16);
  dp->filename = strdup(filename);
  int len;
  dp->filename_hash = gethash(filename, &len);
  list_add_tail(&dp->list, &dedup_database.head[dp->filename_hash & DATABASE_HASH_MASK]);
  dedup_database.entries++;
  UNLOCK(&dedup_database.lock);
}

/** Attempt deduplication of file.
 * @param file File to be deduplicated.
 */
void do_dedup(file_t *file)
{
  NEED_LOCK(&file->lock);
  STAT_(STAT_DO_DEDUP);
  
  file->status |= DEDUPING;
  /* No need to keep the file under lock while calculating the hash; should
     it change in the meantime, we will be informed through file->status. */
  UNLOCK(&file->lock);
  
  /* Calculate MD5 hash. */
  MHASH mh = mhash_init(MHASH_MD5);
  int fd = open(file->filename, O_RDONLY);
  int count;
  int failed = 0;
  char buf[4096];
  for(;;) {
    count = read(fd, buf, 4096);
    if (count < 0) {
      DEBUG_("read failed on '%s' while deduping", file->filename);
      failed = 1;
      break;
    }
    if (count == 0)
      break;
    mhash(mh, buf, count);
    /* XXX: It would be good for performance to occasionally check
       file->status & CANCEL. */
  }
  unsigned char md5[16];
  mhash_deinit(mh, md5);
  close(fd);
  
  /* See if everything went fine. */
  LOCK(&file->lock);
  if (failed) {
    DEBUG_("deduping '%s' failed", file->filename);
    /* While this looks suspicious, it is not a deduplication error, so we
       don't have to do anything special. */
    return;
  }
  if (!(file->status & CANCEL)) {
    /* No failure, no cancellation; let's link to identical file from dedup DB. */
    DEBUG_("MD5 for '%s': %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
           file->filename, md5[0], md5[1], md5[2], md5[3], md5[4], md5[5], md5[6], md5[7], md5[8],
           md5[9], md5[10], md5[11], md5[12], md5[13], md5[14], md5[15]);
    hardlink_file(md5, file->filename);
    file->deduped = TRUE;
  }
  else {
    /* Acknowledge the cancellation. */
    DEBUG_("deduping '%s' cancelled", file->filename);
    file->status &= ~CANCEL;
    pthread_cond_broadcast(&file->cond);
  }
  file->status &= ~DEDUPING;
}

/** Reverse deduplication in case a hardlinked file is written to.
 * @param file File to be undeduplicated.
 */
int do_undedup(file_t *file)
{
  NEED_LOCK(&file->lock);
  DEBUG_("undeduping '%s'", file->filename);
  
  /* First, check if there is actually something to be done. */
  struct stat st;
  if (stat(file->filename, &st) < 0)
    return 0;
  if (st.st_nlink < 2)
    return 0;
  
  STAT_(STAT_DO_UNDEDUP);
  
  /* Check if we have enough space on the backing store to undedup. */
  struct statvfs stat;
  if(statvfs(file->filename, &stat) < 0) {
          errno = EIO;
          return FAIL;
  }
  if(stat.f_bsize * stat.f_bavail < st.st_size) {
          if(!(geteuid() == 0 && stat.f_bsize * stat.f_bfree >= st.st_size)) {
                  errno = ENOSPC;
                  return FAIL;
          }
  }
  
  /* XXX: Is this actually necessary? After all, we create an identical
     copy. */
  dedup_discard(file);
  
  /* Behold the shitload of code it takes to safely copy a file on Unix... */
  int fd_out;
  char *temp = file_create_temp(&fd_out);
  if (fd_out == -1) {
    ERR_("undedup: failed to create temp file");
    errno = EIO;
    return FAIL;
  }
  int fd_in = open(file->filename, O_RDONLY);
  if (fd_in == -1) {
    ERR_("undedup: failed to open input file '%s'", file->filename);
    close(fd_out);
    unlink(temp);
    free(temp);
    errno = EIO;
    return FAIL;
  }
  char buf[4096];
  for(;;) {
    int count = read(fd_in, buf, 4096);
    if (count < 0) {
      ERR_("undedup: read() failed on input file '%s'", file->filename);
      close(fd_in);
      close(fd_out);
      unlink(temp);
      free(temp);
      errno = EIO;
      return FAIL;
    }
    else if (count == 0) /* EOF */
      break;
    if (write(fd_out, buf, count) != count) {
      ERR_("undedup: write() failed on output file '%s'", temp);
      close(fd_in);
      close(fd_out);
      unlink(temp);
      free(temp);
      errno = EIO;
      return FAIL;
    }
  }
  close(fd_in);
  /* fix owner and mode of the new file */
  fchown(fd_out, st.st_uid, st.st_gid);
  fchmod(fd_out, st.st_mode);
  close(fd_out);
  
  /* Remove deduplicated link... */
  if (unlink(file->filename) < 0) {
    ERR_("undedup: failed to unlink '%s'", file->filename);
    unlink(temp);
    free(temp);
    errno = EIO;
    return FAIL;
  }
  /* ...and replace it with the new copy. */
  DEBUG_("renaming '%s' to '%s'", temp, file->filename);
  if (rename(temp, file->filename) < 0) {
    ERR_("undedup: failed to rename temp file '%s' to '%s'", temp, file->filename);
    free(temp);
    errno = EIO;
    return FAIL;
  }
  /* fix timestamps */
  struct utimbuf utbuf;
  utbuf.actime = st.st_atime;
  utbuf.modtime = st.st_mtime;
  if (utime(file->filename, &utbuf) < 0) {
    CRIT_("utime failed on '%s'!", file->filename);
    errno = EIO;
    return FAIL;
  }
  errno = 0;
  return 0;
}

/** Remove an entry from the deduplication DB.
 * This function must be called whenever a file's contents are modified.
 * @param file File to be removed.
 */
void dedup_discard(file_t *file)
{
  NEED_LOCK(&file->lock);

  if (file->deduped == FALSE) {
    /* This entry is not in the DB anyway. */
    return;
  }

  file->deduped = FALSE;
  DEBUG_("dedup_discard file '%s'", file->filename);
  STAT_(STAT_DEDUP_DISCARD);
  dedup_t* dp;
  int len;
  unsigned int hash = gethash(file->filename, &len);
  list_for_each_entry(dp, &dedup_database.head[hash & DATABASE_HASH_MASK], list) {
    if (dp->filename_hash == hash && !strcmp(dp->filename, file->filename)) {
      DEBUG_("found file '%s', discarding", file->filename);
      list_del(&dp->list);
      dedup_database.entries--;
      free(dp->filename);
      free(dp);
      return;
    }
  }
}

#define DEDUP_DB_FILE "._.fusecompress_dedup_db"
#define DEDUP_MAGIC "DEDUP"
#define DEDUP_MAGIC_SIZE (sizeof(DEDUP_MAGIC) - 1)
#define DEDUP_VERSION 1

/** Load the dedup DB saved when last mounted.
 * @param root Path to the backing filesystem's root.
 */
void dedup_load(const char *root)
{
  int i;
  for (i = 0; i < DATABASE_HASH_SIZE; i++) {
    dedup_database.head[i].prev = dedup_database.head[i].next = &(dedup_database.head[i]);
  }

  dedup_t *dp;

  /* we're not in the backing FS root yet, so we need to compose an
     absolute path */
  char fn[strlen(root) + 1 + strlen(DEDUP_DB_FILE) + 1];
  sprintf(fn, "%s/%s", root, DEDUP_DB_FILE);
  FILE *db_fp = fopen(fn, "r");
  if (!db_fp) {
    if (errno == ENOENT) {
      DEBUG_("no dedup DB found");
    }
    else {
      ERR_("failed to open dedup DB for reading: %s", strerror(errno));
    }
    return;
  }
  
  /* check header */
  char header[DEDUP_MAGIC_SIZE];
  uint16_t version;
  if (fread(header, DEDUP_MAGIC_SIZE, 1, db_fp) != 1)
    goto out;
  if (memcmp(header, DEDUP_MAGIC, DEDUP_MAGIC_SIZE)) {
    ERR_("dedup DB magic not found");
    goto out;
  }
  if (fread(&version, 2, 1, db_fp) != 1)
    goto out;
  if (version != DEDUP_VERSION) {
    DEBUG_("verion mismatch, ignoring dedup DB");
    goto out;
  }
  
  /* load data */
  uint32_t filename_length;
  /* every entry starts with the length of the filename */
  while (fread(&filename_length, 4, 1, db_fp) == 1) {
    /* allocate DB entry */
    dp = (dedup_t *)malloc(sizeof(dedup_t));
    /* allocate filename */
    dp->filename = (char *)malloc(filename_length + 1);
    /* read filename */
    if (fread(dp->filename, 1, filename_length, db_fp) != filename_length) {
      ERR_("failed to load filename of %d characters", filename_length);
      free(dp->filename);
      free(dp);
      goto out;
    }
    dp->filename[filename_length] = 0;	/* string termination */
    /* read MD5 hash */
    if (fread(dp->md5, 16, 1, db_fp) != 1) {
      ERR_("failed to load MD5 for %s", dp->filename);
      free(dp->filename);
      free(dp);
      goto out;
    }
    int len;
    dp->filename_hash = gethash(dp->filename, &len);

    /* add to in-core dedup DB */
    list_add_tail(&dp->list, &dedup_database.head[dp->filename_hash & DATABASE_HASH_MASK]);
    dedup_database.entries++;
  }
  
  fclose(db_fp);
  return;
out:
  fclose(db_fp);
  unlink(fn); /* it's broken */
  ERR_("failed to load dedup DB");
}

/** Save the current dedup DB.
 */
void dedup_save(void)
{
  dedup_t *dp;
  /* assuming that cwd is the backing filesystem's root */
  FILE* db_fp = fopen(DEDUP_DB_FILE, "w");
  if (!db_fp) {
    ERR_("failed to open dedup DB for writing");
    return;
  }

  /* write header */
  if (fwrite("DEDUP", DEDUP_MAGIC_SIZE, 1, db_fp) == EOF)
    goto out;
  uint16_t version = DEDUP_VERSION;
  if (fwrite(&version, 1, 2, db_fp) == EOF)
    goto out;
  
  /* write data */
  int i;
  for (i = 0; i < DATABASE_HASH_SIZE; i++) {
    list_for_each_entry(dp, &dedup_database.head[i], list) {
      /* length of filename */
      uint32_t len = strlen(dp->filename);
      if (fwrite(&len, 1, 4, db_fp) == EOF)
        goto out;
      /* filename */
      if (fputs(dp->filename, db_fp) == EOF)
        goto out;
      /* MD 5 hash */
      if (fwrite(dp->md5, 1, 16, db_fp) == EOF)
        goto out;
      /* no need to save filename hash, it can be easily regenerated
         when loading */
    }
  }

  fclose(db_fp);
  return;
out:
  fclose(db_fp);
  unlink(DEDUP_DB_FILE);
  ERR_("failed to write dedup DB");
}

/** Rename a file in the dedup database.
 * @param from Original file.
 * @param to Target file.
 */
void dedup_rename(file_t *from, file_t *to)
{
  /* propagate the dedup status to the new file */
  NEED_LOCK(&to->lock);
  DEBUG_("to->deduped %d, from->deduped %d", to->deduped, from->deduped);
  to->deduped = from->deduped;

  /* search for from file in the dedup DB and remove it from there */
  dedup_t *dp;
  int found = 0;
  DEBUG_("dedup renaming '%s'/%08x to '%s'/%08x", from->filename, from->filename_hash, to->filename, to->filename_hash);
  LOCK(&dedup_database.lock);
  list_for_each_entry(dp, &dedup_database.head[from->filename_hash & DATABASE_HASH_MASK], list) {
    if (dp->filename_hash == from->filename_hash && !strcmp(dp->filename, from->filename)) {
      DEBUG_("found file '%s' to rename", from->filename);
      list_del(&dp->list);
      found = 1;
      break;
    }
  }

  /* it's quite possible that an entry is not in the dedup DB yet, it
     may not have been released yet */
  if (found) {
    /* new filename */
    free(dp->filename);
    dp->filename = strdup(to->filename);
    dp->filename_hash = to->filename_hash;
    /* add to DB again */
    list_add_tail(&dp->list, &dedup_database.head[dp->filename_hash & DATABASE_HASH_MASK]);
  }
  UNLOCK(&dedup_database.lock);
}
