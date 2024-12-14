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
	
	int i;
	sl_node_t *node;
	
	node = set->head;
	for (i = node->toplevel-1; i >= 0; i--) {
		while (node->next_arr[i].next_val < val) {
			node = node->next_arr[i].next;
		}
	}
	result = (node->next_arr[0].next_val == val);
		
#elif defined STM
	
	int i;
	sl_node_t *node;
	val_t v = VAL_MIN;

	if (transactional > 1) {
	
	  TX_START(EL);
	  node = set->head;
	  for (i = node->toplevel-1; i >= 0; i--) {
        while ((v = TX_LOAD(&node->next_arr[i].next_val)) < val) {
          node = (sl_node_t *)TX_LOAD(&node->next_arr[i].next);
		}
	  }
	  node = (sl_node_t *)TX_LOAD(&node->next_arr[0].next);
	  result = (v == val);
	  TX_END;

	} else {

	  TX_START(NL);
	  node = set->head;
	  for (i = node->toplevel-1; i >= 0; i--) {
	    while ((v = TX_LOAD(&node->next_arr[i].next_val)) < val) {
	      node = (sl_node_t *)TX_LOAD(&node->next_arr[i].next);
	    }
	  }
	  node = (sl_node_t *)TX_LOAD(&node->next_arr[0].next);
	  result = (v == val);
	  TX_END;

	}
	
#endif
	
	return result;
}

inline int sl_seq_add(sl_intset_t *set, val_t val) {
	int i, l, result;
	sl_node_t *node;
	sl_node_t *preds[MAXLEVEL], *succs[MAXLEVEL];
	
	node = set->head;
	for (i = node->toplevel-1; i >= 0; i--) {
		while (node->next_arr[i].next_val < val) {
			node = node->next_arr[i].next;
		}
		preds[i] = node;
		succs[i] = node->next_arr[i].next;
	}
	if ((result = (node->next_arr[0].next_val != val)) == 1) {
		l = get_rand_level();
		node = sl_new_simple_node(val, l, 0);
		for (i = 0; i < l; i++) {
			node->next_arr[i].next = succs[i];
            node->next_arr[i].next_val = succs[i] != NULL ? succs[i]->val : VAL_MAX;
			preds[i]->next_arr[i].next = node;
            preds[i]->next_arr[i].next_val = node->val;
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
	
	int i, l;
	sl_node_t *node;
	sl_node_t *preds[MAXLEVEL];
	val_t v;  
	
	if (transactional > 2) {

	  TX_START(EL);
	  v = VAL_MIN;
	  node = set->head;
	  for (i = node->toplevel-1; i >= 0; i--) {
	    while ((v = TX_LOAD(&node->next_arr[i].next_val)) < val) {
	      node = (sl_node_t *)TX_LOAD(&node->next_arr[i].next);
	    }
	    preds[i] = node;
	  }
	  if ((result = (v != val)) == 1) {
	    l = get_rand_level();
	    node = sl_new_simple_node(val, l, transactional);
	    for (i = 0; i < l; i++) {
          node->next_arr[i].next = TX_LOAD(&preds[i]->next_arr[i].next);
          node->next_arr[i].next_val = TX_LOAD(&preds[i]->next_arr[i].next_val);
	      TX_STORE(&preds[i]->next_arr[i].next, node);
          TX_STORE(&preds[i]->next_arr[i].next_val, node->val);
	    }
	  }
	  TX_END;

	} else {

	  TX_START(NL);
	  v = VAL_MIN;
	  node = set->head;
	  for (i = node->toplevel-1; i >= 0; i--) {
        while ((v = TX_LOAD(&node->next_arr[i].next_val)) < val) {
	      node = (sl_node_t *)TX_LOAD(&node->next_arr[i].next);
	    }
	    preds[i] = node;
	  }
	  if ((result = (v != val)) == 1) {
	    l = get_rand_level();
	    node = sl_new_simple_node(val, l, transactional);
	    for (i = 0; i < l; i++) {
          node->next_arr[i].next = (sl_node_t *)TX_LOAD(&preds[i]->next_arr[i].next);
          node->next_arr[i].next_val = TX_LOAD(&preds[i]->next_arr[i].next_val);
	      TX_STORE(&preds[i]->next_arr[i].next, node);
          TX_STORE(&preds[i]->next_arr[i].next_val, node->val);
	    }
	  }
	  TX_END;
	
	}
	
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
		while (node->next_arr[i].next_val < val) {
			node = node->next_arr[i].next;
		}
		preds[i] = node;
		succs[i] = node->next_arr[i].next;
	}
	if ((result = (node->next_arr[0].next_val == val)) == 1) {
        next = node->next_arr[0].next;
		for (i = 0; i < set->head->toplevel; i++)
            if (succs[i]->val == val)
				preds[i]->next_arr[i] = succs[i]->next_arr[i];
		sl_delete_node(next);
	}

#elif defined STM

	int i;
	sl_node_t *node, *next = NULL;
	sl_node_t *preds[MAXLEVEL], *succs[MAXLEVEL];
	val_t v;

	if (transactional > 3) {

	  TX_START(EL);
	  v = VAL_MIN;
	  node = set->head;
	  for (i = node->toplevel-1; i >= 0; i--) {
	    while ((v = TX_LOAD(&node->next_arr[i].next_val)) < val) {
	      node = (sl_node_t *)TX_LOAD(&node->next_arr[i].next);
	    }
	    preds[i] = node;
	    succs[i] = (sl_node_t *)TX_LOAD(&node->next_arr[i].next);
	  }
	  if ((result = (TX_LOAD(&node->next_arr[0].next_val) == val))) {
        next = (sl_node_t *)TX_LOAD(&node->next_arr[0].next);
	    for (i = 0; i < set->head->toplevel; i++) {
	      if (succs[i]->val == val) {
            TX_STORE(&preds[i]->next_arr[i].next, (sl_node_t *)TX_LOAD(&succs[i]->next_arr[i].next));
            TX_STORE(&preds[i]->next_arr[i].next_val, TX_LOAD(&succs[i]->next_arr[i].next_val));
	      }
	    }
	    FREE(next, sizeof(sl_node_t) + next->toplevel * sizeof(sl_next_entry_t));
	  }
	  TX_END;

	} else {

	  TX_START(NL);
	  v = VAL_MIN;
	  node = set->head;
	  for (i = node->toplevel-1; i >= 0; i--) {
	     while ((v = TX_LOAD(&node->next_arr[i].next_val)) < val) {
	      node = (sl_node_t *)TX_LOAD(&node->next_arr[i].next);
	    }
	    preds[i] = node;
	    succs[i] =(sl_node_t *)TX_LOAD(&node->next_arr[i].next);
	  }
	  if ((result = (TX_LOAD(&node->next_arr[0].next_val) == val))) {
        next = (sl_node_t *)TX_LOAD(&node->next_arr[0].next);
	    for (i = 0; i < set->head->toplevel; i++) {
	      if (succs[i]->val == val) {
            TX_STORE(&preds[i]->next_arr[i].next, (sl_node_t *)TX_LOAD(&succs[i]->next_arr[i].next));
            TX_STORE(&preds[i]->next_arr[i].next_val, TX_LOAD(&succs[i]->next_arr[i].next_val));
	      }
	    }
        FREE(next, sizeof(sl_node_t) + next->toplevel * sizeof(sl_next_entry_t));
	  }
	  TX_END;

	}
	
#endif
	
	return result;
}


