/* direct_compress for fusecompress
 *
 * Copyright (C) 2005 Anders Aagaard <aagaande@gmail.com>
 * Copyright (C) 2006 Milan Svoboda <milan.svoboda@centrum.cz>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <syslog.h>
#include <errno.h>
 
#include "structs.h"
#include "globals.h"
#include "log.h"
#include "list.h"
#include "file.h"
#include "direct_compress.h"
#include "compress.h"
#include "background_compress.h"
#include "utils.h"

/*  MAX_DATABASE_LEN = When the database has this many entries, we clean the database

    Note that these are soft targets and can grow over these sizes if you have a ton of open files.
    Making these big will only slow us down, as we have to scan through the entire database on new files.
*/
#define MAX_DATABASE_LEN 30

void direct_open_delete(file_t *file)
{
	NEED_LOCK(&file->lock);

	// It's out of the database, so we can unlock and destroy it
	//
	UNLOCK(&file->lock);
	pthread_mutex_destroy(&file->lock);

	free(file);
}

/**
 * Truncate database
 *
 * @param force if TRUE, files with accesses != 0 are removed from database
 *              if FALSE, only files with accesses == 0 are removed from database
 */
void _direct_open_purge(int force)
{
	file_t *file;
	file_t *safe = NULL;

	NEED_LOCK(&database.lock);

	DEBUG_("cleaning db, entries %d", database.entries);

	list_for_each_entry_safe(file, safe, &database.head, list)
	{
		LOCK(&file->lock);

		DEBUG_("('%s'), accesses: %d, deleted: %d, compressor: %p, size: %lli",
				file->filename, file->accesses, file->deleted,
				file->compressor, file->size);

		if (file->accesses == 0)
		{
			// Check if file should be compressed
			//
			// file must not be deleted, must not have assigned a compressor,
			// must be bigger than minimal size or have unknown size ( == 0) and
			// compressor can be assigned with this file.
			// Also, the backing FS must have at least enough space to store the
			// file uncompressed (worst case).
			struct statvfs stat;
			if ((!file->deleted) &&
			    (!file->compressor) &&
			    ((file->size == (off_t) -1) || (file->size > min_filesize_background)) &&
			    statvfs(file->filename, &stat) == 0 &&
			    (stat.f_bsize * stat.f_bavail >= file->size || (geteuid() == 0 && stat.f_bsize * stat.f_bfree >= file->size)) &&
			    choose_compressor(file))
			{
				DEBUG_("compress file on background");
				background_compress(file);
			}
			else
			{
				DEBUG_("trim from database");
				list_del(&file->list);
				database.entries--;
	
				// It's out of the database, so we can destroy it
				//
				direct_open_delete(file);
				continue;
			}
		}
		else
		{
			if (force)
			{
				DEBUG_("Forcing trim from database");
				DEBUG_("This could happend only in debug mode, "
				       "when fusecompress was terminated with "
				       "some files still opened. Don't report "
				       "this as bug please.");
				list_del(&file->list);
				database.entries--;
	
				// It's out of the database, so we can destroy it
				//
				/* The background compression thread seems to still use this lock
				   sometimes, leading to massive data corruption. This is triggered
				   by using LZMA. The bright side is that this code here is only run
				   on unmount, so losing a few bytes by not freeing the file
				   descriptor won't hurt us. */
				//direct_open_delete(file);
				UNLOCK(&file->lock);
				continue;
			}
		}			
		UNLOCK(&file->lock);
	}
}

inline void direct_open_purge(void)
{
	_direct_open_purge(FALSE);
}

inline void direct_open_purge_force(void)
{
	_direct_open_purge(TRUE);
}

file_t* direct_new_file(unsigned int filename_hash, const char *filename, int len)
{
	file_t *file;

	DEBUG_("('%s'), hash: %d", filename, filename_hash);

	// Allocate memory for file_t and filename together.
	// This is a optimization - it's much better to allocate one bigger buffer
	// than two small buffers...
	//
	file = malloc(sizeof(file_t) + len);
	if (!file)
	{
		CRIT_("No memory!");
		exit(EXIT_FAILURE);
		// direct_new_file() is used by direct_open(), and there this
		// absolutely no error handling anywhere direct_open() is used,
		// so we have to leave this one.
	}

	file->accesses = 0;
	file->size = (off_t) -1;	// -1 means unknown file size
	file->deleted = FALSE;
	file->compressor = NULL;
	file->type = 0;
	file->dontcompress = FALSE;
	file->status = 0;

	file->filename_hash = filename_hash;
	file->filename = (char *) file + sizeof(file_t);
	memcpy(file->filename, filename, len);

	pthread_mutex_init(&file->lock, &locktype);
	pthread_cond_init(&file->cond, NULL);
	INIT_LIST_HEAD(&file->head);

	return file;
}

// Returns locked database entry
file_t *direct_open(const char *filename, int stabile)
{
	int          len;
	int          hysteresis;
	unsigned int hash;
	file_t      *file;
	file_t      *safe = NULL;

	assert(filename);

	DEBUG_("('%s')", filename);

	hash = gethash(filename, &len);

	LOCK(&database.lock);

	list_for_each_entry_safe(file, safe, &database.head, list)
	{
		LOCK(&file->lock);

//		DEBUG_("%d: ('%s'), size: %lli, deleted: %d",
//			i++, file->filename, file->size, file->deleted);

		if (unlikely(file->filename_hash == hash) &&
				  likely((memcmp(file->filename, filename, len) == 0)))
		{
			// Return file with lock. This file is requested and that
			// means that file cannot be in deleted state.
			//
			file->deleted = FALSE;
			UNLOCK(&database.lock);

			if (stabile == TRUE)
			{
				// TODO: Caller require stable enviroment. The question is:
				// does he need decompressed file or would he work with compressed fine?
				// And has compression been running for a long time? Is it before end or does it
				// just started now? What is better?
				//
				// Caller require stable enviroment - cancel compressing
				// if it is running now or block until decompression ends...
				//
				while (file->status & (COMPRESSING | DECOMPRESSING))
				{
					file->status |= CANCEL;
					pthread_cond_wait(&file->cond, &file->lock);
				}
			}
			return file;
		}
		
		UNLOCK(&file->lock);
	}

	LOCK(&comp_database.lock);
	hysteresis = comp_database.entries;
	UNLOCK(&comp_database.lock);

	if (database.entries++ > MAX_DATABASE_LEN + hysteresis)
	{
		direct_open_purge();
	}

	file = direct_new_file(hash, filename, len);

	// Return file with read-lock
	//
	LOCK(&file->lock);
	list_add_tail(&file->list, &database.head);

	UNLOCK(&database.lock);
	return file;
}

int direct_close(file_t *file, descriptor_t *descriptor)
{
	int ret = 0;

	NEED_LOCK(&file->lock);

	DEBUG_("('%s')", file->filename);

	if (descriptor->handle)
	{
		assert(file->compressor);

		ret = file->compressor->close(descriptor->handle);

		descriptor->offset = 0;
		descriptor->handle = NULL;
	}

	if (file->accesses == 1)
	{
		// There is only one user of this file and he is going to
		// release it => set some properties to default state.
		//
		file->type = 0;
		file->dontcompress = FALSE;
	}

	return ret;
}

int direct_decompress(file_t *file, descriptor_t *descriptor, void *buffer, size_t size, off_t offset)
{
	int len;

	assert(file);
	assert(file->compressor);
	assert(descriptor);
	assert(descriptor->fd != -1);

	NEED_LOCK(&file->lock);

	DEBUG_("('%s'), offset: %lli, descriptor->offset: %lli",
				file->filename, offset, descriptor->offset);
	STAT_(STAT_DIRECT_READ);

	while (file->status & DECOMPRESSING)
	{
		file->status |= CANCEL;
		pthread_cond_wait(&file->cond, &file->lock);
	}

	if (file->type == 0)
	{
		file->type = READ;
	}

	// Decompress file if:
	//  - user wants to read from somewhere else
	//  - user wants to read when he was writing to file before
	//
	// If offset is wrong, we need to close and open file in raw mode.
	//
	if ((offset != descriptor->offset) || (!(file->type & READ)))
	{
		DEBUG_("\tfallback, offset: %lli, descriptor->offset: %lli, !(file->type & READ): %d",
			offset, descriptor->offset, (!(file->type & READ)));
		STAT_(STAT_FALLBACK);

		DEBUG_("calling do_decompress, descriptor->fd %d",descriptor->fd);
		if (!do_decompress(file))
		{
			DEBUG_("do_decompress failed, descriptor->fd %d",descriptor->fd);
			return FAIL;
		}

		file->size = -1;

		return pread(descriptor->fd, buffer, size, offset);
	}

	// Open file if neccesary
	//
	if (!descriptor->handle)
	{
		assert(descriptor->offset == 0);

		descriptor->handle = file->compressor->open(dup(descriptor->fd), "rb");
		if (!descriptor->handle)
		{
			ERR_("failed to open compressor on fd %d file ('%s')",
				descriptor->fd, descriptor->file->filename);

			return FAIL;
		}
	}

	// Do actual decompressing
	//
	len = file->compressor->read(descriptor->handle, buffer, size);
	if (len < 0)
	{
		ERR_("failed to read from compressor");
		return -1;
	}
	descriptor->offset += len;

	DEBUG_("read requested len: %d, got: %d", size, len);
	return len;
}

/*
  We need LOCK_ENTRY here.
  This function returns TRUE on data read ok (length in returned_len), FALSE on use pwrite instead and FAIL on error.
*/
int direct_compress(file_t *file, descriptor_t *descriptor, const void *buffer, size_t size, off_t offset)
{
	int len;
	int ret;

	assert(file);
	assert(file->compressor);
	assert(descriptor);
	assert(descriptor->fd != -1);

	NEED_LOCK(&file->lock);

	DEBUG_("('%s'), offset: %lli, descriptor->offset: %lli",
				file->filename, offset, descriptor->offset);
	STAT_(STAT_DIRECT_WRITE);

	while (file->status & DECOMPRESSING)
	{
		file->status |= CANCEL;
		pthread_cond_wait(&file->cond, &file->lock);
	}

	if (file->type == 0)
	{
		file->type = WRITE;
	}

	// Decompress file if:
	//  - user wants to write somewhere else than to the end of file
	//  - user wants to write to the end of the file after he read from file
	//  - there is more than 1 user of the file, we cannot support more
	//    simulteanous writes at a time
	//
	if ((descriptor->offset != file->size) || (descriptor->offset != offset) || (!(file->type & WRITE)) || (file->accesses > 1))
	{
		DEBUG_("\tfallback, offset: %lli, descriptor->offset: %lli, !(file->type & WRITE): %d, file->accesses: %d",
			offset, descriptor->offset, (!(file->type & WRITE)), file->accesses);
		STAT_(STAT_FALLBACK);

		DEBUG_("calling do_decompress, descriptor->fd %d",descriptor->fd);
		if (!do_decompress(file))
		{
			DEBUG_("do_decompress failed, descriptor->fd %d",descriptor->fd);
			return FAIL;
		}

		file->size = -1;

		return pwrite(descriptor->fd, buffer, size, offset);
	}

	// Open file if neccesary
	//
	if (!descriptor->handle)
	{
		assert(file->size == 0);
		assert(descriptor->offset == 0);

		DEBUG_("writing header...");

		ret = file_write_header(descriptor->fd,
					file->compressor,
					file->size);
		if (ret == FAIL)
		{
			CRIT_("\t...failed!");
			//exit(EXIT_FAILURE);
			return FAIL;
		}
		
		descriptor->handle = file->compressor->open(dup(descriptor->fd), "wb");
		if (!descriptor->handle)
		{
			ERR_("failed to open compressor on fd %d file ('%s')",
				descriptor->fd, descriptor->file->filename);

			return FAIL;
		}
	}
	assert(descriptor->handle);

	// Do actual compressing
	//
	len = file->compressor->write(descriptor->handle, (void *)buffer, size);

	DEBUG_("write requested: %d, really written: %d", size, len);

	if (len == FAIL)
	{
		return FAIL;
	}
	descriptor->offset += len;

	// When writing we're always at EOF
	//
	file->size = descriptor->offset;
	
	// Write file length to file.  We can skip this here and do it on close instead?
	// (could mean sync problems because release isn't sync'ed with close)
	//
	DEBUG_("setting filesize to %lld", file->size);

	if (lseek(descriptor->fd, 0, SEEK_SET) == FAIL)
	{
		CRIT_("lseek failed!");
		//exit(EXIT_FAILURE);
		return FAIL;
	}
	// Todo, we can speed this up by only writing the size, and not flushing
	// it so often.
	//
	ret = file_write_header(descriptor->fd,
				file->compressor,
				file->size);
	if (ret == FAIL)
	{
		CRIT_("file_write_header failed!");
		//exit(EXIT_FAILURE);
		return FAIL;
	}
	if (lseek(descriptor->fd, 0, SEEK_END) == FAIL)
	{
		CRIT_("lseek failed");
		//exit(EXIT_FAILURE);
		return FAIL;
	}

	return len;
}

// Mark when file is deleted
//
inline void direct_delete(file_t *file)
{
	NEED_LOCK(&file->lock);

	file->deleted = TRUE;
	file->size = (off_t) -1;
}

// Move information from file_from to file_to when file_from is
// renamed to file_to.
//
// file_to is always a new file_t. Fuse library creates a temporary
// file (.fusehidden...) when file_to is already opened
//
file_t* direct_rename(file_t *file_from, file_t *file_to)
{
	descriptor_t *safe;
	descriptor_t *descriptor;

	NEED_LOCK(&file_from->lock);
	NEED_LOCK(&file_to->lock);

	DEBUG_("('%s' -> '%s')", file_from->filename, file_to->filename);
	DEBUG_("\tfile_from->compressor: %p, file_from->size: %lli, file_from->accesses: %d",
		file_from->compressor, file_from->size, file_from->accesses);

	file_to->size = file_from->size;
	file_to->compressor = file_from->compressor;

	file_to->dontcompress = file_from->dontcompress;
	file_to->type = file_from->type;
	file_to->status = file_from->status;

	// Move descriptors from file_from to file_to.
	//
	list_for_each_entry_safe(descriptor, safe, &file_from->head, list)
	{

		list_del(&descriptor->list);
		file_from->accesses--;

		list_add(&descriptor->list, &file_to->head);
		file_to->accesses++;

		descriptor->file = file_to;
	}
	assert(file_from->accesses == 0);

	// The file refered by file_from is now deleted
	//
	direct_delete(file_from);

	return file_to;
}

