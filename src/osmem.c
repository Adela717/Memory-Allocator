// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "block_meta.h"

#define ALIGNMENT 8

#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

#define META_SIZE sizeof(struct block_meta)

#define META_SIZE_ALIGNED ALIGN(META_SIZE)

#define I_SIZE (1024 * 128)

#define PAGE_SIZE (4 * 1024)

struct block_meta *global_base;

void *base;

int initialised;

struct block_meta *get_block_ptr(void *ptr)
{
	return (struct block_meta *)ptr - 1;
}

void init_heap(void)
{
	base = sbrk(0);
	void *request = sbrk(I_SIZE);

	request++;

	global_base = base;
	global_base->size = I_SIZE - META_SIZE_ALIGNED;
	global_base->next = NULL;
	global_base->prev = NULL;
	global_base->status = STATUS_ALLOC;
	initialised = 1;
}

struct block_meta *find_fit(struct block_meta **last, size_t size)
{
	struct block_meta *current = global_base;

	// coalesce
	struct block_meta *block_aux = global_base;

	while (block_aux && block_aux->next) {
		if (block_aux->status == 0 && block_aux->next->status == 0) {
			block_aux->size = block_aux->size + block_aux->next->size + META_SIZE_ALIGNED;
			block_aux->next = block_aux->next->next;

			if (block_aux->next)
				block_aux->next->prev = block_aux;
		} else {
			block_aux = block_aux->next;
		}
	}

	while (current && !(current->status == 0 && current->size >= size)) {
		// expand
		if (current->next == NULL && size > current->size &&
			current->status == 0) {
			void *request = sbrk(size - current->size);

			request++;

			current->size = size;

			return current;
		}
		*last = current;
		current = current->next;
	}

	// split
	if (current && current->status == 0 &&
	    current->size >= size + META_SIZE_ALIGNED + 8) {
		struct block_meta *aux =
		    (struct block_meta *)((char *)current + META_SIZE_ALIGNED + size);
		aux->size = current->size - size - META_SIZE_ALIGNED;
		current->size = size;

		aux->next = current->next;
		aux->prev = current;

		if (current->next)
			current->next->prev = aux;

		current->next = aux;
		current->status = 1;
		aux->status = 0;
	}
	return current;
}

struct block_meta *request_space(struct block_meta *last, size_t size)
{
	struct block_meta *block;

	block = sbrk(size + META_SIZE_ALIGNED);

	if (block == (void *)-1)
		return NULL;

	if (last)
		last->next = block;

	block->size = size;
	block->next = NULL;
	block->prev = last;
	block->status = 1;
	return block;
}

void *os_malloc(size_t size)
{
	size_t aligned_size = ALIGN(size);
	struct block_meta *block;

	if (size <= 0)
		return NULL;

	if (aligned_size >= I_SIZE) {
		block = mmap(NULL, aligned_size + META_SIZE_ALIGNED, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (block == MAP_FAILED)
			return NULL;

		block->size = aligned_size;
		block->status = 2;
		block->next = NULL;
		block->prev = NULL;
		return (void *)((char *)block + META_SIZE_ALIGNED);
	}

	if (initialised == 0) {
		init_heap();
		if (global_base == NULL)
			return NULL;

		block = global_base;
	} else {
		struct block_meta *last = global_base;

		block = find_fit(&last, aligned_size);

		if (!block) {
			block = request_space(last, aligned_size);
			if (!block)
				return NULL;
		} else {
			block->status = 1;
		}
	}
	return (void *)((char *)block + META_SIZE_ALIGNED);
}

void os_free(void *ptr)
{
	/* TODO: Implement os_free */
	if (!ptr)
		return;

	struct block_meta *block_ptr = get_block_ptr(ptr);

	if (block_ptr->status == 2)
		munmap(block_ptr, block_ptr->size + META_SIZE_ALIGNED);
	else if (block_ptr->status == 1)
		block_ptr->status = 0;
}

void *os_calloc(size_t nmemb, size_t size)
{
	/* TODO: Implement os_calloc */
	size_t total_size;

	if (nmemb == 1)
		total_size = nmemb * ALIGN(size);
	else
		total_size = nmemb * size;

	if (ALIGN(total_size) + META_SIZE_ALIGNED < PAGE_SIZE) {
		void *ptr = os_malloc(total_size);

		if (ptr)
			memset(ptr, 0, total_size);

		return ptr;
	}
		struct block_meta *block;

		block = mmap(NULL, ALIGN(total_size) + META_SIZE_ALIGNED, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (block == MAP_FAILED)
			return NULL;

		block->size = total_size;
		block->status = 2;
		block->next = NULL;
		block->prev = NULL;
		return (void *)((char *)block + META_SIZE_ALIGNED);
}

void *os_realloc(void *ptr, size_t size)
{
	/* TODO: Implement os_realloc */
	if (ptr == NULL)
		return os_malloc(size);

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	struct block_meta *block_ptr = get_block_ptr(ptr);

	if (size == block_ptr->size)
		return ptr;

	if (block_ptr->status == 0)
		return NULL;

	if (block_ptr->size >= I_SIZE && size < I_SIZE) {
		void *new_mem = os_malloc(size);

		os_free(ptr);

		return new_mem;
	} else if (block_ptr->size > size) {
		return ptr;
	}
	// coalesce
	struct block_meta *auxiliary = block_ptr;

	while (auxiliary->next && auxiliary->next == 0) {
		auxiliary->size =
		    auxiliary->size + META_SIZE_ALIGNED + auxiliary->next->size;
		auxiliary->next = auxiliary->next->next;

		if (auxiliary->next)
			auxiliary->next->prev = auxiliary;

		if (auxiliary->size >= ALIGN(size))
			return ptr;
	}

	// expand
	if (block_ptr->next == NULL && size > block_ptr->size) {
		void *request = sbrk(ALIGN(size) - block_ptr->size);

		request++;

		block_ptr->size = ALIGN(size);
		return ptr;
	}

	void *new_ptr = os_malloc(size);

	if (new_ptr) {
		memcpy(new_ptr, ptr, block_ptr->size);
		os_free(ptr);
	}
	return new_ptr;
}s