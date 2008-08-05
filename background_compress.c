/* background_compress for fusecompress.
 *
 * Copyright (C) 2005 Anders Aagaard <aagaande@gmail.com>
 * Copyright (C) 2005 Milan Svoboda <milan.svoboda@centrum.cz>
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
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "structs.h"
#include "globals.h"
#include "file.h"
#include "compress.h"
#include "log.h"
#include "direct_compress.h"
#include "background_compress.h"

/**
 * Add entry to the list of the files that will be compressed
 * later.
 *
 * @param file File that should be compressed later
 */
void background_compress(file_t *file)
{
	compress_t *entry;

	// This function must be called with read lock
	//
	NEED_LOCK(&file->lock);

	DEBUG_("add '%s' to compress queue", file->filename);

	// Compressor for this entry must not be set
	//
	assert(!file->compressor);

	// Increase accesses by 1, so entry is not flushed from the database.
	//
	file->accesses++;

	entry = (compress_t *) malloc(sizeof(compress_t));
	if (!entry)
	{
		ERR_("malloc failed!");
		return;
	}
	entry->file = file;

	// Add us to the database
	//
	LOCK(&comp_database.lock);
	list_add_tail(&entry->list, &comp_database.head);
	pthread_cond_signal(&comp_database.cond);
	comp_database.entries++;
	UNLOCK(&comp_database.lock);
}

void *thread_compress(void *arg)
{
	file_t     *file;
	compress_t *entry;

	// pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	while (TRUE)
	{
		// Lock compressor database and check for new entries
		//
		LOCK(&comp_database.lock);

		// Wait for new entry in database
		//
		while (list_empty(&comp_database.head))
		{
			pthread_cond_wait(&comp_database.cond, &comp_database.lock);
		}
		STAT_(STAT_BACKGROUND_COMPRESS);

		// Grab first comp_entry
		//
		entry = list_entry((&comp_database.head)->next, typeof(*entry), list);
		assert(entry);
		
		// Get file from entry
		//
		file = entry->file;
		assert(file);

		// We have file, we can delete and free entry
		//
		list_del(&entry->list);
		free(entry);
		comp_database.entries--;

		UNLOCK(&comp_database.lock);

		// This is safe, entry cannot be freed because entry->accesses++ in
		// background_compress
		//
		LOCK(&file->lock);

		// entry->accesses must be > 0 because it was added
		// by background_compress
		//
		assert(file->accesses > 0);

		// If entry->accesses = 1 we're the only one accessing it, also only
		// try to compress files that hasn't been deleted
		// and hasn't been compressed
		//
		if ((file->accesses == 1) && (!file->deleted) && (!file->compressor))
		{
			DEBUG_("compressing '%s'", file->filename);
			do_compress(file);
		}

		// Restore entry->accesses to original value
		// (@see background_compress())
		//
		file->accesses--;

		UNLOCK(&file->lock);
	}

}
