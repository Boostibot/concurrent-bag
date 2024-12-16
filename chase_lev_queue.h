#ifndef JOT_CHASE_LEV_QUEUE
#define JOT_CHASE_LEV_QUEUE

#if defined(_MSC_VER)
    #define CL_QUEUE_INLINE_ALWAYS   __forceinline
    #define CL_QUEUE_INLINE_NEVER    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    #define CL_QUEUE_INLINE_ALWAYS   __attribute__((always_inline)) inline
    #define CL_QUEUE_INLINE_NEVER    __attribute__((noinline))
#else
    #define CL_QUEUE_INLINE_ALWAYS   inline
    #define CL_QUEUE_INLINE_NEVER
#endif

#ifndef CL_QUEUE_API
    #define CL_QUEUE_API_INLINE         CL_QUEUE_INLINE_ALWAYS static
    #define CL_QUEUE_API                static
    #define JOT_CHASE_LEV_QUEUE_IMPL
#endif

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
    #include <atomic>
    #define CL_QUEUE_ATOMIC(T)    std::atomic<T>
#else
    #include <stdatomic.h>
    #include <stdalign.h>
    #define CL_QUEUE_ATOMIC(T)    _Atomic(T) 
#endif

typedef int64_t isize;

typedef struct CL_Queue_Block {
    struct CL_Queue_Block* next;
    uint64_t mask; //capacity - 1
    //items here...
} CL_Queue_Block;

typedef struct CL_Queue {
    alignas(64)
    CL_QUEUE_ATOMIC(uint64_t) top; //changed by pop
    alignas(64)
    CL_QUEUE_ATOMIC(uint64_t) bot; //changed by push (and pop_back)
    CL_QUEUE_ATOMIC(uint64_t) bot_ticket;
    //alignas(64)
    CL_QUEUE_ATOMIC(CL_Queue_Block*) block;
    CL_QUEUE_ATOMIC(uint32_t) item_size;
    CL_QUEUE_ATOMIC(uint32_t) max_capacity_log2; //0 means max capacity off!
} CL_Queue;

CL_QUEUE_API void cl_queue_deinit(CL_Queue* queue);
CL_QUEUE_API void cl_queue_init(CL_Queue* queue, isize item_size, isize max_capacity_or_negative_if_infinite);
CL_QUEUE_API void cl_queue_reserve(CL_Queue* queue, isize to_size);
CL_QUEUE_API_INLINE bool cl_queue_push(CL_Queue *q, const void* item, isize item_size);
CL_QUEUE_API_INLINE bool cl_queue_pop(CL_Queue *q, void* item, isize item_size);
CL_QUEUE_API_INLINE bool cl_queue_pop_back(CL_Queue *q, void* item, isize item_size); 
CL_QUEUE_API_INLINE isize cl_queue_capacity(const CL_Queue *q);
CL_QUEUE_API_INLINE isize cl_queue_count(const CL_Queue *q);

//Result interface - is sometimes needed when using this queue as a building block for other DS
typedef enum CL_Queue_State{
    CL_QUEUE_OK = 0,
    CL_QUEUE_EMPTY,
    CL_QUEUE_FULL,
    CL_QUEUE_FAILED_RACE, //only returned from cl_queue_result_pop_weak functions
} CL_Queue_State;

//contains the state indicator as well as block, bot, top 
// which hold values obtained *before* the call to the said function
typedef struct CL_Queue_Result {
    CL_Queue_Block* block;
    uint64_t bot;
    uint64_t top;
    CL_Queue_State state;
    int _;
} CL_Queue_Result;

CL_QUEUE_API_INLINE CL_Queue_Result cl_queue_result_push(CL_Queue *q, void* item, isize item_size);
CL_QUEUE_API_INLINE CL_Queue_Result cl_queue_result_pop(CL_Queue *q, void* item, isize item_size);
CL_QUEUE_API_INLINE CL_Queue_Result cl_queue_result_pop_weak(CL_Queue *q, void* item, isize item_size);

#endif

#if (defined(JOT_ALL_IMPL) || defined(JOT_CHASE_LEV_QUEUE_IMPL)) && !defined(JOT_CHASE_LEV_QUEUE_HAS_IMPL)
#define JOT_CHASE_LEV_QUEUE_HAS_IMPL

#ifdef JOT_COUPLED
    #include "assert.h"
#endif

#ifndef ASSERT
    #include <assert.h>
    #define ASSERT(x, ...) assert(x)
#endif

#ifdef __cplusplus
    using std::memory_order_acquire;
    using std::memory_order_release;
    using std::memory_order_seq_cst;
    using std::memory_order_relaxed;
    using std::memory_order_consume;
#endif

CL_QUEUE_API void cl_queue_deinit(CL_Queue* queue)
{
    for(CL_Queue_Block* curr = queue->block; curr; )
    {
        CL_Queue_Block* next = curr->next;
        free(curr);
        curr = next;
    }
    memset(queue, 0, sizeof *queue);
    atomic_store(&queue->block, NULL);
}

CL_QUEUE_API void cl_queue_init(CL_Queue* queue, isize item_size, isize max_capacity_or_negative_if_infinite)
{
    cl_queue_deinit(queue);
    queue->item_size = (uint32_t) item_size;
    if(max_capacity_or_negative_if_infinite >= 0)
    {
        while((uint64_t) 1 << queue->max_capacity_log2 < (uint64_t) max_capacity_or_negative_if_infinite)
            queue->max_capacity_log2 ++;

        queue->max_capacity_log2 ++;
    }

    atomic_store(&queue->block, NULL);
}

CL_QUEUE_API_INLINE void* _cl_queue_slot(CL_Queue_Block* block, uint64_t i, isize item_size)
{
    uint64_t mapped = i & block->mask;
    if(1)
    {
        assert(mapped <= block->mask + 1);
        //before: mappped = [    high   ][ mid ][ lo ]
        //                  <-----58----><--4--><-2-->
        //after: mappped =  [    high   ][ lo ][ mid ]
        //                  <-----58----><-2--><--4-->
        mapped = (mapped & ~0x3Full)
            | (mapped & 0x3ull) << 4
            | (mapped & 0x3Cull) >> 2;
        assert(mapped <= block->mask + 1);
    }

    uint8_t* data = (uint8_t*) (void*) (block + 1);
    return data + mapped*item_size;
}

CL_QUEUE_INLINE_NEVER
CL_QUEUE_API CL_Queue_Block* _cl_queue_reserve(CL_Queue* queue, isize to_size)
{
    CL_Queue_Block* old_block = atomic_load(&queue->block);
    CL_Queue_Block* out_block = old_block;
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

        CL_Queue_Block* new_block = (CL_Queue_Block*) malloc(sizeof(CL_Queue_Block) + new_cap*item_size);
        new_block->next = old_block;
        new_block->mask = new_cap - 1;

        if(old_block)
        {
            uint64_t t = atomic_load(&queue->top);
            uint64_t b = atomic_load(&queue->bot);
            for(uint64_t i = t; i < b; i++)
                memcpy(_cl_queue_slot(new_block, i, item_size), _cl_queue_slot(old_block, i, item_size), item_size);
        }

        atomic_store(&queue->block, new_block);
        out_block = new_block;
        
    }

    return out_block;
}

CL_QUEUE_API void cl_queue_reserve(CL_Queue* queue, isize to_size)
{
    _cl_queue_reserve(queue, to_size);
}

CL_QUEUE_API_INLINE CL_Queue_Result cl_queue_result_pop_back(CL_Queue *q, void* item, isize item_size)
{
    ASSERT(atomic_load_explicit(&q->item_size, memory_order_relaxed) == item_size);

    uint64_t b = atomic_load_explicit(&q->bot, memory_order_relaxed) - 1;
    CL_Queue_Block* a = atomic_load_explicit(&q->block, memory_order_relaxed);
    atomic_store_explicit(&q->bot, b, memory_order_relaxed);
    //atomic_thread_fence(memory_order_seq_cst);
    atomic_fetch_add_explicit(&q->bot_ticket, 1, memory_order_seq_cst);
    uint64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    
    CL_Queue_Result out = {a, b, t, CL_QUEUE_OK};
    if ((int64_t) (t - b) <= 0) { //t <= b
        if (t == b) {
            // Single last element in queue. 
            if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1, memory_order_seq_cst, memory_order_relaxed))
                // Failed race (=> the queue must be empty since we are the only producer)
                goto fail;

            atomic_store_explicit(&q->bot, b + 1, memory_order_relaxed);
        }
        
        //@NOTE: copy out once a slot has been secured. 
        //We can do this because this function can only be called by the owner
        // (thus there is no risk of push overwriting the data)
        //Additionally we dont need to worry about memory barriers since we are the ones
        // who stored the data there.
        //This is possibly the only major change we have made compared to the reference paper
        void* slot = _cl_queue_slot(a, b, item_size);
        memcpy(item, slot, item_size);
    } 
    //Empty queue
    else { 
        fail:
        atomic_store_explicit(&q->bot, b + 1, memory_order_relaxed);
        out.state = CL_QUEUE_EMPTY; 
    }
    return out;
}


CL_QUEUE_API_INLINE CL_Queue_Result cl_queue_result_push(CL_Queue *q, const void* item, isize item_size)
{
    ASSERT(atomic_load_explicit(&q->item_size, memory_order_relaxed) == item_size);

    uint64_t b = atomic_load_explicit(&q->bot, memory_order_relaxed);
    uint64_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    CL_Queue_Block *a = atomic_load_explicit(&q->block, memory_order_relaxed);
    if (a == NULL || (int64_t)(b - t) > (int64_t) a->mask) { 
        CL_Queue_Block* new_a = _cl_queue_reserve(q, b - t + 1);
        if(new_a == a)
        {
            CL_Queue_Result out = {a, b, t, CL_QUEUE_FULL};
            return out;
        }

        a = new_a;
    }
    
    void* slot = _cl_queue_slot(a, b, item_size);
    memcpy(slot, item, item_size);

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&q->bot, b + 1, memory_order_relaxed);
    CL_Queue_Result out = {a, b, t, CL_QUEUE_OK};
    return out;
}

CL_QUEUE_API_INLINE CL_Queue_Result cl_queue_result_pop_weak(CL_Queue *q, void* item, isize item_size)
{
    ASSERT(atomic_load_explicit(&q->item_size, memory_order_relaxed) == item_size);

    uint64_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    uint64_t b = atomic_load_explicit(&q->bot, memory_order_acquire);
    
    CL_Queue_Block *a = atomic_load_explicit(&q->block, memory_order_consume);

    CL_Queue_Result out = {a, b, t, CL_QUEUE_EMPTY};
    if ((int64_t) (t - b) < 0) { //t < b 
        void* slot = _cl_queue_slot(a, t, item_size);
        memcpy(item, slot, item_size);

        if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1, memory_order_seq_cst, memory_order_relaxed))
            out.state = CL_QUEUE_FAILED_RACE;
        else
            out.state = CL_QUEUE_OK;
    }

    return out;
}

CL_QUEUE_API_INLINE CL_Queue_Result cl_queue_result_pop(CL_Queue *q, void* item, isize item_size)
{
    for(;;) {
        CL_Queue_Result result = cl_queue_result_pop_weak(q, item, item_size);
        if(result.state != CL_QUEUE_FAILED_RACE)
            return result;
    }
}

CL_QUEUE_API_INLINE bool cl_queue_push(CL_Queue *q, const void* item, isize item_size)
{
    return cl_queue_result_push(q, item, item_size).state == CL_QUEUE_OK;
}

CL_QUEUE_API_INLINE bool cl_queue_pop(CL_Queue *q, void* item, isize item_size)
{
    return cl_queue_result_pop(q, item, item_size).state == CL_QUEUE_OK;
}

CL_QUEUE_API_INLINE bool cl_queue_pop_back(CL_Queue *q, void* item, isize item_size)
{
    return cl_queue_result_pop_back(q, item, item_size).state == CL_QUEUE_OK;
}

CL_QUEUE_API_INLINE isize cl_queue_capacity(const CL_Queue *q)
{
    CL_Queue_Block *a = atomic_load_explicit(&q->block, memory_order_relaxed);
    return a ? (isize) a->mask + 1 : 0;
}

CL_QUEUE_API_INLINE isize cl_queue_count(const CL_Queue *q)
{
    uint64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    uint64_t b = atomic_load_explicit(&q->bot, memory_order_relaxed);
    uint64_t diff = (isize) (b - t);
    return diff >= 0 ? diff : 0;
}

#endif