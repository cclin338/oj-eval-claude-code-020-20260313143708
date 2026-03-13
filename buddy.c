#include "buddy.h"
#define NULL ((void *)0)

#define MAXRANK 16
#define PAGE_SIZE (4 * 1024)  // 4KB

// Node structure for free list
typedef struct free_node {
    struct free_node *next;
} free_node_t;

// Global state
static void *base_addr = NULL;
static int total_pages = 0;
static free_node_t *free_lists[MAXRANK + 1];  // free_lists[1] to free_lists[16]
static char *allocated_map = NULL;  // Track which pages are allocated
static char *rank_map = NULL;  // Track the rank of each page (for allocated blocks)
static char *free_rank_map = NULL;  // Track the rank of free blocks

// Get the page index from address
static int get_page_index(void *p) {
    if (p < base_addr || p >= base_addr + total_pages * PAGE_SIZE) {
        return -1;
    }
    long offset = (char *)p - (char *)base_addr;
    if (offset % PAGE_SIZE != 0) {
        return -1;
    }
    return offset / PAGE_SIZE;
}

// Get address from page index
static void *get_page_addr(int index) {
    return base_addr + index * PAGE_SIZE;
}

// Get buddy index
static int get_buddy_index(int index, int rank) {
    int block_size = 1 << (rank - 1);  // 2^(rank-1) pages
    return index ^ block_size;
}

// Mark block as allocated
static void mark_allocated(int index, int rank, int value) {
    int block_size = 1 << (rank - 1);
    for (int i = 0; i < block_size; i++) {
        allocated_map[index + i] = value;
        if (value) {
            rank_map[index + i] = rank;
        } else {
            rank_map[index + i] = 0;
        }
    }
}

// Set free block rank marker
static void set_free_rank(int index, int rank) {
    free_rank_map[index] = rank;
}

// Clear free block rank marker
static void clear_free_rank(int index) {
    free_rank_map[index] = 0;
}

// Get free block rank
static int get_free_rank(int index) {
    return free_rank_map[index];
}

// Remove a block from free list
static void remove_from_free_list(int index, int rank) {
    void *addr = get_page_addr(index);
    free_node_t **head = &free_lists[rank];
    free_node_t *prev = NULL;
    free_node_t *curr = *head;

    while (curr != NULL) {
        if ((void *)curr == addr) {
            if (prev == NULL) {
                *head = curr->next;
            } else {
                prev->next = curr->next;
            }
            clear_free_rank(index);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

// Add block to free list (keeping sorted by address for deterministic allocation)
static void add_to_free_list(int index, int rank) {
    void *addr = get_page_addr(index);
    free_node_t *node = (free_node_t *)addr;

    // Insert in sorted order by address
    free_node_t **head = &free_lists[rank];
    free_node_t *prev = NULL;
    free_node_t *curr = *head;

    while (curr != NULL && curr < node) {
        prev = curr;
        curr = curr->next;
    }

    if (prev == NULL) {
        node->next = *head;
        *head = node;
    } else {
        node->next = prev->next;
        prev->next = node;
    }

    set_free_rank(index, rank);
}

int init_page(void *p, int pgcount) {
    base_addr = p;
    total_pages = pgcount;

    // Initialize free lists
    for (int i = 0; i <= MAXRANK; i++) {
        free_lists[i] = NULL;
    }

    // Allocate tracking arrays (using the managed pages themselves for storage)
    // We'll use a simple static approach
    static char alloc_map[128 * 1024 / 4];  // Max pages
    static char r_map[128 * 1024 / 4];
    static char free_r_map[128 * 1024 / 4];
    allocated_map = alloc_map;
    rank_map = r_map;
    free_rank_map = free_r_map;

    for (int i = 0; i < total_pages; i++) {
        allocated_map[i] = 0;
        rank_map[i] = 0;
        free_rank_map[i] = 0;
    }

    // Add initial blocks to free lists
    // We need to build the largest possible blocks
    int remaining = pgcount;
    int current_index = 0;

    for (int rank = MAXRANK; rank >= 1 && remaining > 0; rank--) {
        int block_size = 1 << (rank - 1);  // 2^(rank-1) pages
        while (remaining >= block_size) {
            add_to_free_list(current_index, rank);
            current_index += block_size;
            remaining -= block_size;
        }
    }

    return OK;
}

void *alloc_pages(int rank) {
    // Check if rank is valid
    if (rank < 1 || rank > MAXRANK) {
        return ERR_PTR(-EINVAL);
    }

    // Find a free block of the requested rank or larger
    int found_rank = -1;
    for (int r = rank; r <= MAXRANK; r++) {
        if (free_lists[r] != NULL) {
            found_rank = r;
            break;
        }
    }

    if (found_rank == -1) {
        return ERR_PTR(-ENOSPC);
    }

    // Split blocks until we get to the desired rank
    while (found_rank > rank) {
        // Remove from current list
        free_node_t *node = free_lists[found_rank];
        free_lists[found_rank] = node->next;

        int index = get_page_index((void *)node);
        clear_free_rank(index);

        int buddy_index = index + (1 << (found_rank - 2));

        found_rank--;

        // Add both buddies to the smaller rank list
        add_to_free_list(index, found_rank);
        add_to_free_list(buddy_index, found_rank);
    }

    // Allocate the block
    free_node_t *node = free_lists[rank];
    free_lists[rank] = node->next;

    int index = get_page_index((void *)node);
    clear_free_rank(index);
    mark_allocated(index, rank, 1);

    return (void *)node;
}

int return_pages(void *p) {
    // Check if address is valid
    int index = get_page_index(p);
    if (index < 0) {
        return -EINVAL;
    }

    // Check if this page is allocated
    if (!allocated_map[index]) {
        return -EINVAL;
    }

    int rank = rank_map[index];

    // Mark as free
    mark_allocated(index, rank, 0);

    // Coalesce with buddy if possible
    while (rank < MAXRANK) {
        int buddy_index = get_buddy_index(index, rank);
        int block_size = 1 << (rank - 1);

        // Check if buddy is in valid range
        if (buddy_index < 0 || buddy_index + block_size > total_pages) {
            break;
        }

        // Check if buddy is free and has the same rank using the free_rank_map
        if (get_free_rank(buddy_index) != rank) {
            break;
        }

        // Remove buddy from free list
        remove_from_free_list(buddy_index, rank);

        // Merge: the merged block starts at the lower index
        if (buddy_index < index) {
            index = buddy_index;
        }

        rank++;
    }

    // Add the (possibly merged) block to free list
    add_to_free_list(index, rank);

    return OK;
}

int query_ranks(void *p) {
    int index = get_page_index(p);
    if (index < 0) {
        return -EINVAL;
    }

    // If allocated, return its rank
    if (allocated_map[index]) {
        return rank_map[index];
    }

    // If not allocated, find the maximum rank of free block containing this page
    for (int rank = MAXRANK; rank >= 1; rank--) {
        free_node_t *curr = free_lists[rank];
        while (curr != NULL) {
            int block_index = get_page_index((void *)curr);
            int block_size = 1 << (rank - 1);
            if (index >= block_index && index < block_index + block_size) {
                return rank;
            }
            curr = curr->next;
        }
    }

    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAXRANK) {
        return -EINVAL;
    }

    int count = 0;
    free_node_t *curr = free_lists[rank];
    while (curr != NULL) {
        count++;
        curr = curr->next;
    }

    return count;
}
