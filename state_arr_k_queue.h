#pragma once

#include "chase_lev_queue.h"

//#define ATOMIC(T) _Atomic(T)
#define ATOMIC(T) std::atomic<T>

typedef struct K_Queue_Block {
    alignas(64)
    ATOMIC(uint64_t) tail;
    ATOMIC(uint64_t) gen;
    struct K_Queue_Block* next;
    uint64_t mask; //capacity - 1
} K_Queue_Block;

typedef struct K_Queue_Local {
    uint64_t head;
    uint64_t tail;
    uint64_t gen;
    uint64_t mask;
    K_Queue_Block* block;
    uint64_t rand; 
    uint64_t iter; //[0, chunk_size = K)

    uint64_t reloads;
} K_Queue_Local;

typedef struct K_Queue {
    ATOMIC(uint64_t) head;
    union {
        ATOMIC(K_Queue_Block*) block;
        K_Queue_Block* _block;
    };
} K_Queue;

typedef struct SPMC_Slot {
    ATOMIC(uint64_t)* gen;
    void* item;
} SPMC_Slot;

SPMC_Slot _k_queue_slot(K_Queue_Block* block, uint64_t mask, isize i, isize item_size)
{
    isize slot_size = item_size + sizeof(uint64_t);
    uint8_t* block_data = (uint8_t*) (void*) (block + 1); 
    uint8_t* slot_ptr = block_data + ((uint64_t) i & mask)*slot_size;
    SPMC_Slot slot = {0};
    slot.item = slot_ptr;
    slot.gen = (ATOMIC(uint64_t)*) (void*) (slot_ptr + item_size);

    return slot;
}

void k_queue_deinit(K_Queue* pool)
{
    for(K_Queue_Block* curr = pool->block; curr; )
    {
        K_Queue_Block* next = curr->next;
        _aligned_free(curr);
        curr = next;
    }
    atomic_store_explicit(&pool->head, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->block, NULL, memory_order_release);
}
void k_queue_init(K_Queue* pool, isize capacity, isize item_size)
{
    k_queue_deinit(pool);
    isize pow_2_capacity = 64;
    while(pow_2_capacity < capacity)
        pow_2_capacity *= 2;

    isize block_alloced_size = sizeof(K_Queue_Block) + pow_2_capacity*(item_size + sizeof(uint64_t));
    K_Queue_Block* block = (K_Queue_Block*) _aligned_malloc(block_alloced_size, 64);
    ASSERT(block);

    memset(block, 0, block_alloced_size);
    block->mask = pow_2_capacity - 1;
    atomic_store_explicit(&pool->block, block, memory_order_release);
}

K_Queue_Local k_queue_local(K_Queue* queue)
{
    K_Queue_Local local = {0};
    local.head = atomic_load_explicit(&queue->head, memory_order_relaxed);
    local.block = atomic_load_explicit(&queue->block, memory_order_relaxed);
    local.tail = atomic_load_explicit(&local.block->tail, memory_order_relaxed);
    local.gen = atomic_load_explicit(&local.block->gen, memory_order_relaxed);
    local.mask = local.block->mask;

    return local;
}

K_Queue_Block* _k_queue_make_block(K_Queue* pool, K_Queue_Block* old_block, isize item_size)
{
    uint64_t head = atomic_load_explicit(&pool->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&old_block->tail, memory_order_relaxed);
    uint64_t gen = atomic_load_explicit(&old_block->gen, memory_order_relaxed);

    isize old_capacity = old_block->mask + 1;
    isize new_capacity = old_capacity*2;
    ASSERT(0 < new_capacity && new_capacity < (1ll << 62));

    isize oldblock_size = sizeof(K_Queue_Block) + old_capacity*(item_size + sizeof(uint64_t));
    isize new_block_size = sizeof(K_Queue_Block) + new_capacity*(item_size + sizeof(uint64_t));
    K_Queue_Block* new_block = (K_Queue_Block*) _aligned_malloc(new_block_size, 64);
    ASSERT(new_block);
    
    isize old_mask = old_capacity - 1;
    isize new_mask = new_capacity - 1;

    #ifndef NDEBUG
        memset(new_block, -1, new_block_size);
    #endif

    for(isize k = 0; k < new_capacity; k++)
    {
        ATOMIC(uint64_t)* gen_ptr = _k_queue_slot(new_block, new_mask, k, item_size).gen;
        atomic_store_explicit(gen_ptr, gen << 1, memory_order_relaxed);
    }
    
    for(uint64_t k = head; (int64_t) (k - tail) < 0; k++)
    {
        void* new_item = _k_queue_slot(new_block, new_mask, k, item_size).item;
        void* old_item = _k_queue_slot(old_block, old_mask, k, item_size).item;
        memcpy(new_item, old_item, item_size + sizeof(uint64_t));
    }

    new_block->next = NULL;
    new_block->mask = new_capacity - 1;
    atomic_store_explicit(&new_block->tail, tail, memory_order_relaxed);
    atomic_store_explicit(&new_block->gen, gen, memory_order_relaxed);

    return new_block;
}

CL_QUEUE_INLINE_NEVER
void _k_queue_push_grow(K_Queue* pool, K_Queue_Local* local_ptr, isize item_size)
{
    //make new block
    K_Queue_Block* old_block = local_ptr->block;
    K_Queue_Block* new_block = _k_queue_make_block(pool, old_block, item_size);
    
    old_block->next = new_block;
    atomic_store_explicit(&pool->block, new_block, memory_order_release);
    local_ptr->block = new_block;
    local_ptr->mask = new_block->mask;
}

__forceinline
bool k_queue_push(K_Queue* pool, K_Queue_Local* local, const void* item, isize item_size)
{
    uint64_t min_head = local->tail - local->mask - 1;
    //if full reload the head estimate
    if(local->head == min_head)
    {
        local->reloads += 1;
        local->head = atomic_load_explicit(&pool->head, memory_order_relaxed);
        //if still full then really is full grow.
        if(local->head == min_head)
            _k_queue_push_grow(pool, local, item_size);
    }

    //store and update shared structures
    K_Queue_Block* block = local->block;
    uint64_t mask = local->mask;
    uint64_t tail = local->tail;
    uint64_t gen = local->gen;
    SPMC_Slot slot = _k_queue_slot(local->block, mask, tail, item_size);
    memcpy(slot.item, item, item_size);
    
    //if we wrapped around the queue then increase generation
    if(((tail + 1) & mask) == 0)
    {
        local->gen = gen + 1;
        atomic_store_explicit(&block->gen, gen + 1, memory_order_relaxed);
    }

    atomic_store_explicit(&block->tail, tail + 1, memory_order_relaxed);
    atomic_store_explicit(slot.gen, (gen << 1) | 1, memory_order_release); //release!
    
    local->tail = tail + 1;
    return true;
}

bool k_queue_pop_back(K_Queue* pool, K_Queue_Local* local_ptr, void* item, isize item_size)
{
    ASSERT(false);
    K_Queue_Local local = *local_ptr;
    K_Queue_Block* block = local.block;
    SPMC_Slot slot = _k_queue_slot(local.block, local.mask, local.tail, item_size);

    //scan slots from the back up to chunk size before giving up
    uint64_t slot_gen = atomic_load_explicit(slot.gen, memory_order_relaxed);
    if((slot_gen & 1) == 0)
    {
        //since we are the sole writer we can first secure our spot and only then copy
        //There is no fear of the writer coming over and overwriting our not-yet-copied data.
        if(atomic_exchange_explicit(slot.gen, (local.gen << 1) | 0, memory_order_relaxed) == slot_gen)
        {
            memcpy(item, slot.item, item_size);
            *local_ptr = local;
            return true;
        }
    }

    memcpy(slot.item, item, item_size);
    //if we wrapped around the queue then increase generation
    if(((local.tail + 1) & local.mask) == 0)
    {
        local_ptr->gen = local.gen + 1;
        atomic_store_explicit(&block->gen, local.gen + 1, memory_order_relaxed);
    }

    atomic_store_explicit(&block->tail, local.tail + 1, memory_order_relaxed);
    atomic_store_explicit(slot.gen, (local.gen << 1) | 1, memory_order_release); //release!

    local_ptr->tail = local.tail + 1;
    return true;
}

__forceinline
bool k_queue_pop(K_Queue* pool, K_Queue_Local* local, void* item, isize item_size, uint64_t chunk_size)
{
    for(;;) {
        //we iterate one block. Just one - if there have been many many pushes 
        // we dont want to have to scan everything just to catch up.
        //Instead after one block we give up and ask whats the current state of things
        uint64_t sgen = (local->gen << 1) | 1;
        for(uint64_t iter = 0; iter < chunk_size; iter ++)
        {
            uint64_t i = local->head + (local->rand + iter) % chunk_size;
            SPMC_Slot slot = _k_queue_slot(local->block, local->mask, i, item_size);
            uint64_t slot_gen = atomic_load_explicit(slot.gen, memory_order_relaxed);
            if((int64_t) (sgen - slot_gen) >= 0 && (slot_gen & 1) == 1)
            {
                atomic_thread_fence(memory_order_acquire);
                memcpy(item, slot.item, item_size);
                    //memset(slot.item, -1, item_size);

                if(atomic_compare_exchange_strong_explicit(slot.gen, &slot_gen, local->gen << 1, memory_order_relaxed, memory_order_relaxed))
                    return true;
                else
                    slot_gen = atomic_load_explicit(slot.gen, memory_order_relaxed);
            }

            if(slot_gen & 2)
            {
                K_Queue_Block* alt_block = local->block->next
            
            }
        }
        
        isize retry_counter = 0;
        uint64_t old_head = local->head;
        uint64_t old_gen = local->gen;

        local->head = atomic_load_explicit(&pool->head, memory_order_relaxed);
        load_again:
        local->block = atomic_load_explicit(&pool->block, memory_order_relaxed);
        local->tail = atomic_load_explicit(&local->block->tail, memory_order_relaxed);
        local->gen = atomic_load_explicit(&local->block->gen, memory_order_relaxed);
        local->mask = local->block->mask;
        local->reloads += 1;
        
        //if block reallocated and we read the old block and new head (other thread popped) then we could have head > tail.
        // In that case we just put down barrier and try again.
        if((int64_t) (local->head - local->tail) > 0)
        {
            ASSERT(retry_counter++ == 0, "Must not get stuck in a cycle");
            atomic_thread_fence(memory_order_seq_cst);
            goto load_again;
        }

        //if this really is the last chunk
        if(local->head == old_head)
        {
            //we scanned the full chunk - try moving the head forward
            if((int64_t) (local->tail - local->head) >= (int64_t) chunk_size)
            {
                if(atomic_compare_exchange_strong_explicit(&pool->head, &local->head, local->head + chunk_size, memory_order_relaxed, memory_order_relaxed))
                    local->head += chunk_size;
                else
                    local->head = atomic_load_explicit(&pool->head, memory_order_relaxed);
            }

            //if there were no pushes and there is no space at all then we are canonically empty
            uint64_t scanned_to = old_head + chunk_size;
            if(local->gen == old_gen && (int64_t) (scanned_to - local->tail) > 0)
                return false;
        }
    }
}