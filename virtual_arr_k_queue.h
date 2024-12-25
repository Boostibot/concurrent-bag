#ifndef MODULE_VK_QUEUE
#define MODULE_VK_QUEUE

#if defined(_MSC_VER)
    #define VK_QUEUE_INLINE_ALWAYS   __forceinline
    #define VK_QUEUE_INLINE_NEVER    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    #define VK_QUEUE_INLINE_ALWAYS   __attribute__((always_inline)) inline
    #define VK_QUEUE_INLINE_NEVER    __attribute__((noinline))
#else
    #define VK_QUEUE_INLINE_ALWAYS   inline
    #define VK_QUEUE_INLINE_NEVER
#endif

#ifndef VK_QUEUE_API
    #define VK_QUEUE_API_INLINE         VK_QUEUE_INLINE_ALWAYS static
    #define VK_QUEUE_API                static
    #define MODULE_VK_QUEUE_IMPL
#endif

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
    #include <atomic>
    #define VK_QUEUE_ATOMIC(T)    std::atomic<T>
#else
    #include <stdatomic.h>
    #include <stdalign.h>
    #define VK_QUEUE_ATOMIC(T)    _Atomic(T) 
#endif

typedef int64_t isize;

typedef struct VK_Queue_Block {
    struct VK_Queue_Block* next;
    uint64_t mask; //capacity - 1
    //items here...
} VK_Queue_Block;

typedef struct VK_Queue_Slot {
    alignas(64)
    VK_QUEUE_ATOMIC(uint64_t) estimate_tail;
    VK_QUEUE_ATOMIC(uint64_t) gen; //(tail << 1) | taken
} VK_Queue_Slot;

typedef struct VK_Queue {
    alignas(64)
    VK_QUEUE_ATOMIC(uint64_t) head; //changed by pop
    //VK_QUEUE_ATOMIC(uint64_t) estimate_tail;

    alignas(64)
    VK_QUEUE_ATOMIC(uint64_t) tail; //changed by push
    uint64_t estimate_head;

    alignas(64)
    VK_Queue_Slot* slots;
    uint64_t unordered_count;
    VK_QUEUE_ATOMIC(VK_Queue_Block*) block;
    uint32_t item_size;
    uint32_t max_capacity_log2; //0 means max capacity off!
} VK_Queue;

VK_QUEUE_API void vk_queue_deinit(VK_Queue* queue);
VK_QUEUE_API void vk_queue_init(VK_Queue* queue, isize item_size, isize unordered_count, isize max_capacity_or_negative_if_infinite);
VK_QUEUE_API void vk_queue_reserve(VK_Queue* queue, isize to_size);

//Result interface - is sometimes needed when using this queue as a building block for other DS
typedef enum VK_Queue_State{
    VK_QUEUE_OK = 0,
    VK_QUEUE_EMPTY,
    VK_QUEUE_FULL,
    VK_QUEUE_FAILED_RACE, //only returned from vk_queue_result_pop_weak functions
} VK_Queue_State;

//contains the state indicator as well as block, tail, head 
// which hold values obtained *before* the call to the said function
typedef struct VK_Queue_Result {
    uint64_t head;
    uint64_t tail;
    VK_Queue_State state;
    int _;
} VK_Queue_Result;
#endif

#if (defined(MODULE_ALL_IMPL) || defined(MODULE_VK_QUEUE_IMPL)) && !defined(MODULE_VK_QUEUE_HAS_IMPL)
#define MODULE_VK_QUEUE_HAS_IMPL

#ifdef MODULE_COUPLED
    #include "assert.h"
#endif

#ifndef ASSERT
    #include <assert.h>
    #define ASSERT(x, ...) assert(x)
#endif

#ifdef __cplusplus
    #define _VK_QUEUE_USE_ATOMICS \
        using std::memory_order_acquire;\
        using std::memory_order_release;\
        using std::memory_order_seq_cst;\
        using std::memory_order_relaxed;\
        using std::memory_order_consume;
#else
    #define _VK_QUEUE_USE_ATOMICS
#endif

VK_QUEUE_API void vk_queue_deinit(VK_Queue* queue)
{
    for(VK_Queue_Block* curr = queue->block; curr; )
    {
        VK_Queue_Block* next = curr->next;
        free(curr);
        curr = next;
    }
    memset(queue, 0, sizeof *queue);
    atomic_store(&queue->block, NULL);
}

VK_QUEUE_API void vk_queue_init(VK_Queue* queue, isize item_size, isize unordered_count, isize max_capacity_or_negative_if_infinite)
{
    vk_queue_deinit(queue);
    queue->item_size = (uint32_t) item_size;
    if(max_capacity_or_negative_if_infinite >= 0)
    {
        while((uint64_t) 1 << queue->max_capacity_log2 < (uint64_t) max_capacity_or_negative_if_infinite)
            queue->max_capacity_log2 ++;

        queue->max_capacity_log2 ++;
    }
    queue->slots = (VK_Queue_Slot*) _aligned_malloc(unordered_count*sizeof(VK_Queue_Slot), 64);
    queue->unordered_count = unordered_count;
    atomic_store(&queue->block, NULL);
}

VK_QUEUE_API_INLINE void* _vk_queue_slot(VK_Queue_Block* block, uint64_t i, isize item_size)
{
    uint64_t mapped = i & block->mask;
    uint8_t* data = (uint8_t*) (void*) (block + 1);
    return data + mapped*item_size;
}

VK_QUEUE_INLINE_NEVER
VK_QUEUE_API VK_Queue_Block* _vk_queue_reserve(VK_Queue* queue, isize to_size)
{
    VK_Queue_Block* old_block = atomic_load(&queue->block);
    VK_Queue_Block* out_block = old_block;
    isize old_cap = old_block ? (isize) (old_block->mask + 1) : 0;
    isize item_size = queue->item_size;
    isize max_capacity = queue->max_capacity_log2 > 0 
        ? (isize) 1 << (queue->max_capacity_log2 - 1) 
        : INT64_MAX;

    if(old_cap < to_size && to_size <= max_capacity)
    {
        uint64_t new_cap = 64;
        while((isize) new_cap < to_size)
            new_cap *= 2;

        VK_Queue_Block* new_block = (VK_Queue_Block*) malloc(sizeof(VK_Queue_Block) + new_cap*item_size);
        if(new_block)
        {
            new_block->next = old_block;
            new_block->mask = new_cap - 1;

            if(old_block)
            {
                uint64_t t = atomic_load(&queue->head);
                uint64_t b = atomic_load(&queue->tail);
                for(uint64_t i = t; (int64_t) (i - b) < 0; i++) //i < b
                    memcpy(_vk_queue_slot(new_block, i, item_size), _vk_queue_slot(old_block, i, item_size), item_size);
            }

            atomic_store(&queue->block, new_block);
            out_block = new_block;
        }
        
    }

    return out_block;
}

VK_QUEUE_API void vk_queue_reserve(VK_Queue* queue, isize to_size)
{
    _vk_queue_reserve(queue, to_size);
}

VK_QUEUE_API_INLINE VK_Queue_Result vk_queue_result_push(VK_Queue *q, const void* item, isize item_size)
{
    _VK_QUEUE_USE_ATOMICS;
    ASSERT(q->item_size == item_size);

    VK_Queue_Block *a = atomic_load_explicit(&q->block, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint64_t head = q->estimate_head;

    if (a == NULL || (int64_t)(tail - head) > (int64_t) a->mask) { 
        head = atomic_load_explicit(&q->head, memory_order_acquire);
        q->estimate_head = head;
        if (a == NULL || (int64_t)(tail - head) > (int64_t) a->mask) { 
            VK_Queue_Block* new_a = _vk_queue_reserve(q, tail - head + 1);
            if(new_a == a)
            {
                VK_Queue_Result out = {tail, head, VK_QUEUE_FULL};
                return out;
            }

            a = new_a;
        }
    }
    
    void* slot = _vk_queue_slot(a, tail, item_size);
    memcpy(slot, item, item_size);

    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    VK_Queue_Result out = {head, tail, VK_QUEUE_OK};
    return out;
}

VK_QUEUE_API_INLINE VK_Queue_Result vk_queue_result_pop(VK_Queue *q, void* item, isize item_size)
{
    _VK_QUEUE_USE_ATOMICS;
    ASSERT(q->item_size == item_size);
    uint64_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint64_t unordered = q->unordered_count;

    for(;;) {
        VK_Queue_Block *a = atomic_load_explicit(&q->block, memory_order_seq_cst);

        uint64_t from = rand();
        uint64_t curr_gen = head << 1 | 1;
        for(uint64_t k = 0; k < unordered; k++)
        {
            uint64_t slot_i = (from + k) & (unordered - 1);
            VK_Queue_Slot* slot = q->slots + slot_i;

            //if the slot is filled
            if(head + slot_i < tail)
            {
                uint64_t slot_gen = atomic_load_explicit(&slot->gen, memory_order_relaxed);
                //if the slot is not taken or is from previous gen
                // (if is from future gen then we are left behind. We should rerun this entire function).
                if((slot_gen & 1) == 0 || (int64_t) (slot_gen - curr_gen) < 0)
                {
                    //Is this needed??? I dont think so!
                    //atomic_thread_fence(memory_order_acquire); 
                    //copy data over
                    void* data = _vk_queue_slot(a, head + slot_i, item_size);
                    memcpy(item, data, item_size);

                    //and try to take this slot
                    if (atomic_compare_exchange_strong_explicit(&slot->gen, &slot_gen, curr_gen, memory_order_seq_cst, memory_order_relaxed))
                    {
                        VK_Queue_Result out = {head + slot_i, tail, VK_QUEUE_OK};
                        return out;
                    }
                }
            }
        }

        //if all slots were empty...
    
        //test if someone else moved head along. If so try again
        uint64_t new_head = atomic_load_explicit(&q->head, memory_order_relaxed);
        if(new_head == head)
        {
            //if we can move by unordered_count along, move.
            if((int64_t) (tail - head) >= unordered)
            {
                if (atomic_compare_exchange_strong_explicit(&q->head, &head, head + unordered, memory_order_relaxed, memory_order_relaxed))
                    new_head = head + unordered;
                else
                    new_head = atomic_load_explicit(&q->head, memory_order_relaxed);
            }
        }
    
        //if there were no pushes then the queue really is empty.
        uint64_t new_tail = atomic_load_explicit(&q->head, memory_order_relaxed);
        if((int64_t) ((head + unordered) - new_tail) >= 0)
        {
            VK_Queue_Result out = {new_tail, new_tail, VK_QUEUE_EMPTY};
            return out;
        }
    
        head = new_head;
        tail = new_tail;
    }
}

VK_QUEUE_API_INLINE bool vk_queue_push(VK_Queue *q, const void* item, isize item_size)
{
    return vk_queue_result_push(q, item, item_size).state == VK_QUEUE_OK;
}

VK_QUEUE_API_INLINE bool vk_queue_pop(VK_Queue *q, void* item, isize item_size)
{
    return vk_queue_result_pop(q, item, item_size).state == VK_QUEUE_OK;
}

VK_QUEUE_API_INLINE isize vk_queue_capacity(const VK_Queue *q)
{
    _VK_QUEUE_USE_ATOMICS;
    VK_Queue_Block *a = atomic_load_explicit(&q->block, memory_order_relaxed);
    return a ? (isize) a->mask + 1 : 0;
}

VK_QUEUE_API_INLINE isize vk_queue_count(const VK_Queue *q)
{
    //TODO
    _VK_QUEUE_USE_ATOMICS;
    uint64_t t = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint64_t b = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint64_t diff = (isize) (b - t);
    return diff >= 0 ? diff : 0;
}

#endif