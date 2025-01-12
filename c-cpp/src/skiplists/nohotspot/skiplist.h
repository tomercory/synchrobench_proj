/*
 * Interface for the skip list data stucture.
 *
 * Author: Ian Dick, 2013
 *
 */
#ifndef SKIPLIST_H_
#define SKIPLIST_H_

#include <atomic_ops.h>

#include "common.h"
#include "ptst.h"
#include "garbagecoll.h"

#define MAX_LEVELS 128

#define NUM_LEVELS 2
#define NODE_LEVEL 0
#define INODE_LEVEL 1

#define MAX_KEY 18446744073709551615

typedef unsigned long sl_key_t;
typedef void* val_t;

/* bottom-level nodes */
typedef VOLATILE struct sl_node node_t;
struct sl_node {
        sl_key_t key;
        val_t val;
        struct sl_node *prev;
        struct sl_node *next;
        unsigned int level;
        unsigned int marker;
};

/* key-pointer duo, used for foresight in the modified version */
typedef VOLATILE struct sl_kp kp_t;
struct sl_kp {
    VOLATILE struct sl_inode *right_p;
    sl_key_t right_k;
};

/* index-level nodes */
typedef VOLATILE struct sl_inode inode_t;
struct sl_inode {
        struct sl_kp right; // Must ensure sl_kp is 16-byte aligned and does not cross a cache line boundary!
        struct sl_inode *down;
        struct sl_node  *node;
};



/* the skip list set */
typedef VOLATILE struct sl_set set_t;
struct sl_set {
        inode_t *top;
        node_t  *head;
        int raises;
};

node_t* node_new(sl_key_t key, val_t val, node_t *prev, node_t *next,
                 unsigned int level, ptst_t *ptst);

inode_t* inode_new(kp_t right, inode_t *down, node_t *node, ptst_t *ptst);

void node_delete(node_t *node, ptst_t *ptst);
void inode_delete(inode_t *inode, ptst_t *ptst);

set_t* set_new(int bg_start);
void set_delete(set_t *set);
void set_print(set_t *set, int flag);
int set_size(set_t *set, int flag);

void set_subsystem_init(void);

#endif /* SKIPLIST_H_ */
