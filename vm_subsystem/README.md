# Virtual Memory Subsystem

This is a prototype implementation of a virtual memory subsystem inspired by Linux's page cache. It uses:
- a least-recently-used (LRU) doubly-linked list to maintain eviction order of cached pages.
- a global hashmap to quickly look up inodes (files) that are currently cached in memory.
- per-inode radix trees to efficiently locate individual page indices within a file's cached contents.

## Workflow
 1. On file access, check the hashmap to see if the file's inode is already cached.
 2. If the file is cached, consult its radix tree to determine whether the requested page(s) are in memory.
 3. If found, serve them from memory and move the corresponding vm_page_t entries to the head of the LRU list.
 4. If missing, map the pages to the virtual memory address, insert them into the file's radix tree, and add them to the head of the LRU list.

## Eviction
- If the global LRU list reaches capacity (LRU_LIST_MAX_CAPACITY) evict the least-recently-used page(s) from the tail.
- The evicted pages are removed from its inode's radix tree.
- If the inode has no remaining cached pages, it may also be removed from the inode hashmap.
