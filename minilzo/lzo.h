//
// C Interface: lzo
//
// Description:
//
//
// Author: Milan Svoboda <milan.svoboda@centrum.cz>, (C) 2006
//
// Copyright: See COPYING file that comes with this distribution
//
//
#ifndef __LZO_H
#define __LZO_H

#include <lzo/lzo1x.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	lzo_uint	usize;
	lzo_uint	psize;
} lzoHead;

typedef struct {
	char		*buf;
	lzo_uint	 usize;	// For write: size of buffer
	lzo_uint	 psize;	// For write: number of stored bytes
} lzoBlock;

enum lzoMode {
	LZO_READ,
	LZO_WRITE,
};

typedef struct {
	int		fd;
	enum lzoMode	mode;
	lzoBlock	block;
	lzo_uint	blockoff;
} lzoFile;

lzoFile* lzodopen(int fd, const char *mode);
int lzoclose(lzoFile *file);
int lzowrite(lzoFile *file, char *buf, unsigned buf_len);
int lzoread(lzoFile *file, char *buf, unsigned buf_len);

#ifdef __cplusplus
}
#endif

#endif
