/*
 * File:
 *   test.c
 * Author(s):
 *   Vincent Gramoli <vincent.gramoli@epfl.ch>
 * Description:
 *   Concurrent accesses to skip list integer set
 *
 * Copyright (c) 2009-2010.
 *
 * test.c is part of Synchrobench
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

#include <assert.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>
#include <atomic_ops.h>
#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>
#include <string.h>
#include "common.h"
#include "tm.h"
#include "ptst.h"
#include "garbagecoll.h"

#define DEFAULT_DURATION                10000
#define DEFAULT_INITIAL                 256
#define DEFAULT_NB_THREADS              1
#define DEFAULT_RANGE                   0x7FFFFFFF
#define DEFAULT_SEED                    0
#define DEFAULT_UPDATE                  20
#define DEFAULT_ELASTICITY              4
#define DEFAULT_ALTERNATE               0
#define DEFAULT_EFFECTIVE               1
#define DEFAULT_MONITOR                 0
#define DEFAULT_TEST                    0
#define DEFAULT_PARALLELISM             1
#define DEFAULT_UNBALANCED              0

#define XSTR(s)                         STR(s)
#define STR(s)                          #s

#define ATOMIC_CAS_MB(a, e, v)          (AO_compare_and_swap_full((VOLATILE AO_t *)(a), (AO_t)(e), (AO_t)(v)))
#define ATOMIC_FETCH_AND_INC_FULL(a)    (AO_fetch_and_add1_full((VOLATILE AO_t *)(a)))

#define TRANSACTIONAL                   d->unit_tx

#define VAL_MIN                         INT_MIN
#define VAL_MAX                         INT_MAX

#define  RAND_TEST_RANGE                8
#define  LOG2NUMTHREADS                 8
#define NUM_EVENTS 9

const char *events[NUM_EVENTS] = {
        "L1-dcache-loads",
        "L1-dcache-stores",
        "L1-dcache-load-misses",
        "LLC-loads",
        "LLC-stores",
        "LLC-load-misses",
        "LLC-store-misses",
        "cache-references",
        "cache-misses"
};


enum event_num {
    L1_CACHE_LOADS,
    L1_CACHE_STORES,
    L1_CACHE_MISSES,
    L3_CACHE_LOADS,
    L3_CACHE_STORES,
    L3_CACHE_LOAD_MISSES,
    L3_CACHE_STORE_MISSES,
    TOTAL_CACHE_REFS,
    TOTAL_CACHE_MISSES
};

int floor_log_2(unsigned int n);

inline long rand_range(long r); /* declared in test.c */

static inline int max(int a, int b) {
  return (a > b) ? a : b;
}

#include "intset.h"
#include "background.h"
#include <unistd.h>
#include <stdbool.h>

VOLATILE AO_t stop;
unsigned int global_seed;
#ifdef TLS
__thread unsigned int *rng_seed;
#else /* ! TLS */
pthread_key_t rng_seed_key;
#endif /* ! TLS */
unsigned int levelmax;

typedef struct barrier {
	pthread_cond_t complete;
	pthread_mutex_t mutex;
	int count;
	int crossing;
} barrier_t;

void barrier_init(barrier_t *b, int n)
{
	pthread_cond_init(&b->complete, NULL);
	pthread_mutex_init(&b->mutex, NULL);
	b->count = n;
	b->crossing = 0;
}

void barrier_cross(barrier_t *b)
{
	pthread_mutex_lock(&b->mutex);
	/* One more thread through */
	b->crossing++;
	/* If not all here, wait */
	if (b->crossing < b->count) {
		pthread_cond_wait(&b->complete, &b->mutex);
	} else {
		pthread_cond_broadcast(&b->complete);
		/* Reset for next time */
		b->crossing = 0;
	}
	pthread_mutex_unlock(&b->mutex);
}

int floor_log_2(unsigned int n) {
  int pos = 0;
  if (n >= 1<<16) { n >>= 16; pos += 16; }
  if (n >= 1<< 8) { n >>=  8; pos +=  8; }
  if (n >= 1<< 4) { n >>=  4; pos +=  4; }
  if (n >= 1<< 2) { n >>=  2; pos +=  2; }
  if (n >= 1<< 1) {           pos +=  1; }
  return ((n == 0) ? (-1) : pos);
}



/* 
 * Returns a pseudo-random value in [1;range).
 * Depending on the symbolic constant RAND_MAX>=32767 defined in stdlib.h,
 * the granularity of rand() could be lower-bounded by the 32767^th which might 
 * be too high for given values of range and initial.
 *
 * Note: this is not thread-safe and will introduce futex locks
 */
inline long rand_range(long r) {
	int m = RAND_MAX;
	int d, v = 0;
	
	do {
		d = (m > r ? r : m);		
		v += 1 + (int)(d * ((double)rand()/((double)(m)+1.0)));
		r -= m;
	} while (r > 0);
	return v;
}
long rand_range(long r);

/* Thread-safe, re-entrant version of rand_range(r) */
inline long rand_range_re(unsigned int *seed, long r) {
	int m = RAND_MAX;
	int d, v = 0;
	
	do {
		d = (m > r ? r : m);		
		v += 1 + (int)(d * ((double)rand_r(seed)/((double)(m)+1.0)));
		r -= m;
	} while (r > 0);
	return v;
}
long rand_range_re(unsigned int *seed, long r);

typedef struct thread_data {
	unsigned int first;
	long range;
	int update;
	int unit_tx;
	int alternate;
	int effective;
	int cache_monitoring;
    int validation_txs;
	unsigned long nb_add;
	unsigned long nb_added;
	unsigned long nb_remove;
	unsigned long nb_removed;
	unsigned long nb_contains;
	unsigned long nb_found;
	unsigned long nb_aborts;
	unsigned long nb_aborts_locked_read;
	unsigned long nb_aborts_locked_write;
	unsigned long nb_aborts_validate_read;
	unsigned long nb_aborts_validate_write;
	unsigned long nb_aborts_validate_commit;
	unsigned long nb_aborts_invalid_memory;
	unsigned long nb_aborts_double_write;
	unsigned long max_retries;
	unsigned int seed;
	struct sl_set *set;
	barrier_t *barrier;
	unsigned long failures_because_contention;
	unsigned long L1_cache_accesses;
    unsigned long L1_cache_misses;
    unsigned long L3_cache_accesses;
    unsigned long L3_cache_misses;
    unsigned long total_cache_accesses;
    unsigned long total_cache_misses;
	CACHE_PAD(0); // avoid false sharing with other threads
} thread_data_t;

typedef struct population_data {
  struct sl_set *set;
  unsigned long to_populate;
  long range; 
  sl_key_t* lastp;
} population_data_t;


void print_skiplist(struct sl_set *set) {
	struct sl_node *curr;
	int i, j;
	int arr[levelmax];
	
	for (i=0; i< sizeof arr/sizeof arr[0]; i++) arr[i] = 0;
	
	curr = set->head;
	do {
		printf("%lu", curr->key);
		for (i=0; i<curr->level-1; i++) {
			printf("-*");
		}
		arr[curr->level-2]++;
		printf("\n");
		curr = curr->next;
	} while (curr); 
	for (j=0; j<MAX_LEVELS-2; j++)
		printf("%d nodes of level %d\n", arr[j], j);
}



void* sanity_check(void *data) {
    thread_data_t *d = (thread_data_t *)data;
    unsigned int lsb = d->first;
    unsigned int key;
    sleep(1);

    /* Wait on barrier */
    barrier_cross(d->barrier);

    for (int i=0; i<d->validation_txs; ++i){
        key = (rand_range_re(&d->seed, d->range)<<LOG2NUMTHREADS) + lsb;
        if (key == 0) continue;
        if (!sl_contains_old(d->set, key, TRANSACTIONAL)){
            if(sl_remove_old(d->set, key, TRANSACTIONAL)) printf("BAD: managed to remove non-existent key %u\n", key);
            if(!sl_add_old(d->set, key, TRANSACTIONAL)) printf("BAD: failed to insert non-existent key %u\n", key);
            if(!sl_contains_old(d->set, key, TRANSACTIONAL)) printf("BAD: failed to find key %u after insertion \n", key);
            if(sl_add_old(d->set, key, TRANSACTIONAL)) printf("BAD: managed to insert an already existent key %u\n", key);
            if(rand_range_re(&d->seed, d->range)%8){ // i.e., with probability ~ 0.875
                if(!sl_remove_old(d->set, key, TRANSACTIONAL)) printf("BAD: failed to remove key %u after insertion \n", key);
                if(sl_contains_old(d->set, key, TRANSACTIONAL)) printf("BAD: managed to find key %u after removal \n", key);
            }
        }
        else {
            if(sl_add_old(d->set, key, TRANSACTIONAL)) printf("BAD insert contained key %u\n", key);
            if(rand_range_re(&d->seed, d->range)%8){ // i.e., with probability ~ 0.875
                if(!sl_remove_old(d->set, key, TRANSACTIONAL)) printf("BAD: failed to remove key %u after insertion \n", key);
                if(sl_contains_old(d->set, key, TRANSACTIONAL)) printf("BAD: managed to find key %u after removal \n", key);
            }
        }
    }

    printf("Thread %d local test done\n", lsb);
    return NULL;
}


void *test(void *data) {
	int i, unext, last = -1; 
	unsigned int val = 0;
	
	thread_data_t *d = (thread_data_t *)data;
	
	/* Create transaction */
	TM_THREAD_ENTER();

	    /* set up perf events for cache behavior */
    int fds[NUM_EVENTS];
    unsigned long counts[NUM_EVENTS];
    if (d->cache_monitoring) {
        struct perf_event_attr pe[NUM_EVENTS];
        pfm_initialize();

        for (i = 0; i < NUM_EVENTS; i++) {
            memset(&pe[i], 0, sizeof(struct perf_event_attr));
            pe[i].size = sizeof(struct perf_event_attr);
            pe[i].type = PERF_TYPE_RAW;
            pe[i].disabled = 1;
            pe[i].exclude_kernel = 1;
            pe[i].exclude_hv = 1;

            pfm_perf_encode_arg_t arg;
            memset(&arg, 0, sizeof(arg));
            arg.attr = &pe[i];

            if (pfm_get_os_event_encoding(events[i], PFM_PLM3, PFM_OS_PERF_EVENT, &arg) != PFM_SUCCESS) {
                fprintf(stderr, "Error encoding event %s\n", events[i]);
                exit(1);
            }

            fds[i] = perf_event_open(&pe[i], 0, -1, -1, 0);
            if (fds[i] == -1) {
                perror("perf_event_open failed");
                exit(1);
            }
        }
        for (i = 0; i < NUM_EVENTS; i++) ioctl(fds[i], PERF_EVENT_IOC_RESET, 0);
        for (i = 0; i < NUM_EVENTS; i++) ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0);
    }

	/* Wait on barrier */
	barrier_cross(d->barrier);
	
	/* start counting cache events*/
    if (d->cache_monitoring) {
        for (i = 0; i < NUM_EVENTS; i++) ioctl(fds[i], PERF_EVENT_IOC_ENABLE, 0);
    }

	/* Is the first op an update? */
	unext = (rand_range_re(&d->seed, 100) - 1 < d->update);

#ifdef ICC
	while (stop == 0) {
#else
        while (AO_load_full(&stop) == 0) {
#endif
		
		if (unext) { // update
			
			if (last < 0) { // add
				
				val = rand_range_re(&d->seed, d->range);
				if (sl_add_old(d->set, val, TRANSACTIONAL)) {
					d->nb_added++;
					last = val;
				} 				
				d->nb_add++;
				
			} else { // remove
				
				if (d->alternate) { // alternate mode (default)
					if (sl_remove_old(d->set, last, TRANSACTIONAL)) {
						d->nb_removed++;
					} 
					last = -1;
				} else {
					/* Random computation only in non-alternated cases */
					val = rand_range_re(&d->seed, d->range);
					/* Remove one random value */
					if (sl_remove_old(d->set, val, TRANSACTIONAL)) {
						d->nb_removed++;
						/* Repeat until successful, to avoid size variations */
						last = -1;
					} 
				}
				d->nb_remove++;
			}
			
		} else { // read
			
			if (d->alternate) {
				if (d->update == 0) {
					if (last < 0) {
						val = d->first;
						last = val;
					} else { // last >= 0
						val = rand_range_re(&d->seed, d->range);
						last = -1;
					}
				} else { // update != 0
					if (last < 0) {
						val = rand_range_re(&d->seed, d->range);
						//last = val;
					} else {
						val = last;
					}
				}
			}	else val = rand_range_re(&d->seed, d->range);
			
			if (sl_contains_old(d->set, val, TRANSACTIONAL)) 
				d->nb_found++;
			d->nb_contains++;
			
		}
		
		/* Is the next op an update? */
		if (d->effective) { // a failed remove/add is a read-only tx
			unext = ((100 * (d->nb_added + d->nb_removed))
							 < (d->update * (d->nb_add + d->nb_remove + d->nb_contains)));
		} else { // remove/add (even failed) is considered as an update
			unext = (rand_range_re(&d->seed, 100) - 1 < d->update);
		}
		
#ifdef ICC
	}
#else
	}
#endif /* ICC */
	
	if (d->cache_monitoring) {
        for (i = 0; i < NUM_EVENTS; i++) ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0);
        for (i = 0; i < NUM_EVENTS; i++) read(fds[i], &counts[i], sizeof(uint64_t));;

        d->L1_cache_accesses = counts[L1_CACHE_LOADS] + counts[L1_CACHE_STORES];
        d->L1_cache_misses = counts[L1_CACHE_MISSES];
        d->L3_cache_accesses = counts[L3_CACHE_LOADS] + counts[L3_CACHE_STORES];
        d->L3_cache_misses = counts[L3_CACHE_LOAD_MISSES] + counts[L3_CACHE_STORE_MISSES];
        d->total_cache_accesses = counts[TOTAL_CACHE_REFS];
        d->total_cache_misses = counts[TOTAL_CACHE_MISSES];

        for (i = 0; i < NUM_EVENTS; i++) close(fds[i]);
    }

	/* Free transaction */
	TM_THREAD_EXIT();
	
	return NULL;
}

void* set_populate(void* data) {
  int i;
  sl_key_t val;

  population_data_t *d = (population_data_t *)data;

  unsigned int seed = rand();
  while (i < d->to_populate) {
    val = rand_range_re(&seed, d->range);
    if (sl_add_old(d->set, val, DEFAULT_ELASTICITY)) {
      i++;
      if (d->lastp != NULL)
        *d->lastp = val;
    }
  }
}

void catcher(int sig)
{
	printf("CAUGHT SIGNAL %d\n", sig);
}

int main(int argc, char **argv)
{

	struct option long_options[] = {
		// These options don't set a flag
		{"help",                      no_argument,       NULL, 'h'},
		{"duration",                  required_argument, NULL, 'd'},
		{"initial-size",              required_argument, NULL, 'i'},
		{"num-threads",               required_argument, NULL, 'n'},
		{"range",                     required_argument, NULL, 'r'},
		{"seed",                      required_argument, NULL, 's'},
		{"update-rate",               required_argument, NULL, 'u'},
		{"elasticity",                required_argument, NULL, 'x'},
		{"cache monitoring", required_argument, NULL, 'm'},
        {"test mode", required_argument, NULL, 'v'},
		{"population parallelism",    required_argument, NULL, 'p'},
		{NULL, 0, NULL, 0}
	};
	
	struct sl_set *set;
	int i, c, size;
	unsigned int last = 0; 
	unsigned int val = 0;
	unsigned long reads, effreads, updates, effupds, aborts, aborts_locked_read, aborts_locked_write,
	aborts_validate_read, aborts_validate_write, aborts_validate_commit,
	aborts_invalid_memory, aborts_double_write, max_retries, failures_because_contention,
	L1_cache_accesses, L1_cache_misses, L3_cache_accesses, L3_cache_misses, total_cache_accesses, total_cache_misses;
	thread_data_t *data;
	population_data_t *pop_data;
	pthread_t *threads;
	pthread_attr_t attr;
	barrier_t barrier;
	struct timeval start, end;
	struct timespec timeout;
	int duration = DEFAULT_DURATION;
	int initial = DEFAULT_INITIAL;
	int nb_threads = DEFAULT_NB_THREADS;
	long range = DEFAULT_RANGE;
	int seed = DEFAULT_SEED;
	int update = DEFAULT_UPDATE;
	int unit_tx = DEFAULT_ELASTICITY;
	int alternate = DEFAULT_ALTERNATE;
	int effective = DEFAULT_EFFECTIVE;
	int cache_monitoring = DEFAULT_MONITOR;
    int test_mode = DEFAULT_TEST;
	int pop_par = DEFAULT_PARALLELISM;
	sigset_t block_set;
        struct sl_ptst *ptst;
        struct sl_node *temp;

        int unbalanced = DEFAULT_UNBALANCED;

	while(1) {
		i = 0;
		c = getopt_long(argc, argv, "hAf:d:i:t:r:S:u:x:U:m:v:p:"
										, long_options, &i);
		
		if(c == -1)
			break;
		
		if(c == 0 && long_options[i].flag == 0)
			c = long_options[i].val;
		
		switch(c) {
				case 0:
					break;
				case 'h':
					printf("intset -- STM stress test "
								 "(skip list)\n"
								 "\n"
								 "Usage:\n"
								 "  intset [options...]\n"
								 "\n"
								 "Options:\n"
								 "  -h, --help\n"
								 "        Print this message\n"
								 "  -A, --Alternate\n"
								 "        Consecutive insert/remove target the same value\n"
								 "  -f, --effective <int>\n"
								 "        update txs must effectively write (0=trial, 1=effective, default=" XSTR(DEFAULT_EFFECTIVE) ")\n"
								 "  -d, --duration <int>\n"
								 "        Test duration in milliseconds (0=infinite, default=" XSTR(DEFAULT_DURATION) ")\n"
								 "  -i, --initial-size <int>\n"
								 "        Number of elements to insert before test (default=" XSTR(DEFAULT_INITIAL) ")\n"
								 "  -t, --thread-num <int>\n"
								 "        Number of threads (default=" XSTR(DEFAULT_NB_THREADS) ")\n"
								 "  -r, --range <int>\n"
								 "        Range of integer values inserted in set (default=" XSTR(DEFAULT_RANGE) ")\n"
								 "  -S, --seed <int>\n"
								 "        RNG seed (0=time-based, default=" XSTR(DEFAULT_SEED) ")\n"
								 "  -u, --update-rate <int>\n"
								 "        Percentage of update transactions (default=" XSTR(DEFAULT_UPDATE) ")\n"
								 "  -x, --elasticity (default=4)\n"
								 "        Use elastic transactions\n"
								 "        0 = non-protected,\n"
								 "        1 = normal transaction,\n"
								 "        2 = read elastic-tx,\n"
								 "        3 = read/add elastic-tx,\n"
								 "        4 = read/add/rem elastic-tx,\n"
								 "        5 = fraser lock-free\n"
								 "  -m, --cache monitoring (default=0)\n"
                                 "        0 = do not monitor cache,\n"
                                 "        1 = monitor cache,\n"
                                 "  -v, --test mode (default=0)\n"
                                 "        0 = run benchmark,\n"
                                 "        non-zero = validate correctness, dictates number of validation txs,\n"
								 "  -p, --population parallelism <int>\n"
                				 "        Number of threads that take part in the set initialization(default=" XSTR(DEFAULT_PARALLELISM) ")\n"
								 );
					exit(0);
				case 'A':
					alternate = 1;
					break;
				case 'f':
					effective = atoi(optarg);
					break;
				case 'd':
					duration = atoi(optarg);
					break;
				case 'i':
					initial = atoi(optarg);
					break;
				case 't':
					nb_threads = atoi(optarg);
					break;
				case 'r':
					range = atol(optarg);
					break;
				case 'S':
					seed = atoi(optarg);
					break;
				case 'u':
					update = atoi(optarg);
					break;
				case 'x':
					unit_tx = atoi(optarg);
					break;
                case 'U':
                        unbalanced = atoi(optarg);
                        break;
				case 'm':
                    cache_monitoring = atoi(optarg);
                    break;
                case 'v':
                    test_mode = atoi(optarg);
                    break;
				case 'p':
					pop_par = atoi(optarg);
					break;
				case '?':
					printf("Use -h or --help for help\n");
					exit(0);
				default:
					exit(1);
		}
	}
	
	assert(duration >= 0);
	assert(initial >= 0);
	assert(nb_threads > 0);
	assert(range > 0 && range >= initial);
	assert(update >= 0 && update <= 100);
	
	printf("Set type     : skip list\n");
	printf("Duration     : %d\n", duration);
	printf("Initial size : %u\n", initial);
	printf("Nb threads   : %d\n", nb_threads);
	printf("Value range  : %ld\n", range);
	printf("Seed         : %d\n", seed);
	printf("Update rate  : %d\n", update);
	printf("Elasticity   : %d\n", unit_tx);
	printf("Alternate    : %d\n", alternate);
	printf("Efffective   : %d\n", effective);
	printf("Type sizes   : int=%d/long=%d/ptr=%d/word=%d\n",
				 (int)sizeof(int),
				 (int)sizeof(long),
				 (int)sizeof(void *),
				 (int)sizeof(uintptr_t));
	
	timeout.tv_sec = duration / 1000;
	timeout.tv_nsec = (duration % 1000) * 1000000;
	
	if ((data = (thread_data_t *)malloc(nb_threads * sizeof(thread_data_t))) == NULL) {
		perror("malloc");
		exit(1);
	}
	if ((pop_data = (population_data_t *)malloc(pop_par * sizeof(population_data_t))) == NULL) {
		perror("malloc");
		exit(1);
	}
	if ((threads = (pthread_t *)malloc(max(nb_threads, pop_par) * sizeof(pthread_t))) == NULL) {
		perror("malloc");
		exit(1);
	}
	
	if (seed == 0)
		srand((int)time(0));
	else
		srand(seed);
	
	levelmax = floor_log_2((unsigned int) initial);
	
        /* create the skip list set and do inits */
        ptst_subsystem_init();
        gc_subsystem_init();
        set_subsystem_init();
        set = set_new(1);
	stop = 0;

        global_seed = rand();
#ifdef TLS
	rng_seed = &global_seed;
#else /* ! TLS */
	if (pthread_key_create(&rng_seed_key, NULL) != 0) {
		fprintf(stderr, "Error creating thread local\n");
		exit(1);
	}
	pthread_setspecific(rng_seed_key, &global_seed);
#endif /* ! TLS */
	
	// Init STM 
	printf("Initializing STM\n");
	
	TM_STARTUP();
	
	// Populate set 
	printf("Adding %d entries to set\n", initial);
	if (pop_par == 1 || initial < 1000000) {
		i = 0;
		while (i < initial) {
			if (unbalanced)
							val = rand_range_re(&global_seed, initial);
			else
							val = rand_range_re(&global_seed, range);
			if (sl_add_old(set, val, 0)) {
				last = val;
				i++;
			}
		}
		size = set_size(set, 1);
		printf("Set size     : %d\n", size); // avoid sequential traversal during parallel population
		printf("Level max    : %d\n", levelmax);
	}
	else{ // currently does not support unbalanced
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
		for (i=0; i<pop_par; i++) {
			pop_data[i].lastp = (i==0) ? &last : NULL;
			pop_data[i].range = range;
			pop_data[i].set = set;
			pop_data[i].to_populate = initial/pop_par + ((i==0) ? (initial % pop_par) : 0);
			if (pthread_create(&threads[i], &attr, set_populate, (void *)(&pop_data[i])) != 0) {
				fprintf(stderr, "Error creating thread\n");
				exit(1);
			}
		}
		pthread_attr_destroy(&attr);
		for (i = 0; i < pop_par; i++) {
			if (pthread_join(threads[i], NULL) != 0) {
				fprintf(stderr, "Error waiting for thread completion\n");
				exit(1);
			}
		}
  	} 

        // nullify all the index nodes we created so
        // we can start again and rebalance the skip list
        bg_stop();

        // the following code is hacky since it creates a memory
        // leak - we cut off all the nodes in the index levels
        // without reclaiming them - this is only a one-off though
        ptst = ptst_critical_enter();
        set->top = inode_new(NULL, NULL, set->head, ptst);
        ptst_critical_exit(ptst);
        set->head->level = 1;
        temp = set->head->next;
        while (temp) {
            temp->level = 0;
            temp = temp->next;
        }

        // wait till the list is balanced
        bg_start(0);
        while (set->head->level < floor_log_2(initial)) {
            AO_nop_full();
        }
        printf("Number of levels is %d\n", set->head->level);
        bg_stop();
        bg_start(1000000);

        // Access set from all threads 
	barrier_init(&barrier, nb_threads + 1);
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	for (i = 0; i < nb_threads; i++) {
		printf("Creating thread %d\n", i);
		data[i].first = last;
		data[i].range = range;
		data[i].update = update;
		data[i].unit_tx = unit_tx;
		data[i].alternate = alternate;
		data[i].effective = effective;
		data[i].nb_add = 0;
		data[i].nb_added = 0;
		data[i].nb_remove = 0;
		data[i].nb_removed = 0;
		data[i].nb_contains = 0;
		data[i].nb_found = 0;
		data[i].nb_aborts = 0;
		data[i].nb_aborts_locked_read = 0;
		data[i].nb_aborts_locked_write = 0;
		data[i].nb_aborts_validate_read = 0;
		data[i].nb_aborts_validate_write = 0;
		data[i].nb_aborts_validate_commit = 0;
		data[i].nb_aborts_invalid_memory = 0;
		data[i].nb_aborts_double_write = 0;
		data[i].max_retries = 0;
		data[i].seed = rand();
		data[i].set = set;
		data[i].barrier = &barrier;
		data[i].failures_because_contention = 0;
		data[i].cache_monitoring = cache_monitoring;
        data[i].L1_cache_misses = 0;
        data[i].L1_cache_accesses = 0;
        data[i].L3_cache_misses = 0;
        data[i].L3_cache_accesses = 0;
        data[i].total_cache_misses = 0;
        data[i].total_cache_accesses = 0;
        data[i].validation_txs = test_mode;
        if (test_mode) {
            data[i].first = i;
            if (pthread_create(&threads[i], &attr, sanity_check, (void *)(&data[i])) != 0) {
                fprintf(stderr, "Error creating thread\n");
                exit(1);
            }
        }
        else {
            if (pthread_create(&threads[i], &attr, test, (void *) (&data[i])) != 0) {
                fprintf(stderr, "Error creating thread\n");
                exit(1);
            }
        }
	}
	pthread_attr_destroy(&attr);
	
	// Catch some signals 
	if (signal(SIGHUP, catcher) == SIG_ERR ||
			//signal(SIGINT, catcher) == SIG_ERR ||
			signal(SIGTERM, catcher) == SIG_ERR) {
		perror("signal");
		exit(1);
	}
	
	// Start threads 
	barrier_cross(&barrier);
	
	printf("STARTING...\n");
	gettimeofday(&start, NULL);
	if (duration > 0) {
		nanosleep(&timeout, NULL);
	} else {
		sigemptyset(&block_set);
		sigsuspend(&block_set);
	}
	
#ifdef ICC
	stop = 1;
#else	
	AO_store_full(&stop, 1);
#endif /* ICC */

        stop = 1;

	gettimeofday(&end, NULL);
	printf("STOPPING...\n");

	// Wait for thread completion 
	for (i = 0; i < nb_threads; i++) {
                if (pthread_join(threads[i], NULL) != 0) {
			fprintf(stderr, "Error waiting for thread completion\n");
			exit(1);
		}
	}

    if (test_mode) {
        printf("If no BAD messages were printed, all tests have passed. Otherwise... :(\n");
    }
    else {
        duration = (end.tv_sec * 1000 + end.tv_usec / 1000) - (start.tv_sec * 1000 + start.tv_usec / 1000);
        aborts = 0;
        aborts_locked_read = 0;
        aborts_locked_write = 0;
        aborts_validate_read = 0;
        aborts_validate_write = 0;
        aborts_validate_commit = 0;
        aborts_invalid_memory = 0;
        aborts_double_write = 0;
        failures_because_contention = 0;
        reads = 0;
        effreads = 0;
        updates = 0;
        effupds = 0;
        max_retries = 0;
		L1_cache_misses = 0;
		L1_cache_accesses = 0;
		L3_cache_misses = 0;
		L3_cache_accesses = 0;
		total_cache_misses = 0;
		total_cache_accesses = 0;

        for (i = 0; i < nb_threads; i++) {
            /*
				printf("Thread %d\n", i);
                printf("  #add        : %lu\n", data[i].nb_add);
                printf("    #added    : %lu\n", data[i].nb_added);
                printf("  #remove     : %lu\n", data[i].nb_remove);
                printf("    #removed  : %lu\n", data[i].nb_removed);
                printf("  #contains   : %lu\n", data[i].nb_contains);
                printf("  #found      : %lu\n", data[i].nb_found);
                printf("  #aborts     : %lu\n", data[i].nb_aborts);
                printf("    #lock-r   : %lu\n", data[i].nb_aborts_locked_read);
                printf("    #lock-w   : %lu\n", data[i].nb_aborts_locked_write);
                printf("    #val-r    : %lu\n", data[i].nb_aborts_validate_read);
                printf("    #val-w    : %lu\n", data[i].nb_aborts_validate_write);
                printf("    #val-c    : %lu\n", data[i].nb_aborts_validate_commit);
                printf("    #inv-mem  : %lu\n", data[i].nb_aborts_invalid_memory);
                printf("    #dup-w    : %lu\n", data[i].nb_aborts_double_write);
                printf("    #failures : %lu\n", data[i].failures_because_contention);
                printf("  Max retries : %lu\n", data[i].max_retries);
				printf("#L1 cache misses    : %lu\n", data[i].L1_cache_misses);
				printf("#L1 cache accesses  : %lu\n", data[i].L1_cache_accesses);
				printf("#L3 cache misses    : %lu\n", data[i].L3_cache_misses);
				printf("#L3 cache accesses  : %lu\n", data[i].L3_cache_accesses);
				printf("#total cache misses    : %lu\n", data[i].total_cache_misses);
				printf("#total cache accesses  : %lu\n", data[i].total_cache_accesses);
			*/
            aborts += data[i].nb_aborts;
            aborts_locked_read += data[i].nb_aborts_locked_read;
            aborts_locked_write += data[i].nb_aborts_locked_write;
            aborts_validate_read += data[i].nb_aborts_validate_read;
            aborts_validate_write += data[i].nb_aborts_validate_write;
            aborts_validate_commit += data[i].nb_aborts_validate_commit;
            aborts_invalid_memory += data[i].nb_aborts_invalid_memory;
            aborts_double_write += data[i].nb_aborts_double_write;
            failures_because_contention += data[i].failures_because_contention;
            reads += data[i].nb_contains;
            effreads += data[i].nb_contains +
                        (data[i].nb_add - data[i].nb_added) +
                        (data[i].nb_remove - data[i].nb_removed);
            updates += (data[i].nb_add + data[i].nb_remove);
            effupds += data[i].nb_removed + data[i].nb_added;
            size += data[i].nb_added - data[i].nb_removed;
			L1_cache_misses += data[i].L1_cache_misses;
			L1_cache_accesses += data[i].L1_cache_accesses;
			L3_cache_misses += data[i].L3_cache_misses;
			L3_cache_accesses += data[i].L3_cache_accesses;
			total_cache_misses += data[i].total_cache_misses;
			total_cache_accesses += data[i].total_cache_accesses;
            if (max_retries < data[i].max_retries)
                max_retries = data[i].max_retries;
        }
		if (pop_par == 1 || initial < 1000000) { // we do not calculate initial size if we populate in parallel
        	printf("Set size      : %d (expected: %d)\n", set_size(set, 1), size);
		}
        printf("Duration      : %d (ms)\n", duration);
        printf("#txs          : %lu (%f / s)\n", reads + updates, (reads + updates) * 1000.0 / duration);

        printf("#read txs     : ");
        if (effective) {
            printf("%lu (%f / s)\n", effreads, effreads * 1000.0 / duration);
            printf("  #contains   : %lu (%f / s)\n", reads, reads * 1000.0 / duration);
        } else printf("%lu (%f / s)\n", reads, reads * 1000.0 / duration);

        printf("#eff. upd rate: %f \n", 100.0 * effupds / (effupds + effreads));

        printf("#update txs   : ");
        if (effective) {
            printf("%lu (%f / s)\n", effupds, effupds * 1000.0 / duration);
            printf("  #upd trials : %lu (%f / s)\n", updates, updates * 1000.0 /
                                                              duration);
        } else printf("%lu (%f / s)\n", updates, updates * 1000.0 / duration);

        printf("#aborts       : %lu (%f / s)\n", aborts, aborts * 1000.0 / duration);
        printf("  #lock-r     : %lu (%f / s)\n", aborts_locked_read, aborts_locked_read * 1000.0 / duration);
        printf("  #lock-w     : %lu (%f / s)\n", aborts_locked_write, aborts_locked_write * 1000.0 / duration);
        printf("  #val-r      : %lu (%f / s)\n", aborts_validate_read, aborts_validate_read * 1000.0 / duration);
        printf("  #val-w      : %lu (%f / s)\n", aborts_validate_write, aborts_validate_write * 1000.0 / duration);
        printf("  #val-c      : %lu (%f / s)\n", aborts_validate_commit, aborts_validate_commit * 1000.0 / duration);
        printf("  #inv-mem    : %lu (%f / s)\n", aborts_invalid_memory, aborts_invalid_memory * 1000.0 / duration);
        printf("  #dup-w      : %lu (%f / s)\n", aborts_double_write, aborts_double_write * 1000.0 / duration);
        printf("  #failures   : %lu\n", failures_because_contention);
        printf("Max retries   : %lu\n", max_retries);
		if (cache_monitoring) {
			printf("#L1 cache misses    : %lu\n", L1_cache_misses);
			printf("#L1 cache accesses  : %lu\n", L1_cache_accesses);
			printf("#L1 cache miss%%  : %f\n", 100.0 * L1_cache_misses / L1_cache_accesses);
			printf("#L3 cache misses    : %lu\n", L3_cache_misses);
			printf("#L3 cache accesses  : %lu\n", L3_cache_accesses);
			printf("#L3 cache miss%%  : %f\n", 100.0 * L3_cache_misses / L3_cache_accesses);
			printf("#total cache misses    : %lu\n", total_cache_misses);
			printf("#total cache accesses  : %lu\n", total_cache_accesses);
			printf("#total cache miss%%  : %f\n", 100.0 * total_cache_misses / total_cache_accesses);
		}
    }

        bg_stop();
        bg_print_stats();

        /*sl_set_print(set, 1);*/
        gc_subsystem_destroy();

	// Delete set 
        set_delete(set);
	
	// Cleanup STM 
	TM_SHUTDOWN();
	
#ifndef TLS
	pthread_key_delete(rng_seed_key);
#endif /* ! TLS */
	
	free(threads);
	free(data);
	free(pop_data);
	
	return 0;
}

