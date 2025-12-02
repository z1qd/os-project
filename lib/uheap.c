#include <inc/lib.h>

//==================================================================================//
//============================== GIVEN FUNCTIONS ===================================//
//==================================================================================//

// Data Structures for User Heap Page Allocator
struct UserHeapBlock {
	uint32 start_va;
	uint32 num_pages;
	LIST_ENTRY(UserHeapBlock) prev_next_info;
};

LIST_HEAD(UserFreeBlockList, UserHeapBlock) uheap_free_blocks_list;
LIST_HEAD(UserUsedBlockList, UserHeapBlock) uheap_used_blocks_list;

//==============================================
// [1] INITIALIZE USER HEAP:
//==============================================
int __firstTimeFlag = 1;
void uheap_init()
{
	if(__firstTimeFlag)
	{
		// Initialize the dynamic allocator (Block Allocator)
		initialize_dynamic_allocator(USER_HEAP_START, USER_HEAP_START + DYN_ALLOC_MAX_SIZE);
		uheapPlaceStrategy = sys_get_uheap_strategy();

		// [FIX] Explicitly set Page Allocator Start using Constants.
		// Range: [USER_HEAP_START + DYN_ALLOC_MAX_SIZE + PAGE_SIZE, USER_HEAP_MAX)
		uheapPageAllocStart = USER_HEAP_START + DYN_ALLOC_MAX_SIZE + PAGE_SIZE;
		uheapPageAllocBreak = uheapPageAllocStart;

		// Initialize Lists
		LIST_INIT(&uheap_free_blocks_list);
		LIST_INIT(&uheap_used_blocks_list);

		__firstTimeFlag = 0;
	}
}

//==============================================
// [2] GET A PAGE FROM THE KERNEL FOR DA:
//==============================================
int get_page(void* va)
{
	int ret = __sys_allocate_page(ROUNDDOWN(va, PAGE_SIZE), PERM_USER|PERM_WRITEABLE|PERM_UHPAGE);
	if (ret < 0)
		panic("get_page() in user: failed to allocate page from the kernel");
	return 0;
}

//==============================================
// [3] RETURN A PAGE FROM THE DA TO KERNEL:
//==============================================
void return_page(void* va)
{
	int ret = __sys_unmap_frame(ROUNDDOWN((uint32)va, PAGE_SIZE));
	if (ret < 0)
		panic("return_page() in user: failed to return a page to the kernel");
}

//==================================================================================//
//============================ REQUIRED FUNCTIONS ==================================//
//==================================================================================//

//=================================
// [1] ALLOCATE SPACE IN USER HEAP:
//=================================
void* malloc(uint32 size)
{
	//==============================================================
	//DON'T CHANGE THIS CODE========================================
	uheap_init();
	if (size == 0) return NULL ;
	//==============================================================

	// [A] Block Allocator (Small Allocations <= DYN_ALLOC_MAX_BLOCK_SIZE)
	if (size <= DYN_ALLOC_MAX_BLOCK_SIZE)
	{
		return alloc_block(size);
	}

	// [B] Page Allocator (Large Allocations) -> CUSTOM FIT STRATEGY
	uint32 needed_pages = ROUNDUP(size, PAGE_SIZE) / PAGE_SIZE;
	uint32 alloc_va = 0;

	struct UserHeapBlock *exact_fit = NULL;
	struct UserHeapBlock *worst_fit = NULL;
	struct UserHeapBlock *block = NULL;

	// 1. Search Free List
	LIST_FOREACH(block, &uheap_free_blocks_list)
	{
		if (block->num_pages == needed_pages)
		{
			exact_fit = block;
			break; // Found exact, priority #1
		}
		if (block->num_pages > needed_pages)
		{
			// Worst fit: Find the block with the MAXIMUM size
			if (worst_fit == NULL || block->num_pages > worst_fit->num_pages)
				worst_fit = block;
		}
	}

	struct UserHeapBlock *target_block = (exact_fit != NULL) ? exact_fit : worst_fit;

	// 2. Allocation Logic
	if (target_block != NULL)
	{
		// Found in free list
		alloc_va = target_block->start_va;
		LIST_REMOVE(&uheap_free_blocks_list, target_block);

		// Split if worst fit (target is larger than needed)
		if (target_block->num_pages > needed_pages)
		{
			// Create a new block for the remaining free space using Block Allocator for metadata
			struct UserHeapBlock *new_free_block = (struct UserHeapBlock*)alloc_block(sizeof(struct UserHeapBlock));

			if (new_free_block != NULL)
			{
				new_free_block->start_va = alloc_va + (needed_pages * PAGE_SIZE);
				new_free_block->num_pages = target_block->num_pages - needed_pages;
				LIST_INSERT_HEAD(&uheap_free_blocks_list, new_free_block);
			}
			else
			{
				// If metadata allocation fails, revert the removal and fail
				LIST_INSERT_HEAD(&uheap_free_blocks_list, target_block);
				return NULL;
			}
		}

		// Update the used block metadata
		target_block->num_pages = needed_pages;
		LIST_INSERT_HEAD(&uheap_used_blocks_list, target_block);
	}
	else
	{
		// 3. Not found in free list, check if we can extend the break

		uint32 size_to_alloc = needed_pages * PAGE_SIZE;
		uint32 new_break = uheapPageAllocBreak + size_to_alloc;

		// [CRITICAL FIX] Overflow & Boundary Check
		// 1. Check for Overflow: If (Start + Size) wraps around, new_break will be smaller than Start.
		// 2. Check Boundary: If new_break reaches or exceeds USER_HEAP_MAX.
		if (new_break < uheapPageAllocBreak || new_break >= USER_HEAP_MAX)
			return NULL;

		alloc_va = uheapPageAllocBreak;
		uheapPageAllocBreak = new_break;

		// Create new metadata for used block
		struct UserHeapBlock *new_used_block = (struct UserHeapBlock*)alloc_block(sizeof(struct UserHeapBlock));
		if (new_used_block == NULL)
		{
			// Rollback break if metadata alloc fails
			uheapPageAllocBreak -= size_to_alloc;
			return NULL;
		}

		new_used_block->start_va = alloc_va;
		new_used_block->num_pages = needed_pages;
		LIST_INSERT_HEAD(&uheap_used_blocks_list, new_used_block);
	}

	// [C] Mark memory in Kernel
	sys_allocate_user_mem(alloc_va, size);

	return (void*)alloc_va;
}

//=================================
// [2] FREE SPACE FROM USER HEAP:
//=================================
void free(void* virtual_address)
{
	if (virtual_address == NULL) return;
	uint32 va = (uint32)virtual_address;

	// [A] Block Allocator Range
	if (va >= USER_HEAP_START && va < uheapPageAllocStart)
	{
		free_block(virtual_address);
		return;
	}

	// [B] Page Allocator Range
	if (va >= uheapPageAllocStart && va < USER_HEAP_MAX)
	{
		// 1. Find the block in Used List
		struct UserHeapBlock *block_to_free = NULL;
		LIST_FOREACH(block_to_free, &uheap_used_blocks_list)
		{
			if (block_to_free->start_va == va) break;
		}

		if (block_to_free == NULL) panic("free: invalid virtual address or double free!");

		// 2. Remove from Used List
		LIST_REMOVE(&uheap_used_blocks_list, block_to_free);

		// 3. Unmark in Kernel (Free pages from RAM and PageFile)
		sys_free_user_mem(va, block_to_free->num_pages * PAGE_SIZE);

		// 4. [PERFORMANCE OPTIMIZATION] One-Pass O(N) Merge Logic
		// Avoids nested loops which caused the 2-minute runtime.
		struct UserHeapBlock *prev_block = NULL;
		struct UserHeapBlock *next_block = NULL;
		struct UserHeapBlock *iterator = NULL;

		uint32 my_start = block_to_free->start_va;
		uint32 my_size = block_to_free->num_pages * PAGE_SIZE;
		uint32 my_end = my_start + my_size;

		LIST_FOREACH(iterator, &uheap_free_blocks_list)
		{
			// Check for Left Neighbor (ends at my start)
			if (iterator->start_va + iterator->num_pages * PAGE_SIZE == my_start)
			{
				prev_block = iterator;
			}
			// Check for Right Neighbor (starts at my end)
			if (iterator->start_va == my_end)
			{
				next_block = iterator;
			}
		}

		struct UserHeapBlock *final_block = NULL;

		if (prev_block != NULL && next_block != NULL)
		{
			// Merge ALL: Prev + My + Next -> Prev
			prev_block->num_pages += block_to_free->num_pages + next_block->num_pages;

			// Remove Next from list and free its metadata
			LIST_REMOVE(&uheap_free_blocks_list, next_block);
			free_block(next_block);

			// Free My metadata (it was never inserted into free list)
			free_block(block_to_free);

			final_block = prev_block;
		}
		else if (prev_block != NULL)
		{
			// Merge Prev + My -> Prev
			prev_block->num_pages += block_to_free->num_pages;

			// Free My metadata
			free_block(block_to_free);

			final_block = prev_block;
		}
		else if (next_block != NULL)
		{
			// Merge My + Next -> Next
			next_block->start_va = my_start;
			next_block->num_pages += block_to_free->num_pages;

			// Free My metadata
			free_block(block_to_free);

			final_block = next_block;
		}
		else
		{
			// No merging, simply insert
			LIST_INSERT_HEAD(&uheap_free_blocks_list, block_to_free);
			final_block = block_to_free;
		}

		// 5. Shrink Break Logic
		// If the final (merged) free block ends exactly at the Break, we can lower the Break
		if (final_block->start_va + final_block->num_pages * PAGE_SIZE == uheapPageAllocBreak)
		{
			uheapPageAllocBreak = final_block->start_va;
			LIST_REMOVE(&uheap_free_blocks_list, final_block);
			free_block(final_block);
		}
		return;
	}
	panic("free: invalid address outside user heap!");
}

//=================================
// [3] ALLOCATE SHARED VARIABLE:
//=================================
void* smalloc(char *sharedVarName, uint32 size, uint8 isWritable)
{
	//==============================================================
	//DON'T CHANGE THIS CODE========================================
	uheap_init();
	if (size == 0) return NULL ;
	//==============================================================

	panic("smalloc() is not implemented yet...!!");
	return NULL;
}

//========================================
// [4] SHARE ON ALLOCATED SHARED VARIABLE:
//========================================
void* sget(int32 ownerEnvID, char *sharedVarName)
{
	//==============================================================
	//DON'T CHANGE THIS CODE========================================
	uheap_init();
	//==============================================================

	panic("sget() is not implemented yet...!!");
	return NULL;
}


//==================================================================================//
//============================== BONUS FUNCTIONS ===================================//
//==================================================================================//


//=================================
// REALLOC USER SPACE:
//=================================
void *realloc(void *virtual_address, uint32 new_size)
{
	//==============================================================
	//DON'T CHANGE THIS CODE========================================
	uheap_init();
	//==============================================================
	panic("realloc() is not implemented yet...!!");
	return NULL;
}


//=================================
// FREE SHARED VARIABLE:
//=================================
void sfree(void* virtual_address)
{
	panic("sfree() is not implemented yet...!!");
}
