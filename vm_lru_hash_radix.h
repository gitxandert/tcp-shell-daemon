/*
 * This is a prototype implementation of a virtual memory subsystem
 * inspired by Linux's page cache. It uses:
 *  - a least-recently-used (LRU) doubly-linked list to maintain
 *    eviction order of cached pages
 *  - a global hashmap to quickly look up inodes (files) that
 *    are currently cached in memory
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
 * 4. If missing, load the pages from disk, insert them into the
 *    file's radix tree, and add them to the head of the LRU list.
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
  bool      dirty;
  bool      referenced;

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

// we need:
// - locks
// - init functions
// - hash algorithm
// - insert into list
// - insert into hash
// - insert into tree
// - look up list
// - look up hash
// - look up tree
// - remove from list
// - remove from hash
// - remove from tree
//
// So here's the flow:
// 1. Client opens file.
// 2. Prebuffer adds n pages from the buffer, either from VM or disk.
// 3. Feed pages from buffer into audio stream (can use ALSA for this).
// 4. Fill buffer up to a certain point, then wait until stream requests more.
//
// Okay, for now, let's just implement the insert functions.

// assume global list
vm_list_t VM_LIST;

void vm_list_init() {
  VM_LIST.head = VM_LIST->tail = NULL;
  VM_LIST.size = 0;
  VM_LIST.map = malloc(sizeof(hashmap_entry_t *) * HASH_BUCKETS_CAPACITY);
}

// hashmap_t functions
//
// hash with inode and page index (i.e. with what would be stored in vm_page_t)
size_t hash_page(ino_t key, uint64_t page_index) {
  uint64_t h = (uint64_t)key * 11400714819323198485llu; // Knuth constant
  h ^= page_index + 0x9e3779b97f4a7c15llu + (h << 6) + (h >> 2);
  return (size_t)(h % HASH_BUCKETS_CAPACITY);
}

void insert_into_hashmap(ino_t inode, uint64_t page_index) {
}

// radix_tree_t functions
//
size_t get_radix_byte(uint64_t page_index, int level) {
  int shift = (12 - level) * 4;

  return (size_t)(page_index >> shift & 0xF);
}

bool get_page(radix_tree_t *tree, vm_page_t *ret_page, uint64_t page_index) {
  radix_node_t *cur = tree->root;
  int level = 0;
  // if there's no vm_page_t stored at the current node
  // then check the slots to see if anything is stored at the next level
  while (cur && !cur->page) {
    size_t cur_byte = get_radix_byte(page_index, level);
    if (cur->slots[cur_byte]) // go to the next level
      cur = cur->slots[cur_byte];
    else // the next level at this index is empty
      return false;

    level++;
  }

  if (cur && cur->page && cur->page->page_index == page_index) {
    ret_page = cur->page;
    return true;
  }

  return false;
}

void move_to_head(vm_page_t *new_head) {
  // if this node is already the head of the list or NULL
  if (VM_LIST.head == new_head || !new_head) 
   return;

  // previous node points forward to next node
  if (new_head->prev)
   new_head->prev->next = new_head->next;
  // next node points backward to previous node
  if (new_head->next)
    new_head->next->prev = new_head->prev;

  // they are no longer pointing to the "current" node

  // current node points forward to list's head
  new_head->prev = NULL;
  new_head->next = VM_LIST.head;
  // list's head points backward to current node
  VM_LIST.head->prev = new_head;

  // set list's head to current node (next node is old head,
  // which points back to current node)
  VM_LIST.head = new_head;
}

// verify page is cached
//
bool is_in_memory(ino_t inode, uint64_t page_index) {
  // if no hashmap_entry for this file, return
  size_t hm_idx = hash_page(inode, page_index);
  if (!VM_LIST.map[hm_idx])
    return false;

  hashmap_entry_t *hm_ent = VM_LIST.map[hm_idx];

  // if inode not in hash, return false
  while (hm_ent->key != inode && hm_ent->next != NULL)
    hm_ent = hm_ent->next;
  if (hm_ent->key != inode)
    return false;

  // if not in tree, return false
  radix_tree_t *tree = &(hm_ent->file->tree);
  if (tree->root == NULL)
    return false;

  // 
  vm_page_t *req_page = malloc(sizeof(vm_page_t *));
  if (!get_page(tree, req_page, page_index))
    return false;

  move_to_head(req_page);
  return true;
}
