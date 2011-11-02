#include "structs.h"

void do_dedup(file_t *file);
int do_undedup(file_t *file);
void dedup_discard(file_t *file);
void dedup_rename(file_t *from, file_t *to);

void dedup_load(const char *dir);
void dedup_save();
