#ifndef _BTREE_H_
#define _BTREE_H_

#include <stddef.h>
#include <stdint.h>

typedef uint64_t btree_key_t;
typedef struct btree_s btree;

size_t btree_size(size_t block_size);
int btree_create(size_t block_size,btree **T);
void btree_free(btree *T,int notroot);
int btree_insert(btree* T,btree_key_t key,intptr_t value, int overwrite);
int btree_delete(btree* T,btree_key_t key, intptr_t * value);
int btree_search(btree* T,btree_key_t key, intptr_t * value);
size_t btree_count(btree* T);

#endif