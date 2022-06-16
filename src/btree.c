#include "btree.h"
#include <stdlib.h>
#include <string.h>

#define BTREE_NON_LEAF 0
#define BTREE_LEAF 1

typedef struct{
    btree_key_t key;
    intptr_t value;
    intptr_t child;
}btree_key;

typedef struct btree_node_s{
    uint32_t num:31;
    uint32_t leaf:1;
    btree_key keys[0];
}btree_node;

typedef struct btree_s{
    size_t block_size;
    size_t M;
    size_t count;
    btree_node root[0];
}btree;

#define M(block_size) (((block_size)-sizeof(btree_node))/sizeof(btree_key) - 1) // 子树为M, 但合并时候会出现M+1, 需要预留足够位置
#define ceil(M) (((M)-1)/2)

inline static btree_node* btree_node_create(size_t block_size, int leaf){
    btree_node *node = calloc(block_size, sizeof(char));
    node->leaf = leaf;
    return node;
}

inline static void btree_node_destroy(btree_node *node){
    free(node);
}

size_t btree_size(size_t block_size){
    return sizeof(btree) + block_size;
}

int btree_create(size_t block_size,btree **tree){
    if((block_size & (block_size-1)) != 0){
        // block size must be pow of 2!
        return -1;
    }
    if(block_size < sizeof(btree_node) + sizeof(btree_key)){
        return -1;
    }
    size_t M = M(block_size);
    if(M < 3){
        return -1;
    }

    if(M >= (1UL<<31)){
        return -1;
    }

    if(*tree == NULL){
        *tree = malloc(sizeof(btree) + block_size);
    }
    memset(*tree, 0, sizeof(btree) + block_size);
    (*tree)->block_size = block_size;
    (*tree)->M = M;
    (*tree)->root->leaf = BTREE_LEAF;
    return 0;
}

inline static int key_binary_search(btree_key *key, int num, btree_key_t target)
{
	int low = 0, high = num-1, mid;
	while (low <= high) {
		mid = low + (high - low) / 2;
        if(target == key[mid].key){
            return mid;
        } else if (target > key[mid].key) {
			low = mid + 1;
		} else {
			high = mid - 1;
		}
	}
	return -low-1;
}

#define keycpy(dest, src,n) memmove(dest,src,sizeof(*src) * (n+1))

inline static void btree_split_child(btree* tree, btree_node *node, int position){
    // 新建右结点, 分走x的一半，x的中间结点升到父节点
    btree_node *x = (btree_node *)node->keys[position].child;
    btree_node *y = btree_node_create(tree->block_size, x->leaf);

    size_t n = ceil(tree->M);
    keycpy(&y->keys[0], &x->keys[n+1], x->num - n - 1);

    y->num = x->num - n - 1;
    x->num = n;
    
    keycpy(&node->keys[position+1], &node->keys[position], node->num - position);// 父节点右移1个空位
    node->keys[position] = x->keys[n];
    node->keys[position].child = (intptr_t)x;
    node->keys[position+1].child = (intptr_t)y;
    node->num++;
}

inline static int btree_merge(btree* tree, btree_node *node, int position){
    // 父的一个节点下降，并与两个子节点合并
    btree_node *x = (btree_node *)node->keys[position].child;
    btree_node *y = (btree_node *)node->keys[position+1].child;

    x->keys[x->num].key = node->keys[position].key;
    x->keys[x->num].value = node->keys[position].value;
    keycpy(&x->keys[x->num+1], &y->keys[0],y->num);    
    x->num += ( 1 + y->num );
    btree_node_destroy(y);

    keycpy(&node->keys[position], &node->keys[position+1], node->num - position - 1);// 左移一位
    node->keys[position].child = (intptr_t)x;
    node->num--;

    if(node->num > 0){
        return 0;
    }

    // node->num == 0 must be root
    memcpy(tree->root, x, tree->block_size);
    btree_node_destroy(x);
    return 1;
}

int btree_insert(btree* tree, btree_key_t key, intptr_t value, int overwrite){
    btree_node *node;
    int i;

    // root结点已满, 在merge的时候关键词有机会出现==M的情况
    if(tree->root->num >= tree->M-1){
        node = btree_node_create(tree->block_size, tree->root->leaf);
        memcpy(node, tree->root, tree->block_size);
        memset(tree->root, 0, tree->block_size);
        tree->root->leaf = BTREE_NON_LEAF;
        tree->root->keys[0].child = (intptr_t)node;
        btree_split_child(tree, tree->root, 0);
    }

    node = tree->root;
    
    while(node->leaf == BTREE_NON_LEAF){
        i = key_binary_search(node->keys, node->num, key);
        if(i >= 0){
            if(overwrite){
                node->keys[i].value = value;
                return 1;
            }else{
                return 0;
            }
        }

        i = -(i+1);

        if(((btree_node *)(node->keys[i].child))->num >= tree->M-1){
            btree_split_child(tree, node, i);
            if(key == node->keys[i].key){// 上升关键词的正好相等
                if(overwrite){
                    node->keys[i].value = value;
                    return 1;
                }else{
                    return 0;
                }
            }else if(key > node->keys[i].key){
                i++;
            }
        }
        node = (btree_node *)(node->keys[i].child);
    }

    i = key_binary_search(node->keys, node->num, key);
    if(i >= 0){
        if(overwrite){
            node->keys[i].value = value;
            return 1;
        }else{
            return 0;
        }
    }

    i = -(i+1);

    keycpy(&node->keys[i+1], &node->keys[i], node->num-i);// 右移1个空位
    node->keys[i].key = key;
    node->keys[i].value = value;
    node->num++;
    tree->count++;
    return 1;
}


int btree_delete(btree* tree, btree_key_t key, intptr_t * value){    
    #define LESS 1
    #define MORE 2
    int i,i_match=-1,flag = 0;
    btree_node *node = tree->root, *node_match = NULL;

    while(node->leaf == BTREE_NON_LEAF){
        switch (flag)
        {
        case LESS:
            i = -0-1;
            break;
        case MORE:
            i = -node->num-1;
            break;
        default:
            i = key_binary_search(node->keys, node->num, key);
            break;
        }

        // match when in internal
        if(i >= 0){
            if(((btree_node *)(node->keys[i].child))->num > ceil(tree->M)){
                flag = MORE;
                node_match = node;
                i_match = i;
                node = (btree_node *)node->keys[i].child;
            }else if(((btree_node *)node->keys[i+1].child)->num > ceil(tree->M)){
                flag = LESS;
                node_match = node;
                i_match = i;
                node = (btree_node *)node->keys[i+1].child;
            }else{
                if(!btree_merge(tree, node, i)){
                    node = (btree_node *)node->keys[i].child;
                }
            }
            continue;
        }
        
        i = -(i+1);

        // need prepare , make sure child have enough key
        btree_node *sub_x = (btree_node *)node->keys[i].child;

        if(sub_x->num > ceil(tree->M)){
            node = sub_x;
            continue;
        }

        if(i+1<=node->num && ((btree_node *)(node->keys[i+1].child))->num > ceil(tree->M)){
            //borrow from right
            btree_node *sub_y = (btree_node *)(node->keys[i+1].child);
            sub_x->keys[sub_x->num].key = node->keys[i].key;
            sub_x->keys[sub_x->num].value = node->keys[i].value;
            sub_x->keys[sub_x->num+1].child = sub_y->keys[0].child;
            sub_x->num++;

            node->keys[i].key = sub_y->keys[0].key;
            node->keys[i].value = sub_y->keys[0].value;
            keycpy(&sub_y->keys[0], &sub_y->keys[1], sub_y->num-1);
            sub_y->num--;
            node = sub_x;
        }else if(i-1>=0 && ((btree_node *)(node->keys[i-1].child))->num > ceil(tree->M)){
            // borrow from left
            btree_node *sub_w = (btree_node *)(node->keys[i-1].child);
            keycpy(&sub_x->keys[1],&sub_x->keys[0],sub_x->num);
            sub_x->keys[0].key = node->keys[i-1].key;
            sub_x->keys[0].value = node->keys[i-1].value;
            sub_x->keys[0].child = sub_w->keys[sub_w->num].child;
            sub_x->num++;

            node->keys[i-1].key = sub_w->keys[sub_w->num-1].key;
            node->keys[i-1].value = sub_w->keys[sub_w->num-1].value;
            sub_w->num--;
            node = sub_x;
        }else{
            if(i+1 <= node->num){
                // merge with right
                if(!btree_merge(tree, node, i)){
                    node = (btree_node *)(node->keys[i].child);
                }
            }else{
                if(!btree_merge(tree, node, i-1)){
                    node = (btree_node *)(node->keys[i-1].child);
                }
            }
        }
    }

    if(flag == LESS){
        if(value)
            *value = node_match->keys[i_match].value;
        node_match->keys[i_match].key = node->keys[0].key;
        node_match->keys[i_match].value = node->keys[0].value;
        keycpy(&node->keys[0],&node->keys[1],node->num-1);
        node->num--;
        tree->count--;
        return 1;
    }else if(flag == MORE){
        if(value)
            *value = node_match->keys[i_match].value;
        node_match->keys[i_match].key = node->keys[node->num-1].key;
        node_match->keys[i_match].value = node->keys[node->num-1].value;
        node->num--;
        tree->count--;
        return 1;
    }else{
        i = key_binary_search(node->keys,node->num,key);
        if(i >= 0){
            if(value)
                *value = node->keys[i].value;
            keycpy(&node->keys[i],&node->keys[i+1],node->num - i - 1);
            node->num--;
            tree->count--;
            return 1;
        }
        return 0;
    }
}

int btree_search(btree* tree, btree_key_t key, intptr_t * value){
    btree_node *node = tree->root;
    int i;
    do{
        i = key_binary_search(node->keys, node->num, key);
        if(i >= 0){
            if(value)
                *value = node->keys[i].value;
            return 1;
        }
        i = -(i+1);
        node = (btree_node *)node->keys[i].child;
    }while(node);
    return 0;
}

static void btree_node_free(size_t M, btree_node *node){
    if(!node){
        return ;
    }
    int i;
    for(i=0; i<=node->num; i++){
        btree_node_free(M, (btree_node *)(node->keys[i].child));
    }
    free(node);
}

void btree_free(btree *tree,int notroot){
    int i;
    for(i=0; i<=tree->root->num; i++){
        btree_node_free(tree->M, (btree_node *)(tree->root->keys[i].child));
    }
    if(notroot){
        return;
    }
    free(tree);
}

size_t btree_count(btree* tree){
    return tree->count;
}