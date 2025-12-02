#include <inc/assert.h>
#include <inc/string.h>
#include "../inc/dynamic_allocator.h"

//==================================================================================//
//============================== GIVEN FUNCTIONS ===================================//
//==================================================================================//
//==================================
// [1] GET PAGE VA:
//==================================
__inline__ uint32 to_page_va(struct PageInfoElement *ptrPageInfo)
{
	//Get start VA of the page from the corresponding Page Info pointer
	int idxInPageInfoArr = (ptrPageInfo - pageBlockInfoArr);
	return dynAllocStart + (idxInPageInfoArr << PGSHIFT);
}

//==================================================================================//
//============================ REQUIRED FUNCTIONS ==================================//
//==================================================================================//

//==================================
// [1] INITIALIZE DYNAMIC ALLOCATOR:
//==================================
bool is_initialized = 0;
void initialize_dynamic_allocator(uint32 daStart, uint32 daEnd)
{
	//==================================================================================
	//DON'T CHANGE THESE LINES==========================================================
	//==================================================================================
	{
		assert(daEnd <= daStart + DYN_ALLOC_MAX_SIZE);
		is_initialized = 1;
	}
	//==================================================================================

	dynAllocStart = daStart;
	dynAllocEnd = daEnd;

	int i;
	for (i = 0; i <= (LOG2_MAX_SIZE - LOG2_MIN_SIZE); i++)
	{
		LIST_INIT(&freeBlockLists[i]);
	}

	LIST_INIT(&freePagesList);

	uint32 max_pages_in_array = DYN_ALLOC_MAX_SIZE / PAGE_SIZE;
	for (i = 0; i < max_pages_in_array; i++)
	{
		pageBlockInfoArr[i].block_size = 0;
		pageBlockInfoArr[i].num_of_free_blocks = 0;
	}

	uint32 num_active_pages = (daEnd - daStart) / PAGE_SIZE;
	for (i = 0; i < num_active_pages; i++)
	{
		LIST_INSERT_TAIL(&freePagesList, &pageBlockInfoArr[i]);
	}
}

//===========================
// [2] GET BLOCK SIZE:
//===========================
__inline__ uint32 get_block_size(void *va)
{
	uint32 page_va = ROUNDDOWN((uint32)va, PAGE_SIZE);
	int idxInPageInfoArr = (page_va - dynAllocStart) >> PGSHIFT;
	return pageBlockInfoArr[idxInPageInfoArr].block_size;
}

//===========================
// 3) ALLOCATE BLOCK:
//===========================
void *alloc_block(uint32 size)
{
	if (size == 0) return NULL;

	// 1. Find nearest power-of-2 size
	uint32 actual_block_size = DYN_ALLOC_MIN_BLOCK_SIZE;
	int list_index = 0;
	while (actual_block_size < size)
	{
		actual_block_size <<= 1;
		list_index++;
	}

	// CASE 1: Free block exists
	if (!LIST_EMPTY(&freeBlockLists[list_index]))
	{
		struct BlockElement* block_to_alloc = LIST_FIRST(&freeBlockLists[list_index]);
		LIST_REMOVE(&freeBlockLists[list_index], block_to_alloc);

		uint32 page_va = ROUNDDOWN((uint32)block_to_alloc, PAGE_SIZE);
		int page_index = (page_va - dynAllocStart) >> PGSHIFT;
		pageBlockInfoArr[page_index].num_of_free_blocks--;

		return (void*)block_to_alloc;
	}

	// CASE 2: Free page exists
	if (!LIST_EMPTY(&freePagesList))
	{
		struct PageInfoElement* new_page_info = LIST_FIRST(&freePagesList);
		LIST_REMOVE(&freePagesList, new_page_info);

		uint32 new_page_va = to_page_va(new_page_info);

		if (get_page((void*)new_page_va) != 0)
		{
			LIST_INSERT_HEAD(&freePagesList, new_page_info);
			return NULL;
		}

		int num_blocks_in_page = PAGE_SIZE / actual_block_size;

		// Add blocks 2..N to list
		uint32 current_block_va = new_page_va + actual_block_size;
		for (int i = 1; i < num_blocks_in_page; i++)
		{
			struct BlockElement* block_element = (struct BlockElement*)current_block_va;
			LIST_INSERT_TAIL(&freeBlockLists[list_index], block_element);
			current_block_va += actual_block_size;
		}

		new_page_info->block_size = actual_block_size;
		new_page_info->num_of_free_blocks = num_blocks_in_page - 1;

		return (void*)new_page_va;
	}

	// CASE 3: Try larger blocks
	int max_list_index = LOG2_MAX_SIZE - LOG2_MIN_SIZE;
	for (int next_list_index = list_index + 1; next_list_index <= max_list_index; next_list_index++)
	{
		if (!LIST_EMPTY(&freeBlockLists[next_list_index]))
		{
			struct BlockElement* larger_block = LIST_FIRST(&freeBlockLists[next_list_index]);
			LIST_REMOVE(&freeBlockLists[next_list_index], larger_block);

			uint32 page_va = ROUNDDOWN((uint32)larger_block, PAGE_SIZE);
			int page_index = (page_va - dynAllocStart) >> PGSHIFT;

			pageBlockInfoArr[page_index].num_of_free_blocks--;
			return (void*)larger_block;
		}
	}

	return NULL;
}

//===========================
// [4] FREE BLOCK:
//===========================
void free_block(void *va)
{
	if (va == NULL) return;
	// Verify address is within dynamic allocator range
	if ((uint32)va < dynAllocStart || (uint32)va >= dynAllocEnd) return;

	uint32 actual_block_size = get_block_size(va);
	if (actual_block_size == 0) return;

	uint32 temp_size = DYN_ALLOC_MIN_BLOCK_SIZE;
	int list_index = 0;
	while (temp_size < actual_block_size)
	{
		temp_size <<= 1;
		list_index++;
	}

	uint32 page_va = ROUNDDOWN((uint32)va, PAGE_SIZE);
	int page_index = (page_va - dynAllocStart) >> PGSHIFT;
	struct PageInfoElement* page_info = &pageBlockInfoArr[page_index];

	struct BlockElement* block_to_free = (struct BlockElement*)va;
	LIST_INSERT_HEAD(&freeBlockLists[list_index], block_to_free);

	page_info->num_of_free_blocks++;

	int num_blocks_in_page = PAGE_SIZE / actual_block_size;

	// [OPTIMIZATION] If page is full of free blocks, return it to OS
	if (page_info->num_of_free_blocks == num_blocks_in_page)
	{
		// Efficiently remove ALL blocks belonging to this page from the list
		// WITHOUT creating a temporary list.
		struct BlockElement *curr = LIST_FIRST(&freeBlockLists[list_index]);
		struct BlockElement *next_node;
		int blocks_removed = 0;

		// We need to find exactly 'num_blocks_in_page' blocks.
		while (curr != NULL && blocks_removed < num_blocks_in_page)
		{
			next_node = LIST_NEXT(curr); // Save next pointer

			// Check if this block belongs to the page we are freeing
			if (ROUNDDOWN((uint32)curr, PAGE_SIZE) == page_va)
			{
				LIST_REMOVE(&freeBlockLists[list_index], curr);
				blocks_removed++;
			}
			curr = next_node;
		}

		return_page((void*)page_va);

		page_info->block_size = 0;
		page_info->num_of_free_blocks = 0;
		LIST_INSERT_HEAD(&freePagesList, page_info);
	}
}

//==================================================================================//
//============================== BONUS FUNCTIONS ===================================//
//==================================================================================//

//===========================
// [1] REALLOCATE BLOCK:
//===========================
void *realloc_block(void* va, uint32 new_size)
{
	if (va == NULL) return alloc_block(new_size);
	if (new_size == 0)
	{
		free_block(va);
		return NULL;
	}

	uint32 old_size = get_block_size(va);
	if (new_size <= old_size) return va;

	void* new_block = alloc_block(new_size);
	if (new_block == NULL) return NULL;

	memcpy(new_block, va, old_size);
	free_block(va);

	return new_block;
}
