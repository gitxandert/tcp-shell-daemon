#include "vm_lru_hash_radix.h"

// NEED TO IMPLEMENT vm_push WITH EVERY NEW vm_page_t

// we need:
// - locks
// - init functions   V
// - hash algorithms  V
// - insert into list V
// - insert into hash V
// - insert into tree V
// - look up hash     V
// - look up tree     V
// - remove from list V
// - remove from hash V
// - remove from tree V
//
// So here's the flow:
// 1. Client opens file.
// 2. Prebuffer adds n pages from the buffer, either from VM or disk.
// 3. Feed pages from buffer into audio stream (can use ALSA for this).
// 4. Fill buffer up to a certain point, then wait until stream requests more.
//

// init global list
//
vm_list_t VM_LIST;

void vm_list_init() {
  VM_LIST.head = VM_LIST.tail = NULL;
  VM_LIST.size = 0;
  VM_LIST.map = malloc(sizeof(hashmap_entry_t *) * HASH_BUCKETS_CAPACITY);
}
//
// end init

// vm_page_t functions
//
vm_page_t *vm_page_create(ino_t inode, uint64_t page_index) {
  vm_page_t *new_page = malloc(sizeof(vm_page_t));
  new_page->inode = inode;
  new_page->page_index = page_index;
  // might set dirty and referenced bits eventually
  return new_page;
}
//
// end vm_page_t functions

// hashmap_t functions
//
// hash with inode and page index (i.e. with what would be stored in vm_page_t)
size_t hash_page(ino_t key, uint64_t page_index) {
  uint64_t h = (uint64_t)key * 11400714819323198485llu; // Knuth constant
  h ^= page_index + 0x9e3779b97f4a7c15llu + (h << 6) + (h >> 2);
  return (size_t)(h % HASH_BUCKETS_CAPACITY);
}

hashmap_entry_t *hashmap_entry_create(ino_t inode, uint64_t page_index) {
  vm_page_t *new_page = vm_page_create(inode, page_index);
  // set dirty and referenced later maybe, if necessary

  radix_node_t *new_node = calloc(1, sizeof(radix_node_t));
  new_node->page = new_page;

  vm_file_t *new_file = malloc(sizeof(vm_file_t));
  new_file->inode = inode;
  new_file->tree.root = new_node;

  hashmap_entry_t *new_entry = malloc(sizeof(hashmap_entry_t));
  new_entry->key = inode;
  new_entry->file = new_file;
  new_entry->next = NULL;

  return new_entry;
}
//
// end hashmap_t functions

// radix_tree_t functions
//
size_t get_radix_byte(uint64_t page_index, int level) {
  int shift = level * RADIX_BITS;
  // shift right RADIX_BITS * level times and return just the rightmost RADIX_BITS bits
  return (size_t)(page_index >> shift);
}

void radix_descend(radix_node_t *upper_level, int level, ino_t inode, uint64_t page_index) {
    size_t rad_byte = get_radix_byte(inode, page_index);
    radix_node_t *lower_level;

    if (upper_level->slots[rad_byte]) {
      // the lower level exists, so go down there
      lower_level = upper_level->slots[rad_byte];
      radix_manage_level(lower_level, level++, inode, page_index);
    } else {
      // the lower level does not exist yet; initialize here
      lower_level = calloc(1, sizeof(radix_node_t));
      
      vm_page_t *new_page = vm_page_create(inode, page_index);
      lower_level->page = new_page;
    }
}

void radix_manage_level(radix_node_t *node, int level, ino_t inode, uint64_t page_index) {
  if (node->page) {
    // there's a page here, so the page has to descend,
    // as well as the incoming inode + page_index
    radix_descend(node, level, inode, node->page->page_index);
    radix_descend(node, level, inode, page_index);
    
    // set page to NULL to bypass future checks
    node->page = NULL;
  } else {
    size_t rad_byte = get_radix_byte(inode, page_index);
    if (node->slots[rad_byte]) {
      // there is a lower level at this index, so go down
      radix_manage_level(node->slots[rad_byte], level++, inode, page_index);
    } else {
      // check to see if there are any lower levels at all
      bool lower_level = false;
      size_t i = 0;
      while (!lower_level && i < RADIX_SIZE) {
        if (node->slots[i])
          lower_level = true;
        i++;
      }

      // making a new page regardless, so allocate now
      vm_page_t *new_page = vm_page_create(inode, page_index);

      if (lower_level) {
        radix_node_t *new_node = calloc(1, sizeof(radix_node_t));
        new_node->page = new_page;
        node->slots[--i] = new_node; // decrement i to go back to lower-level index
      } else {
        // everything is NULL, so set the current level's page
        // I don't think this should ever occur tbh
        node->page = new_page;
      }
    }
  }
}

void radix_insert(ino_t inode, uint64_t page_index) {
  size_t hash = hash_page(inode, page_index);
  hashmap_entry_t *cur = VM_LIST.map->buckets[hash];

  if (!cur) {
    // there is no hashmap entry for this file, make one
    cur = hashmap_entry_create(inode, page_index);
  else {
    // find the entry in the collision chain that corresponds to the file
    while (cur->key != inode && cur->next)
      cur = cur->next;

    if (cur->key != inode)
      // end of chain, but the file has no bucket
      cur->next = hashmap_entry_create(inode, page_index);
    else
      // something already exists at root
      radix_manage_level(cur->file->tree.root, 0, inode, page_index);
  }
}

bool get_node(radix_tree_t *tree, radix_node_t *ret_node, uint64_t page_index) {
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
    ret_node = cur;
    return true;
  }

  return false;
}

int radix_node_free(radix_tree_t *tree, uint64_t page_index) {
  if (!tree->root || !tree->root->page) return -1;

  if (tree->root->page) {
    // the root has a page (does not have lower levels)
    if (tree->root->page->page_index == page_index) {
      free(tree->root->page);
      free(tree->root);
      // return negative to signal that the file
      // and hashmap entry should be deallocated
      return -1;
    }
  } else {
    radix_node_t *req_node;
    if (get_node(tree, req_page, page_index)) {
      // free the page, then the node
      free(req_node->page);
      free(req_node);
      return 0;
    }
  }
}

void radix_node_delete(ino_t inode, uint64_t page_index) {
  size_t hash = hash_page(inode, page_index);
  hashmap_entry_t *cur = VM_LIST.map->buckets[hash];
  hashmap_entry_t *prev = NULL;

  while (cur) {
    if (cur->key == inode) {
      if (cur->file) {
        if (radix_node_free(cur->file->tree, page_index) < 0) {
          // the file's tree no longer points to anything
          free(cur->file);

          // current entry is skipped
          if (prev)
            prev->next = cur->next;
          else
            VM_LIST.map->buckets[hash] = cur->next;

          free(cur);
        }
      }
      return;
    }

    prev = cur;
    cur = cur->next;
  }
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
    vm_page_t *old_tail = VM_LIST.tail;

    VM_LIST.tail = VM_LIST.tail->prev;
    VM_LIST.tail->next = NULL;

    // this function will call free on the page in the
    // radix tree, so no need to free old_tail here
    radix_node_delete(old_tail->inode, old_tail->page_index);
    old_tail = NULL;
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

  radix_node_t *req_node = malloc(sizeof(radix_node_t));
  if (!get_node(tree, req_node, page_index))
    return false;

  // if page is in memory, move to front of LRU list
  vm_list_move_to_head(req_node->page);
  return true;
}

