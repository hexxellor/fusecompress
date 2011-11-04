#include <stdio.h>
#include "structs.h"

void do_dedup(file_t *file);
int do_undedup(file_t *file);
void dedup_discard(file_t *file);
void dedup_rename(file_t *from, file_t *to);

void dedup_load(const char *dir);
void dedup_save();

int dedup_hash_file(const char *name, unsigned char *md5);
void dedup_add(unsigned char *md5, const char *filename);
int dedup_db_has(unsigned char *md5);
void dedup_init_db(void);
