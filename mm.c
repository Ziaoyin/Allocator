/*
 * NOTES:
 *  - Explicit allocator with an explicit free-list
 *  - Free-list uses a single, doubly-linked list with LIFO insertion policy,
 *    first-fit search strategy, and immediate coalescing.
 *  - We use "next" and "previous" to refer to blocks as ordered in the free-list.
 *  - We use "following" and "preceding" to refer to adjacent blocks in memory.
 *  - Pointers in the free-list will point to the beginning of a heap block
 *    (i.e., to the header).
 *  - Pointers returned by mm_malloc point to the beginning of the payload
 *    (i.e., to the word after the header).
 *
 * ALLOCATOR BLOCKS:
 *  - See definition of block_info struct fields further down
 *  - USED: +---------------+   FREE: +---------------+
 *          |    header     |         |    header     |
 *          |(size_and_tags)|         |(size_and_tags)|
 *          +---------------+         +---------------+
 *          |  payload and  |         |   next ptr    |
 *          |    padding    |         +---------------+
 *          |       .       |         |   prev ptr    |
 *          |       .       |         +---------------+
 *          |       .       |         |  free space   |
 *          |               |         |  and padding  |
 *          |               |         |      ...      |
 *          |               |         +---------------+
 *          |               |         |    footer     |
 *          |               |         |(size_and_tags)|
 *          +---------------+         +---------------+
 *
 * BOUNDARY TAGS:
 *  - Headers and footers for a heap block store identical information.
 *  - The block size is stored as a word, but because of alignment, we can use
 *    some number of the least significant bits as tags/flags.
 *  - TAG_USED is bit 0 (the 1's digit) and indicates if this heap block is
 *    used/allocated.
 *  - TAG_PRECEDING_USED is bit 1 (the 2's digit) and indicates if the
 *    preceding heap block is used/allocated. Used for coalescing and avoids
 *    the need for a footer in used/allocated blocks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

#include "memlib.h"
#include "mm.h"


// Static functions for unscaled pointer arithmetic to keep other code cleaner.
//  - The first argument is void* to enable you to pass in any type of pointer
//  - Casting to char* changes the pointer arithmetic scaling to 1 byte
//    (e.g., UNSCALED_POINTER_ADD(0x1, 1) returns 0x2)
//  - We cast the result to void* to force you to cast back to the appropriate
//    type and ensure you don't accidentally use the resulting pointer as a
//    char* implicitly.
static inline void* UNSCALED_POINTER_ADD(void* p, int x) { return ((void*)((char*)(p) + (x))); }
static inline void* UNSCALED_POINTER_SUB(void* p, int x) { return ((void*)((char*)(p) - (x))); }


// A block_info can be used to access information about a heap block,
// including boundary tag info (size and usage tags in header and footer)
// and pointers to the next and previous blocks in the free-list.
struct block_info {
    // Size of the block and tags (preceding-used? and used? flags) combined
	// together. See the SIZE() function and TAG macros below for more details
	// and how to extract these pieces of info.
    size_t size_and_tags;
    // Pointer to the next block in the free list.
    struct block_info* next;
    // Pointer to the previous block in the free list.
    struct block_info* prev;
};
typedef struct block_info block_info;


// Pointer to the first block_info in the free list, the list's head.
// In this implementation, this is stored in the first word in the heap and
// accessed via mem_heap_lo().
#define FREE_LIST_HEAD *((block_info **)mem_heap_lo())

// Size of a word on this architecture.
#define WORD_SIZE sizeof(void*)

// Minimum block size (accounts for header, next ptr, prev ptr, and footer).
#define MIN_BLOCK_SIZE (sizeof(block_info) + WORD_SIZE)

// Alignment requirement for allocator.
#define ALIGNMENT 8

// SIZE(block_info->size_and_tags) extracts the size of a 'size_and_tags' field.
// SIZE(size) returns a properly-aligned value of 'size' (by rounding down).
static inline size_t SIZE(size_t x) { return ((x) & ~(ALIGNMENT - 1)); }

// Bit mask to use to extract or set TAG_USED in a boundary tag.
#define TAG_USED 1

// Bit mask to use to extract or set TAG_PRECEDING_USED in a boundary tag.
#define TAG_PRECEDING_USED 2


/*
 * Print the heap by iterating through it as an implicit free list.
 *  - For debugging; make sure to remove calls before submission as will affect
 *    throughput.
 *  - Can ignore compiler warning about this function being unused.
 */
static void examine_heap() {
  block_info* block;

  // print to stderr so output isn't buffered and not output if we crash
  fprintf(stderr, "FREE_LIST_HEAD: %p\n", (void*) FREE_LIST_HEAD);

  for (block = (block_info*) UNSCALED_POINTER_ADD(mem_heap_lo(), WORD_SIZE);  // first block on heap
       SIZE(block->size_and_tags) != 0 && block < (block_info*) mem_heap_hi();
       block = (block_info*) UNSCALED_POINTER_ADD(block, SIZE(block->size_and_tags))) {

    // print out common block attributes
    fprintf(stderr, "%p: %ld %ld %ld\t",
            (void*) block,
            SIZE(block->size_and_tags),
            block->size_and_tags & TAG_PRECEDING_USED,
            block->size_and_tags & TAG_USED);

    // and allocated/free specific data
    if (block->size_and_tags & TAG_USED) {
      fprintf(stderr, "ALLOCATED\n");
    } else {
      fprintf(stderr, "FREE\tnext: %p, prev: %p\n",
              (void*) block->next,
              (void*) block->prev);
    }
  }
  fprintf(stderr, "END OF HEAP\n\n");
}


/*
 * Find a free block of the requested size in the free list.
 * Returns NULL if no free block is large enough.
 */
static block_info* search_free_list(size_t req_size) {
  block_info* free_block;

  free_block = FREE_LIST_HEAD;
  while (free_block != NULL) {
    if (SIZE(free_block->size_and_tags) >= req_size) {
      return free_block;
    } else {
      free_block = free_block->next;
    }
  }
  return NULL;
}


/* Insert free_block at the head of the list (LIFO). */
static void insert_free_block(block_info* free_block) {
  block_info* old_head = FREE_LIST_HEAD;
  free_block->next = old_head;
  if (old_head != NULL) {
    old_head->prev = free_block;
  }
  free_block->prev = NULL;
  FREE_LIST_HEAD = free_block;
}


/* Remove a free block from the free list. */
static void remove_free_block(block_info* free_block) {
  block_info* next_free;
  block_info* prev_free;

  next_free = free_block->next;
  prev_free = free_block->prev;

  // If the next block is not null, patch its prev pointer.
  if (next_free != NULL) {
    next_free->prev = prev_free;
  }

  // If we're removing the head of the free list, set the head to be
  // the next block, otherwise patch the previous block's next pointer.
  if (free_block == FREE_LIST_HEAD) {
    FREE_LIST_HEAD = next_free;
  } else {
    prev_free->next = next_free;
  }
}


/* Coalesce 'old_block' with any preceding or following free blocks. */
static void coalesce_free_block(block_info* old_block) {
  block_info* block_cursor;
  block_info* new_block;
  block_info* free_block;
  // size of old block
  size_t old_size = SIZE(old_block->size_and_tags);
  // running sum to be size of final coalesced block
  size_t new_size = old_size;

  // Coalesce with any preceding free block
  block_cursor = old_block;
  while ((block_cursor->size_and_tags & TAG_PRECEDING_USED) == 0) {
    // While the block preceding this one in memory (not the
    // prev. block in the free list) is free:

    // Get the size of the previous block from its boundary tag.
    size_t size = SIZE(*((size_t*) UNSCALED_POINTER_SUB(block_cursor, WORD_SIZE)));
    // Use this size to find the block info for that block.
    free_block = (block_info*) UNSCALED_POINTER_SUB(block_cursor, size);
    // Remove that block from free list.
    remove_free_block(free_block);

    // Count that block's size and update the current block pointer.
    new_size += size;
    block_cursor = free_block;
  }
  new_block = block_cursor;

  // Coalesce with any following free block.
  // Start with the block following this one in memory
  block_cursor = (block_info*) UNSCALED_POINTER_ADD(old_block, old_size);
  while ((block_cursor->size_and_tags & TAG_USED) == 0) {
    // While following block is free:

    size_t size = SIZE(block_cursor->size_and_tags);
    // Remove it from the free list.
    remove_free_block(block_cursor);
    // Count its size and step to the following block.
    new_size += size;
    block_cursor = (block_info*) UNSCALED_POINTER_ADD(block_cursor, size);
  }

  // If the block actually grew, remove the old entry from the free-list
  // and add the new entry.
  if (new_size != old_size) {
    // Remove the original block from the free list
    remove_free_block(old_block);

    // Save the new size in the block info and in the boundary tag
    // and tag it to show the preceding block is used (otherwise, it
    // would have become part of this one!).
    new_block->size_and_tags = new_size | TAG_PRECEDING_USED;
    // The boundary tag of the preceding block is the word immediately
    // preceding block in memory where we left off advancing block_cursor.
    *(size_t*) UNSCALED_POINTER_SUB(block_cursor, WORD_SIZE) = new_size | TAG_PRECEDING_USED;

    // Put the new block in the free list.
    insert_free_block(new_block);
  }
  return;
}


/* Get more heap space of size at least req_size. */
static void request_more_space(size_t req_size) {
  size_t pagesize = mem_pagesize();
  size_t num_pages = (req_size + pagesize - 1) / pagesize;
  block_info* new_block;
  size_t total_size = num_pages * pagesize;
  size_t prev_last_word_mask;

  void* mem_sbrk_result = mem_sbrk(total_size);
  if ((size_t) mem_sbrk_result == -1) {
    printf("ERROR: mem_sbrk failed in request_more_space\n");
    exit(0);
  }
  new_block = (block_info*) UNSCALED_POINTER_SUB(mem_sbrk_result, WORD_SIZE);

  // Initialize header by inheriting TAG_PRECEDING_USED status from the
  // end-of-heap word and resetting the TAG_USED bit.
  prev_last_word_mask = new_block->size_and_tags & TAG_PRECEDING_USED;
  new_block->size_and_tags = total_size | prev_last_word_mask;
  // Initialize new footer
  ((block_info*) UNSCALED_POINTER_ADD(new_block, total_size - WORD_SIZE))->size_and_tags =
          total_size | prev_last_word_mask;

  // Initialize new end-of-heap word: SIZE is 0, TAG_PRECEDING_USED is 0,
  // TAG_USED is 1. This trick lets us do the "normal" check even at the end
  // of the heap.
  *((size_t*) UNSCALED_POINTER_ADD(new_block, total_size)) = TAG_USED;

  // Add the new block to the free list and immediately coalesce newly
  // allocated memory space.
  insert_free_block(new_block);
  coalesce_free_block(new_block);
}


/* Initialize the allocator. */
int mm_init() {
  // Head of the free list.
  block_info* first_free_block;

  // Initial heap size: WORD_SIZE byte heap-header (stores pointer to head
  // of free list), MIN_BLOCK_SIZE bytes of space, WORD_SIZE byte heap-footer.
  size_t init_size = WORD_SIZE + MIN_BLOCK_SIZE + WORD_SIZE;
  size_t total_size;

  void* mem_sbrk_result = mem_sbrk(init_size);
  //  printf("mem_sbrk returned %p\n", mem_sbrk_result);
  if ((ssize_t) mem_sbrk_result == -1) {
    printf("ERROR: mem_sbrk failed in mm_init, returning %p\n",
           mem_sbrk_result);
    exit(1);
  }

  first_free_block = (block_info*) UNSCALED_POINTER_ADD(mem_heap_lo(), WORD_SIZE);

  // Total usable size is full size minus heap-header and heap-footer words.
  // NOTE: These are different than the "header" and "footer" of a block!
  //  - The heap-header is a pointer to the first free block in the free list.
  //  - The heap-footer is the end-of-heap indicator (used block with size 0).
  total_size = init_size - WORD_SIZE - WORD_SIZE;

  // The heap starts with one free block, which we initialize now.
  first_free_block->size_and_tags = total_size | TAG_PRECEDING_USED;
  first_free_block->next = NULL;
  first_free_block->prev = NULL;
  // Set the free block's footer.
  *((size_t*) UNSCALED_POINTER_ADD(first_free_block, total_size - WORD_SIZE)) =
	  total_size | TAG_PRECEDING_USED;

  // Tag the end-of-heap word at the end of heap as used.
  *((size_t*) UNSCALED_POINTER_SUB(mem_heap_hi(), WORD_SIZE - 1)) = TAG_USED;

  // Set the head of the free list to this new free block.
  FREE_LIST_HEAD = first_free_block;
  return 0;
}


// TOP-LEVEL ALLOCATOR INTERFACE ------------------------------------

/*
 * Allocate a block of size size and return a pointer to it. If size is zero,
 * returns NULL.
 */
void* mm_malloc(size_t size) {
  size_t req_size;
  block_info* ptr_free_block = NULL;
  size_t block_size;
  size_t preceding_block_use_tag;

  // Zero-size requests get NULL.
  if (size == 0) {
    return NULL;
  }
  

  // Figure out how big a block you need (factor in alignment, minimum block size, etc)
  // Add one word for the initial size header.
  // Note that we don't need a footer when the block is used/allocated!
  size += WORD_SIZE;
  if (size <= MIN_BLOCK_SIZE) {
    // Make sure we allocate enough space for the minimum block size.
    req_size = MIN_BLOCK_SIZE;
  } else {
    // Round up for proper alignment.
    req_size = ALIGNMENT * ((size + ALIGNMENT - 1) / ALIGNMENT);
  }

  // Call search_free_list() to get a free block that is large enough
  ptr_free_block = search_free_list(req_size);
  // If space runs out in heap
  if (ptr_free_block == NULL) {
    request_more_space(req_size);
    ptr_free_block = search_free_list(req_size);
  }
  
  // Remove block from the free list
  remove_free_block(ptr_free_block);

  // Set block attributes
  block_size = SIZE(ptr_free_block->size_and_tags);
  size_t extra_space = block_size - req_size;
  preceding_block_use_tag = (ptr_free_block->size_and_tags) & TAG_PRECEDING_USED;
  size_t whole_tag = block_size | TAG_USED | preceding_block_use_tag;

  // May need to split this block
  if (extra_space >= MIN_BLOCK_SIZE) { // enough left over to make into a new free block
    // new extra free block due to split
    block_info* extra = (block_info*)UNSCALED_POINTER_ADD(ptr_free_block, req_size);
     
    // need to update size_and_tags for extra
    size_t new_size_and_tags = extra_space | TAG_PRECEDING_USED; // always has pre used as 1 and used as 0
    extra->size_and_tags = new_size_and_tags; // set the extra block size_and_tags
    *((size_t*)UNSCALED_POINTER_ADD(extra, extra_space-WORD_SIZE)) = extra->size_and_tags;
    insert_free_block(extra);

    // old free block that now is allocated
    whole_tag = req_size | TAG_USED | preceding_block_use_tag;
  } else { // allocate entire free block
    block_info* following = (block_info*)UNSCALED_POINTER_ADD(ptr_free_block, block_size);
    following->size_and_tags = following->size_and_tags | TAG_PRECEDING_USED;

    // update footer of next (if its a free block)
    if (!(following->size_and_tags & TAG_USED)) {
      size_t following_payload_index = SIZE(following->size_and_tags)-WORD_SIZE;
      *((size_t*)UNSCALED_POINTER_ADD(following,following_payload_index)) = following->size_and_tags;
    }
  }

  ptr_free_block->size_and_tags = whole_tag; // set allocated tag
  return UNSCALED_POINTER_ADD(ptr_free_block, WORD_SIZE); // payload size
}


/* Free the block referenced by ptr. */
void mm_free(void* ptr) {
  size_t payload_size;
  block_info* block_to_free;
  block_info* following_block;

  block_to_free = (block_info*)UNSCALED_POINTER_SUB(ptr, WORD_SIZE); // gets the start of block
  if (!(block_to_free->size_and_tags)&TAG_USED) { //if already free
    return;
  }

  // get block info
  payload_size = SIZE(block_to_free->size_and_tags) - WORD_SIZE;
  following_block = (block_info*)UNSCALED_POINTER_ADD(block_to_free, SIZE(block_to_free->size_and_tags));
  
  // Convert the given used block into a free block
  block_to_free->size_and_tags = block_to_free->size_and_tags & (~TAG_USED);
  *((size_t*)UNSCALED_POINTER_ADD(block_to_free, payload_size)) = block_to_free->size_and_tags;

  // Update size_and_tags for following
  following_block->size_and_tags = following_block->size_and_tags & (~TAG_PRECEDING_USED);

  if (!(following_block->size_and_tags & TAG_USED)) { // if next is free, also update footer
    size_t following_payload_size = SIZE(following_block->size_and_tags) - WORD_SIZE;
    *((size_t*)UNSCALED_POINTER_ADD(following_block, following_payload_size)) = following_block->size_and_tags;
  }
  
  // insert and coalesce
  insert_free_block(block_to_free);
  coalesce_free_block(block_to_free);
  
}
