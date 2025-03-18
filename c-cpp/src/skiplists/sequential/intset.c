/*
 * File:
 *   intset.c
 * Author(s):
 *   Vincent Gramoli <vincent.gramoli@epfl.ch>
 * Description:
 *   Skip list integer set operations 
 *
 * Copyright (c) 2009-2010.
 *
 * intset.c is part of Synchrobench
 * 
 * Synchrobench is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "intset.h"

#define MAXLEVEL    32

int sl_contains(sl_intset_t *set, val_t val, int transactional)
{
	int result = 0;
	
#ifdef SEQUENTIAL /* Unprotected */
	
	int i, goDown;
	sl_node_t *node, *next;
	
	node = set->head;
    i = node->toplevel-1;
    while(i >= 0){
    	next = node->next_arr[i].next;
        goDown = node->next_arr[i].next_val >= val;
        node = (sl_node_t *)(((unsigned long)node)*(goDown) + ((unsigned long)next)*(!goDown));
        i = i - goDown;
    }
//	for (i = node->toplevel-1; i >= 0; i--) {
//		next = node->next_arr[i].next;
//		while (node->next_arr[i].next_val < val) {
//			node = next;
//			next = node->next_arr[i].next;
//		}
//	}
    result = (node->next_arr[0].next_val == val);
		
#elif defined STM

    // not supported

#endif
	
	return result;
}

inline int sl_seq_add(sl_intset_t *set, val_t val) {
	int i, l, result;
	sl_node_t *node, *next;
	sl_node_t *preds[MAXLEVEL], *succs[MAXLEVEL];
	
	node = set->head;
	for (i = node->toplevel-1; i >= 0; i--) {
		next = node->next_arr[i].next;
		while (node->next_arr[i].next_val < val) {
			node = next;
			next = node->next_arr[i].next;
		}
		preds[i] = node;
		succs[i] = node->next_arr[i].next;
	}
	if ((result = (node->next_arr[0].next_val != val)) == 1) {
		l = get_rand_level();
		node = sl_new_simple_node(val, l, 0);
		for (i = 0; i < l; i++) {
            node->next_arr[i] = (sl_next_entry_t){ .next = succs[i], .next_val = succs[i]->val };
            preds[i]->next_arr[i] = (sl_next_entry_t){ .next = node, .next_val = node->val };
		}
	}
	return result;
}

int sl_add(sl_intset_t *set, val_t val, int transactional)
{
  int result = 0;
	
  if (!transactional) {
		
    result = sl_seq_add(set, val);
	
  } else {

#ifdef SEQUENTIAL
		
	result = sl_seq_add(set, val);
		
#elif defined STM

      // not supported
	
#endif
		
  }
	
  return result;
}

int sl_remove(sl_intset_t *set, val_t val, int transactional)
{
	int result = 0;
	
#ifdef SEQUENTIAL
	
	int i;
	sl_node_t *node, *next = NULL;
	sl_node_t *preds[MAXLEVEL], *succs[MAXLEVEL];
	
	node = set->head;
	for (i = node->toplevel-1; i >= 0; i--) {
		next = node->next_arr[i].next;
		while (node->next_arr[i].next_val < val) {
			node = next;
			next = node->next_arr[i].next;
		}
		preds[i] = node;
		succs[i] = node->next_arr[i].next;
	}
	if ((result = (next->val == val)) == 1) {
		for (i = 0; i < set->head->toplevel; i++) 
			if (succs[i]->val == val)
				preds[i]->next_arr[i] = succs[i]->next_arr[i];
		sl_delete_node(next); 
	}

#elif defined STM
	
	// not supported
	
#endif
	
	return result;
}


