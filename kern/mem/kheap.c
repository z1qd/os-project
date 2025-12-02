#include "kheap.h"

#include <inc/memlayout.h>
#include <inc/dynamic_allocator.h>
#include <kern/conc/sleeplock.h>
#include <kern/proc/user_environment.h>
#include <kern/mem/memory_manager.h>
#include "../conc/kspinlock.h"

#include "inc/queue.h"

uint32 kheapPageAllocStart;
uint32 kheapPageAllocBreak;
uint32 kheapPlacementStrategy;

struct PageAllocBlock {
    uint32 start_va;
    uint32 num_pages;
    LIST_ENTRY(PageAllocBlock) prev_next_info;
};

LIST_HEAD(FreeBlockList, PageAllocBlock) free_blocks_list;
LIST_HEAD(UsedBlockList, PageAllocBlock) used_blocks_list;

void kheap_init()
{
	{
		initialize_dynamic_allocator(KERNEL_HEAP_START, KERNEL_HEAP_START + DYN_ALLOC_MAX_SIZE);
		set_kheap_strategy(KHP_PLACE_CUSTOMFIT);
		kheapPageAllocStart = dynAllocEnd + PAGE_SIZE;
		kheapPageAllocBreak = kheapPageAllocStart;
	}

	LIST_INIT(&free_blocks_list);
	LIST_INIT(&used_blocks_list);
}

int get_page(void* va)
{
	int ret = alloc_page(ptr_page_directory, ROUNDDOWN((uint32)va, PAGE_SIZE), PERM_WRITEABLE, 1);
	if (ret < 0)
		panic("get_page() in kern: failed to allocate page from the kernel");
	return 0;
}

void return_page(void* va)
{
	unmap_frame(ptr_page_directory, ROUNDDOWN((uint32)va, PAGE_SIZE));
}

void* kmalloc(unsigned int size)
{
	if (size == 0) return NULL;

	if(size <= DYN_ALLOC_MAX_BLOCK_SIZE){
		return alloc_block(size);
	}

	uint32 needed_pages = ROUNDUP(size, PAGE_SIZE) / PAGE_SIZE;

	struct PageAllocBlock *exact_fit = NULL;
	struct PageAllocBlock *worst_fit = NULL;
	struct PageAllocBlock *block;

	// Custom Fit Strategy
	LIST_FOREACH(block, &free_blocks_list) {
		if (block->num_pages == needed_pages) {
			exact_fit = block;
			break;
		}
		if (block->num_pages > needed_pages) {
			if (worst_fit == NULL || block->num_pages > worst_fit->num_pages) {
				worst_fit = block;
			}
		}
	}

	struct PageAllocBlock *block_to_use = (exact_fit != NULL) ? exact_fit : worst_fit;
	uint32 alloc_va;

	if (block_to_use != NULL) {
		alloc_va = block_to_use->start_va;
		LIST_REMOVE(&free_blocks_list, block_to_use);

		if (block_to_use->num_pages > needed_pages) {
			struct PageAllocBlock *new_free_block = (struct PageAllocBlock*)alloc_block(sizeof(struct PageAllocBlock));
			if (new_free_block == NULL) return NULL;

			new_free_block->start_va = alloc_va + needed_pages * PAGE_SIZE;
			new_free_block->num_pages = block_to_use->num_pages - needed_pages;
			LIST_INSERT_HEAD(&free_blocks_list, new_free_block);
		}

		block_to_use->start_va = alloc_va;
		block_to_use->num_pages = needed_pages;
		LIST_INSERT_HEAD(&used_blocks_list, block_to_use);

		for (int i = 0; i < needed_pages; i++) {
			int ret = get_page((void*)(alloc_va + i * PAGE_SIZE));
			if (ret < 0) return NULL;
		}

		return (void*)alloc_va;

	} else {
		if ((uint64)kheapPageAllocBreak + needed_pages * PAGE_SIZE > KERNEL_HEAP_MAX) {
			return NULL;
		}

		alloc_va = kheapPageAllocBreak;
		kheapPageAllocBreak += needed_pages * PAGE_SIZE;

		for (int i = 0; i < needed_pages; i++) {
			int ret = alloc_page(ptr_page_directory, alloc_va + i * PAGE_SIZE, PERM_WRITEABLE, 1);
			if (ret < 0) return NULL;
		}

		struct PageAllocBlock *new_used_block = (struct PageAllocBlock*)alloc_block(sizeof(struct PageAllocBlock));
		if (new_used_block == NULL) return NULL;

		new_used_block->start_va = alloc_va;
		new_used_block->num_pages = needed_pages;
		LIST_INSERT_HEAD(&used_blocks_list, new_used_block);

		return (void*)alloc_va;
	}
}

void kfree(void* virtual_address)
{
	if (virtual_address == NULL) return;
	uint32 va = (uint32)virtual_address;

	if (va >= KERNEL_HEAP_START && va < kheapPageAllocStart) {
		free_block(virtual_address);
		return;
	}

	if (va >= kheapPageAllocStart && va < KERNEL_HEAP_MAX) {
		struct PageAllocBlock *block_to_free = NULL;

		LIST_FOREACH(block_to_free, &used_blocks_list) {
			if (block_to_free->start_va == va) break;
		}

		if (block_to_free == NULL) panic("kfree: invalid virtual address!");

		for (int i = 0; i < block_to_free->num_pages; i++) {
			unmap_frame(ptr_page_directory, va + i * PAGE_SIZE);
		}

		LIST_REMOVE(&used_blocks_list, block_to_free);

		// [FAST PAGE ALLOCATOR] One-Pass O(N) Merging
		struct PageAllocBlock *prev_free = NULL;
		struct PageAllocBlock *next_free = NULL;
		struct PageAllocBlock *iter_block;

		uint32 free_end = va + block_to_free->num_pages * PAGE_SIZE;

		LIST_FOREACH(iter_block, &free_blocks_list) {
			if (iter_block->start_va + iter_block->num_pages * PAGE_SIZE == va) {
				prev_free = iter_block;
			} else if (free_end == iter_block->start_va) {
				next_free = iter_block;
			}
		}

		if (prev_free != NULL) {
			prev_free->num_pages += block_to_free->num_pages;
			free_block(block_to_free);
			block_to_free = prev_free;

			if (next_free != NULL) {
				block_to_free->num_pages += next_free->num_pages;
				LIST_REMOVE(&free_blocks_list, next_free);
				free_block(next_free);
			}
		} else if (next_free != NULL) {
			next_free->num_pages += block_to_free->num_pages;
			next_free->start_va = va;
			free_block(block_to_free);
			block_to_free = next_free;
		} else {
			LIST_INSERT_HEAD(&free_blocks_list, block_to_free);
		}

		if (block_to_free->start_va + block_to_free->num_pages * PAGE_SIZE == kheapPageAllocBreak) {
			kheapPageAllocBreak = block_to_free->start_va;
			LIST_REMOVE(&free_blocks_list, block_to_free);
			free_block(block_to_free);
		}
		return;
	}
	panic("kfree: virtual address out of kernel heap range!");
}

unsigned int kheap_virtual_address(unsigned int physical_address)
{
	uint32 offset = physical_address & 0xFFF;
	uint32 frame_pa = physical_address & 0xFFFFF000;

	for (uint32 va = KERNEL_HEAP_START; va < KERNEL_HEAP_MAX; va += PAGE_SIZE)
	{
		uint32* ptr_page_table = NULL;
		get_page_table(ptr_page_directory, va, &ptr_page_table);

		if (ptr_page_table != NULL && (ptr_page_table[PTX(va)] & PERM_PRESENT))
		{
			uint32 entry_pa = ptr_page_table[PTX(va)] & 0xFFFFF000;
			if (entry_pa == frame_pa) return va + offset;
		}
	}
	return 0;
}

unsigned int kheap_physical_address(unsigned int virtual_address)
{
	uint32* ptr_page_table = NULL;
	get_page_table(ptr_page_directory, virtual_address, &ptr_page_table);

	if (ptr_page_table != NULL && (ptr_page_table[PTX(virtual_address)] & PERM_PRESENT))
	{
		uint32 frame_pa = ptr_page_table[PTX(virtual_address)] & 0xFFFFF000;
		uint32 offset = virtual_address & 0xFFF;
		return frame_pa + offset;
	}
	return 0;
}

extern __inline__ uint32 get_block_size(void *va);

void *krealloc(void *virtual_address, uint32 new_size)
{
	if (virtual_address == NULL) return kmalloc(new_size);
	if (new_size == 0) {
		kfree(virtual_address);
		return NULL;
	}

	uint32 va = (uint32)virtual_address;
	uint32 old_size;

	if (va >= KERNEL_HEAP_START && va < kheapPageAllocStart) {
		old_size = get_block_size(virtual_address);
		if (new_size <= old_size) return virtual_address;

		void* new_va = kmalloc(new_size);
		if (new_va == NULL) return NULL;
		memcpy(new_va, virtual_address, old_size);
		kfree(virtual_address);
		return new_va;
	}
	else if (va >= kheapPageAllocStart && va < KERNEL_HEAP_MAX) {
		struct PageAllocBlock *block = NULL;
		LIST_FOREACH(block, &used_blocks_list) {
			if (block->start_va == va) break;
		}
		if (block == NULL) panic("krealloc: invalid page address!");

		old_size = block->num_pages * PAGE_SIZE;
		uint32 needed_pages = ROUNDUP(new_size, PAGE_SIZE) / PAGE_SIZE;

		if (needed_pages <= block->num_pages) return virtual_address;

		void* new_va = kmalloc(new_size);
		if (new_va == NULL) return NULL;
		memcpy(new_va, virtual_address, old_size);
		kfree(virtual_address);
		return new_va;
	}
	return NULL;
}
