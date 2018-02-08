/*
 * Created by Ivo Georgiev on 2/9/16.
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h> // for perror()

#include "mem_pool.h"

/*************/
/*           */
/* Constants */
/*           */
/*************/
static const float      MEM_FILL_FACTOR                 = 0.75;
static const unsigned   MEM_EXPAND_FACTOR               = 2;

static const unsigned   MEM_POOL_STORE_INIT_CAPACITY    = 20;
static const float      MEM_POOL_STORE_FILL_FACTOR      = 0.75;
static const unsigned   MEM_POOL_STORE_EXPAND_FACTOR    = 2;

static const unsigned   MEM_NODE_HEAP_INIT_CAPACITY     = 40;
static const float      MEM_NODE_HEAP_FILL_FACTOR       = 0.75;
static const unsigned   MEM_NODE_HEAP_EXPAND_FACTOR     = 2;

static const unsigned   MEM_GAP_IX_INIT_CAPACITY        = 40;
static const float      MEM_GAP_IX_FILL_FACTOR          = 0.75;
static const unsigned   MEM_GAP_IX_EXPAND_FACTOR        = 2;



/*********************/
/*                   */
/* Type declarations */
/*                   */
/*********************/
typedef struct _alloc {
    char *mem;
    size_t size;
} alloc_t, *alloc_pt;

typedef struct _node {
    alloc_t alloc_record;
    unsigned used;
    unsigned allocated;
    struct _node *next, *prev; // doubly-linked list for gap deletion
} node_t, *node_pt;

typedef struct _gap {
    size_t size;
    node_pt node;
} gap_t, *gap_pt;

typedef struct _pool_mgr {
    pool_t pool;
    node_pt node_heap;
    unsigned total_nodes;
    unsigned used_nodes;
    gap_pt gap_ix;
    unsigned gap_ix_capacity;
} pool_mgr_t, *pool_mgr_pt;



/***************************/
/*                         */
/* Static global variables */
/*                         */
/***************************/
static pool_mgr_pt *pool_store = NULL; // an array of pointers, only expand
static unsigned pool_store_size = 0;
static unsigned pool_store_capacity = 0;



/********************************************/
/*                                          */
/* Forward declarations of static functions */
/*                                          */
/********************************************/
static alloc_status _mem_resize_pool_store();
static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr);
static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr);
static alloc_status
        _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                           size_t size,
                           node_pt node);
static alloc_status
        _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                size_t size,
                                node_pt node);
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr);
static alloc_status _mem_invalidate_gap_ix(pool_mgr_pt pool_mgr);



/****************************************/
/*                                      */
/* Definitions of user-facing functions */
/*                                      */
/****************************************/
alloc_status mem_init() {
    // ensure that it's called only once until mem_free
    // note: holds pointers only, other functions to allocate/deallocate
    if (pool_store == NULL) {
        // allocate the pool store with initial capacity
        pool_store = calloc(MEM_POOL_STORE_INIT_CAPACITY, sizeof(pool_mgr_pt));
        pool_store_capacity = MEM_POOL_STORE_INIT_CAPACITY;
        return ALLOC_OK; // memory has (hopefully) been allocated
    }
    else {
        // if pool_store != NULL, mem_init() was called again before mem_free (bad)
        return ALLOC_CALLED_AGAIN;
    }
}

alloc_status mem_free() {
    // ensure that it's called only once for each mem_init
    // if pool_store == NULL then we called mem_free when already freed
    if (pool_store == NULL) {
        return ALLOC_CALLED_AGAIN;
    }
    // make sure all pool managers have been deallocated
    // if an entry in the pool store is not null, not freed
    for (int i = 0; i < pool_store_size; i++) {
        if (pool_store[i] != NULL) {
            return ALLOC_NOT_FREED;
        }
    }
    // can free the pool store array
    free(pool_store);
    // update static variables, zero out and nullify pool_store
    pool_store_capacity = 0;
    pool_store_size = 0;
    pool_store = NULL;

    return ALLOC_OK; //everything might have worked..
}

pool_pt mem_pool_open(size_t size, alloc_policy policy) {
    // make sure there the pool store is allocated
    if (pool_store == NULL) { // no pool_store has yet been allocated
        return NULL;
    }
    // expand the pool store, if necessary
    /*
     * alloc_status ret_status = _mem_resize_pool_store();
     *assert(ret_status == ALLOC_OK); //end program if alloc NOT ok
     *if (ret_status != ALLOC_OK) {
     *    return NULL; //need to expand the pool store
     *}
     */
    // allocate a new mem pool mgr
    pool_mgr_pt new_pmgr = (pool_mgr_pt) calloc(1, sizeof(pool_mgr_t));
    // check success, on error return null
    assert(new_pmgr);
    if (new_pmgr == NULL) {
        return NULL;
    }
    // allocate a new memory pool
    void * new_mem = malloc(size);
    // allocate mem, set all parameters
    new_pmgr->pool.mem = new_mem;   //mem holds size bytes
    new_pmgr->pool.policy = policy;
    new_pmgr->pool.total_size = size;
    new_pmgr->pool.num_allocs = 0;  // no nodes have been allocated
    new_pmgr->pool.num_gaps = 1;    // the entire thing is a gap
    new_pmgr->pool.alloc_size = 0;  // pool has nothing allocated
    // check success, on error deallocate mgr and return null
    assert(new_pmgr->pool.mem);
    // some error occurred, the pool was not allocated
    if (new_pmgr->pool.mem == NULL) {
        free(new_pmgr);
        new_pmgr = NULL;
        return NULL;
    }
    // allocate a new node heap
    node_pt new_nheap = (node_pt)calloc(MEM_NODE_HEAP_INIT_CAPACITY, sizeof(node_t));
    // check success, on error deallocate mgr/pool and return null
    assert(new_nheap);
    if (new_nheap == NULL) {
        free(new_pmgr);
        new_pmgr = NULL;
        free(new_mem);
        new_mem = NULL;
        return NULL;
    }
    // allocate a new gap index
    gap_pt new_gapix = (gap_pt)calloc(MEM_GAP_IX_INIT_CAPACITY, sizeof(gap_t));
    // check success, on error deallocate mgr/pool/heap and return null
    assert(new_gapix);
    if (new_gapix == NULL) {
        free(new_pmgr);
        new_pmgr = NULL;
        free(new_mem);
        new_mem = NULL;
        free(new_nheap);
        new_nheap = NULL;
    }
    // assign all the pointers and update meta data:
    new_pmgr->node_heap = new_nheap;
    new_pmgr->total_nodes = MEM_NODE_HEAP_INIT_CAPACITY;
    new_pmgr->used_nodes = 1;     //just the 1 gap
    new_pmgr->gap_ix = new_gapix;
    new_pmgr->gap_ix_capacity = MEM_GAP_IX_INIT_CAPACITY;

    //   initialize top node of node heap
    new_pmgr->node_heap[0].alloc_record.size = size;
    new_pmgr->node_heap[0].alloc_record.mem = new_mem;
    new_pmgr->node_heap[0].used = 1;
    new_pmgr->node_heap[0].allocated = 0;
    new_pmgr->node_heap[0].next = NULL;
    new_pmgr->node_heap[0].prev = NULL;
    //   initialize top node of gap index
    new_pmgr->gap_ix[0].size = size;
    new_pmgr->gap_ix[0].node = new_pmgr->node_heap;

    //   initialize pool mgr
    //   link pool mgr to pool store
    // find the first empty position in the pool_store
    // and link the new pool mgr to that location
    int i = 0;
    while (pool_store[i] != NULL) {
        ++i;
    }
    pool_store[i] = new_pmgr;
    ++pool_store_size;
    // return the address of the mgr, cast to (pool_pt)

    return (pool_pt)new_pmgr;
}

alloc_status mem_pool_close(pool_pt pool) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    // check if this pool is allocated
    // check if pool has only one gap
    // check if it has zero allocations
    // free memory pool
    // free node heap
    // free gap index
    // find mgr in pool store and set to null
    // note: don't decrement pool_store_size, because it only grows
    // free mgr

    return ALLOC_FAIL;
}

void * mem_new_alloc(pool_pt pool, size_t size) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    // check if any gaps, return null if none
    // expand heap node, if necessary, quit on error
    // check used nodes fewer than total nodes, quit on error
    // get a node for allocation:
    // if FIRST_FIT, then find the first sufficient node in the node heap
    // if BEST_FIT, then find the first sufficient node in the gap index
    // check if node found
    // update metadata (num_allocs, alloc_size)
    // calculate the size of the remaining gap, if any
    // remove node from gap index
    // convert gap_node to an allocation node of given size
    // adjust node heap:
    //   if remaining gap, need a new node
    //   find an unused one in the node heap
    //   make sure one was found
    //   initialize it to a gap node
    //   update metadata (used_nodes)
    //   update linked list (new node right after the node for allocation)
    //   add to gap index
    //   check if successful
    // return allocation record by casting the node to (alloc_pt)

    return NULL;
}

alloc_status mem_del_alloc(pool_pt pool, void * alloc) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    // get node from alloc by casting the pointer to (node_pt)
    // find the node in the node heap
    // this is node-to-delete
    // make sure it's found
    // convert to gap node
    // update metadata (num_allocs, alloc_size)
    // if the next node in the list is also a gap, merge into node-to-delete
    //   remove the next node from gap index
    //   check success
    //   add the size to the node-to-delete
    //   update node as unused
    //   update metadata (used nodes)
    //   update linked list:
    /*
                    if (next->next) {
                        next->next->prev = node_to_del;
                        node_to_del->next = next->next;
                    } else {
                        node_to_del->next = NULL;
                    }
                    next->next = NULL;
                    next->prev = NULL;
     */

    // this merged node-to-delete might need to be added to the gap index
    // but one more thing to check...
    // if the previous node in the list is also a gap, merge into previous!
    //   remove the previous node from gap index
    //   check success
    //   add the size of node-to-delete to the previous
    //   update node-to-delete as unused
    //   update metadata (used_nodes)
    //   update linked list
    /*
                    if (node_to_del->next) {
                        prev->next = node_to_del->next;
                        node_to_del->next->prev = prev;
                    } else {
                        prev->next = NULL;
                    }
                    node_to_del->next = NULL;
                    node_to_del->prev = NULL;
     */
    //   change the node to add to the previous node!
    // add the resulting node to the gap index
    // check success

    return ALLOC_FAIL;
}

void mem_inspect_pool(pool_pt pool,
                      pool_segment_pt *segments,
                      unsigned *num_segments) {
    // get the mgr from the pool
    // allocate the segments array with size == used_nodes
    // check successful
    // loop through the node heap and the segments array
    //    for each node, write the size and allocated in the segment
    // "return" the values:
    /*
                    *segments = segs;
                    *num_segments = pool_mgr->used_nodes;
     */
}



/***********************************/
/*                                 */
/* Definitions of static functions */
/*                                 */
/***********************************/
static alloc_status _mem_resize_pool_store() {
    // check if necessary

    if (((float) pool_store_size / pool_store_capacity)
        > MEM_POOL_STORE_FILL_FACTOR) {
        pool_store = realloc(pool_store, pool_store_capacity * MEM_POOL_STORE_EXPAND_FACTOR * sizeof(pool_mgr_pt));
        pool_store_capacity = pool_store_capacity * MEM_POOL_STORE_EXPAND_FACTOR;
    }
    return ALLOC_OK;
}

static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr) {
    // see above

    return ALLOC_FAIL;
}

static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr) {
    // see above

    return ALLOC_FAIL;
}

static alloc_status _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                                       size_t size,
                                       node_pt node) {

    // expand the gap index, if necessary (call the function)
    alloc_status result = _mem_resize_gap_ix(pool_mgr);
    assert(result == ALLOC_OK);
    if (result != ALLOC_OK) {
        return ALLOC_FAIL;
    }

    // add the entry at the end
    // clarity: gap_ix[pool_mgr->pool.num_gaps] should refer to
    // the index just behind the last gap in the pool.
    // Set size and pointer to the node of this gap node
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].size = size;
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].node = node;
    // update metadata (num_gaps)
    pool_mgr->pool.num_gaps ++;
    // sort the gap index (call the function)
    result = _mem_sort_gap_ix(pool_mgr);
    //assert(result == ALLOC_OK);
    // check success
    if (result != ALLOC_OK) {
        return ALLOC_FAIL;
    }
    return result;
}

static alloc_status _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                            size_t size,
                                            node_pt node) {
    // find the position of the node in the gap index
    // loop from there to the end of the array:
    //    pull the entries (i.e. copy over) one position up
    //    this effectively deletes the chosen node
    // update metadata (num_gaps)
    // zero out the element at position num_gaps!

    return ALLOC_FAIL;
}

// note: only called by _mem_add_to_gap_ix, which appends a single entry
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr) {
    // the new entry is at the end, so "bubble it up"
    // loop from num_gaps - 1 until but not including 0:
    for (int i = pool_mgr->pool.num_gaps - 1; --i; i > 0) {
        /* if the size of the current entry is less than the previous (u - 1)
         * or if the sizes are the same but the current entry points to a
         * node with a lower address of pool allocation address (mem)
         * swap them (by copying) (remember to use a temporary variable)
         */
        if ((pool_mgr->gap_ix[i].size < pool_mgr->gap_ix[i-1].size)
            || ((pool_mgr->gap_ix[i].size == pool_mgr->gap_ix[i-1].size)
            && (pool_mgr->gap_ix[i].node->alloc_record.mem <
                pool_mgr->gap_ix[i].node->alloc_record.mem))) {
            gap_t tmp_gap = pool_mgr->gap_ix[i];
            pool_mgr->gap_ix[i] = pool_mgr->gap_ix[i-1];
            pool_mgr->gap_ix[i-1] = tmp_gap;
        }
    }
    return ALLOC_OK;
}

static alloc_status _mem_invalidate_gap_ix(pool_mgr_pt pool_mgr) {
    return ALLOC_FAIL;
}

