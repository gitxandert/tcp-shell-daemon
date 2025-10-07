#include "vm_lru_hash_radix.h"

// we need:
// - locks
// - init functions   V
// - hash algorithms  V
// - insert into list V
// - insert into hash
// - insert into tree
// - look up hash     V
// - look up tree     V
// - remove from list V
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
  VM_LIST.head = VM_LIST.tail = NULL;
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

hashmap_entry_t *hashmap_entry_create(ino_t inode, uint64_t page_index) {
  vm_page_t *new_page = malloc(sizeof(vm_page_t));
  new_page->inode = inode;
  new_page->page_index = page_index;
  // set dirty and referenced later maybe, if I need them

  radix_node_t *new_node = calloc(1, sizeof(radix_node_t));
  new_node->page = new_page;

  radix_tree_t new_tree;
  new_tree.root = new_node;

  vm_file_t *new_file = malloc(sizeof(vm_file_t));
  new_file->inode = inode;
  new_file->tree = new_tree; 

  hashmap_entry_t *new_entry = malloc(sizeof(hashmap_entry_t));
  new_entry->key = inode;
  new_entry->file = new_file;
  new_entry->next = NULL;

  return new_entry;
}

void hashmap_entry_delete(ino_t inode, uint64_t page_index) {
  size_t hash = hash_page(inode, page_index);

  if (VM_LIST.map->buckets[hash]) {
    hashmap_entry_t *tmp = VM_LIST.map->buckets[hash];
    // sift through entry chain until key matches or end of chain
    while (tmp->key != inode && tmp->next)
      tmp = tmp->next;

    if (tmp->key == inode) {
      if (tmp->next)
        tmp = tmp->next;
      else
        tmp = NULL;
    }

    // if at end of chain and key doesn't match, do nothing
  }
}
//
// end hashmap_t functions

// radix_tree_t functions
//
size_t get_radix_byte(uint64_t page_index, int level) {
  int shift = level * RADIX_BITS;

  return (size_t)(page_index << shift & 0xF);
}

void radix_manage_level(radix_node_t *node, int level) {
  
}

void radix_insert(ino_t inode, uint64_t page_index) {
  // - first hash,
  //   then go to corresponding bucket
  // - search for hashmap_entry_t with same inode
  //   - if none exists, chain a new hashmap_entry_t,
  //      then set inode to inode and create new vm_file_t 
  //      (also setting its inode to inode)
  // - for each level of the radix tree
  //   - check if vm_page_t exists
  //      - if the page exists, compare current level radix bytes
  //         - if they're the same, then both pages need to drop down,
  //            and the current vm_page_t has to be set to NULL
  //            - might need a separate function for this; radix_move?
  //              - seems recursive...
  //      - if it doesn't exist, then check if any slots are full
  //        - if yes, then bytes need to be compared
  //          - if the page_index's byte at this level is the same as an
  //            occupied slot's, then it'll need to drop down
  size_t hash = hash_page(inode, page_index);
  hashmap_entry_t *cur = VM_LIST.map->buckets[hash];

  if (!cur) {
    // if there is no hashmap entry for this file, make one
    cur = hashmap_entry_create(inode, page_index);
  else {
    // find the entry in the collision chain that corresponds to the file
    while (cur->key != inode && cur->next)
      cur = cur->next;

    if (cur->key != inode)
      // end of chain, but there is no file stored in the bucket
      cur->next = hashmap_entry_create(inode, page_index);
    else {
      // here is where we need some function that manages node insertions
      // if there's a page stored at any level, it de facto needs to be moved down
      radix_node_t *root = cur->file->tree.root;
      radix_manage_level(root, 0);
    }
  }
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
//
// end radix_tree_t functions

// vm_list_t functions
//
// moves a (new or existing) page to the head
void vm_list_push(vm_page_t* new_head) {
  if (VM_LIST.head == NULL) {
    VM_LIST.head = new_head;
    VM_LIST.tail = VM_LIST.head;
  } else {
    new_head->prev = NULL;
    new_head->next = VM_LIST.head;
    VM_LIST.head->prev = new_head;
    VM_LIST.head = new_head;
    // if this is the second page,
    // VM_LIST.tail still points to previous head
  }
}

// this moves existing nodes to the head
void vm_list_move_to_head(vm_page_t *new_head) {
  // if this node is already the head of the list or NULL
  if (VM_LIST.head == new_head || !new_head)
   return;

  // previous node points forward to next node
  if (new_head->prev)
   new_head->prev->next = new_head->next;
  // next node points backward to previous node
  if (new_head->next)
    new_head->next->prev = new_head->prev;
  else // if there is no next, then current node is the tail
    VM_LIST.tail = new_head->prev;

  // they are no longer pointing to the "current" node

  // push new_head to head
  vm_list_push(new_head);
}

// remove tail if it and its prev exist
void vm_list_pop() {
  if (VM_LIST.tail && VM_LIST.tail->prev) {
    VM_LIST.tail = VM_LIST.tail->prev;
    VM_LIST.tail->next = NULL;
  }
}
//
// end vm_list_t functions

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

  vm_page_t *req_page = malloc(sizeof(vm_page_t));
  if (!get_page(tree, req_page, page_index))
    return false;

  // if page is in memory, move to front of LRU list
  vm_list_move_to_head(req_page);
  return true;
}

