#pragma once1

#include "chase_lev_queue.h"

#define ATOMIC(T) CL_QUEUE_ATOMIC(T)
enum {LINK_POOL_BLOCK_SIZE = 64};



typedef struct Link_Pool_Block {
    Link_Pool_Block* next;
    ATOMIC(uint32_t) gen;
    ATOMIC(uint32_t) is_head;
    ATOMIC(uint32_t) deleter;

    ATOMIC(uint32_t) slots[LINK_POOL_BLOCK_SIZE]; 
    uint8_t items[];
} Link_Pool_Block;

typedef struct Link_Pool_Thread {
    alignas(64)
    ATOMIC(Link_Pool_Block*) recycled_public;
    Link_Pool_Block* recycled_private;

    ATOMIC(Link_Pool_Block*) head;
    Link_Pool_Block* tail;

    uint32_t total_blocks;
    uint32_t steal_list;
    uint32_t push_index;
    ATOMIC(uint32_t) push_gen;

    uint32_t* generations;
} Link_Pool_Thread ;

typedef struct Link_Pool {
    Link_Pool_Thread* threads;
    uint32_t threads_capacity;
    ATOMIC(uint32_t) threads_count;
} Link_Pool;

Link_Pool_Block* _link_pool_get_block(Link_Pool_Thread* thread, isize item_size)
{
    if(thread->recycled_private == NULL)
        thread->recycled_private = atomic_exchange(&thread->recycled_public, NULL);

    if(thread->recycled_private == NULL)
    {
        Link_Pool_Block* new_block = (Link_Pool_Block*) malloc(sizeof(Link_Pool_Block) + LINK_POOL_BLOCK_SIZE*item_size);
        new_block->next = 0;
        memset(new_block->slots, 0, sizeof new_block->slots);
        new_block->gen = thread->push_gen;

        thread->recycled_private = new_block;
    }

    Link_Pool_Block* block = thread->recycled_private;
    thread->recycled_private = block->next;
    block->next = NULL;
    return block;
}

void _link_pool_push(Link_Pool_Thread* thread, const void* item, isize item_size)
{
    ASSERT(thread->tail != NULL);
    ASSERT(thread->head != NULL);

    Link_Pool_Block* block = thread->tail;
    uint8_t* slot = block->items + thread->push_index*item_size;
    memcpy(slot, item, item_size);
    thread->push_gen += 1;
    thread->push_index += 1;
    atomic_store_explicit(&block->slots[thread->push_index], 1, memory_order_relaxed);
    atomic_store_explicit(&block->gen, thread->push_gen, memory_order_release);

    if(thread->push_index >= LINK_POOL_BLOCK_SIZE)
    {
        thread->push_index = 0;
        Link_Pool_Block* new_tail = _link_pool_get_block(thread, item_size);
        Link_Pool_Block* prev_tail = thread->tail;
        new_tail->is_head = true;
        prev_tail->next = new_tail;
        prev_tail->is_head = true;
        thread->tail = new_tail;
    }
}

bool _link_pool_pop(Link_Pool* pool, Link_Pool_Thread* thread, void* item, isize item_size)
{
    ASSERT(thread->tail != NULL);
    ASSERT(thread->head != NULL);
    uint32_t* gens = thread->generations;
    uint32_t threads_count = atomic_load_explicit(&pool->threads_count, memory_order_relaxed);
    ASSERT(steal < threads_count);
    for(int round = 0; round < 2; round++)
    {
        uint32_t steal = thread->steal_list;
        for(uint32_t k = 0; k < threads_count; k++) 
        {
            Link_Pool_Thread* present_thread = &pool->threads[steal];
            uint32_t present_gen = atomic_load_explicit(&present_thread->push_gen, memory_order_relaxed);
            uint32_t steal_gen = gens[steal];
            if(steal_gen != present_gen)
            {
                Link_Pool_Block* head = atomic_load_explicit(&present_thread->head, memory_order_acquire);
                Link_Pool_Block* block = head;
                
                for(;;) {
                    for(; present_gen != steal_gen; steal_gen++)
                    {
                        uint32_t i = present_gen & (LINK_POOL_BLOCK_SIZE - 1);
                        uint32_t slot_val = atomic_load_explicit(&block->slots[i], memory_order_relaxed);
                        if(slot_val == 1)
                        {
                            uint8_t* slot = block->items + i*item_size;
                            memcpy(item, slot, item_size);
                            if(atomic_exchange(&block->slots[i], 0) == 1)
                            {
                                //if was last then we can delete the block? We must be the sole holders of the block!
                                return true;
                            }
                        }
                    }

                    block = block->next;
                    if(block == NULL)
                        break;
                }

                gens[steal] = steal_gen;
            }

            steal += 1;
            if(steal > threads_count)
                steal = 0;
        }

        //if someone added a thread while we were searching (rare) 
        // start anew just to be safe
        isize new_threads_count = atomic_load_explicit(&pool->threads_count, memory_order_relaxed);
        if(threads_count != new_threads_count)
        {
            threads_count = new_threads_count;
            round = -1;
        }
    }
}
