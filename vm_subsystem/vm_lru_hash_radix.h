/*
 * This is a prototype implementation of a virtual memory subsystem
 * inspired by Linux's page cache. It uses:
 *  - a least-recently-used (LRU) doubly-linked list to maintain
 *    eviction order of cached pages.
 *  - a global hashmap to quickly look up inodes (files) that
 *    are currently cached in memory.
 *  - per-inode radix trees to efficiently locate individual
 *    page indices within a file's cached contents.
 *
 * Workflow:
 * 1. On file access, check the hashmap to see if the file's inode
 *    is already cached.
 * 2. If the file is cached, consult its radix tree to determine
 *    whether the requested page(s) are in memory.
 * 3. If found, serve them from memory and move the corresponding
 *    vm_page_t entries to the head of the LRU list.
 * 4. If missing, map the pages to the virtual memory address, 
 *    insert them into the file's radix tree, and add them to 
 *    the head of the LRU list.
 *
 * Eviction:
 * - If the global LRU list reaches capacity (LRU_LIST_MAX_CAPACITY)
 *   evict the least-recently-used page(s) from the tail.
 * - The evicted pages are removed from its inode's radix tree.
 * - If the inode has no remaining cached pages, it may also be
 *   removed from the inode hashmap.
*/

#ifndef VM_LRU_HASH_RADIX_H
#define VM_LRU_HASH_RADIX_H

#include <sys/types.h>

#define LRU_LIST_MAX_CAPACITY 128
#define HASH_BUCKETS_CAPACITY 256

#define RADIX_BITS  4
#define RADIX_SIZE  (1 << RADIX_BITS)
#define RADIX_MAX   (RADIX_SIZE - 1)

// info for page cached in memory
typedef struct _vm_page {
  ino_t     inode;
  uint64_t  page_index;
  // bool      dirty;
  // bool      referenced;
  // may use these eventually, but idt they fit my use case now
  struct _vm_page *prev;
  struct _vm_page *next;

} vm_page_t;

// node storing pointer to page (if leaf)
// and array of 2^RADIX_BITS pointers to further nodes
typedef struct _radix_node {
  struct _radix_node  *slots[RADIX_SIZE];
  vm_page_t           *page;

} radix_node_t;

// radix tree for quickly finding page indices
typedef struct _radix_tree {
  radix_node_t *root;

} radix_tree_t;

// stores inode and radix tree
typedef struct _vm_file {
  ino_t         inode;
  radix_tree_t  tree;

} vm_file_t;

// stores inode as key, pointer to radix tree,
// and pointer to next entry (in case of collisions)
typedef struct _hashmap_entry {
  ino_t                 key;
  vm_file_t             *file;
  struct _hashmap_entry *next; // for chaining

} hashmap_entry_t;

// hashmap to keep track of inodes/radix-trees
typedef struct _hashmap {
  hashmap_entry_t **buckets;
  // capacity stored as macro
} hashmap_t;

// doubly-linked list for fast insertion and eviction
typedef struct {
  vm_page_t     *head;
  vm_page_t     *tail;
  size_t        size;
  // capacity stored as macro
  hashmap_t     *map;

} vm_list_t;
