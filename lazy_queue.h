#ifndef MODULE_LAZY_QUEUE
#define MODULE_LAZY_QUEUE

#if defined(_MSC_VER)
    #define LAZY_QUEUE_INLINE_ALWAYS   __forceinline
    #define LAZY_QUEUE_INLINE_NEVER    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    #define LAZY_QUEUE_INLINE_ALWAYS   __attribute__((always_inline)) inline
    #define LAZY_QUEUE_INLINE_NEVER    __attribute__((noinline))
#else
    #define LAZY_QUEUE_INLINE_ALWAYS   inline
    #define LAZY_QUEUE_INLINE_NEVER
#endif

#ifndef LAZY_QUEUE_API
    #define LAZY_QUEUE_API_INLINE         LAZY_QUEUE_INLINE_ALWAYS static
    #define LAZY_QUEUE_API                static
    #define MODULE_LAZY_QUEUE_IMPL
#endif

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
    #include <atomic>
    #define LAZY_QUEUE_ATOMIC(T)    std::atomic<T>
#else
    #include <stdatomic.h>
    #include <stdalign.h>
    #define LAZY_QUEUE_ATOMIC(T)    _Atomic(T) 
#endif

typedef int64_t isize;

typedef struct Lazy_Queue_Block {
    struct Lazy_Queue_Block* next;
    uint64_t mask; //capacity - 1
    //items here...
} Lazy_Queue_Block;

typedef struct Lazy_Queue {
    alignas(64)
    LAZY_QUEUE_ATOMIC(uint64_t) top; //changed by pop
    LAZY_QUEUE_ATOMIC(uint64_t) estimate_bot;

    alignas(64)
    LAZY_QUEUE_ATOMIC(uint64_t) bot; //changed by push
    uint64_t estimate_top;

    alignas(64)
    LAZY_QUEUE_ATOMIC(Lazy_Queue_Block*) block;
    LAZY_QUEUE_ATOMIC(uint32_t) item_size;
    LAZY_QUEUE_ATOMIC(uint32_t) max_capacity_log2; //0 means max capacity off!
} Lazy_Queue;

LAZY_QUEUE_API void lazy_queue_deinit(Lazy_Queue* queue);
LAZY_QUEUE_API void lazy_queue_init(Lazy_Queue* queue, isize item_size, isize max_capacity_or_negative_if_infinite);
LAZY_QUEUE_API void lazy_queue_reserve(Lazy_Queue* queue, isize to_size);
LAZY_QUEUE_API_INLINE bool lazy_queue_st_push(Lazy_Queue *q, const void* item, isize item_size);
LAZY_QUEUE_API_INLINE bool lazy_queue_st_pop(Lazy_Queue *q, void* item, isize item_size);
LAZY_QUEUE_API_INLINE bool lazy_queue_pop(Lazy_Queue *q, void* item, isize item_size);
LAZY_QUEUE_API_INLINE isize lazy_queue_capacity(const Lazy_Queue *q);
LAZY_QUEUE_API_INLINE isize lazy_queue_count(const Lazy_Queue *q);

//Result interface - is sometimes needed when using this queue as a building block for other DS
typedef enum Lazy_Queue_State{
    LAZY_QUEUE_OK = 0,
    LAZY_QUEUE_EMPTY,
    LAZY_QUEUE_FULL,
    LAZY_QUEUE_FAILED_RACE, //only returned from lazy_queue_result_pop_weak functions
} Lazy_Queue_State;

//contains the state indicator as well as block, bot, top 
// which hold values obtained *before* the call to the said function
typedef struct Lazy_Queue_Result {
    uint64_t bot;
    uint64_t top;
    Lazy_Queue_State state;
    int _;
} Lazy_Queue_Result;

LAZY_QUEUE_API_INLINE Lazy_Queue_Result lazy_queue_result_st_push(Lazy_Queue *q, void* item, isize item_size);
LAZY_QUEUE_API_INLINE Lazy_Queue_Result lazy_queue_result_st_pop(Lazy_Queue *q, void* item, isize item_size);
LAZY_QUEUE_API_INLINE Lazy_Queue_Result lazy_queue_result_pop(Lazy_Queue *q, void* item, isize item_size);
LAZY_QUEUE_API_INLINE Lazy_Queue_Result lazy_queue_result_pop_weak(Lazy_Queue *q, void* item, isize item_size);
#endif

#if (defined(MODULE_ALL_IMPL) || defined(MODULE_LAZY_QUEUE_IMPL)) && !defined(MODULE_LAZY_QUEUE_HAS_IMPL)
#define MODULE_LAZY_QUEUE_HAS_IMPL

#ifdef MODULE_COUPLED
    #include "assert.h"
#endif

#ifndef ASSERT
    #include <assert.h>
    #define ASSERT(x, ...) assert(x)
#endif

#ifdef __cplusplus
    #define _LAZY_QUEUE_USE_ATOMICS \
        using std::memory_order_acquire;\
        using std::memory_order_release;\
        using std::memory_order_seq_cst;\
        using std::memory_order_relaxed;\
        using std::memory_order_consume;
#else
    #define _LAZY_QUEUE_USE_ATOMICS
#endif

LAZY_QUEUE_API void lazy_queue_deinit(Lazy_Queue* queue)
{
    for(Lazy_Queue_Block* curr = queue->block; curr; )
    {
        Lazy_Queue_Block* next = curr->next;
        free(curr);
        curr = next;
    }
    memset(queue, 0, sizeof *queue);
    atomic_store(&queue->block, NULL);
}

LAZY_QUEUE_API void lazy_queue_init(Lazy_Queue* queue, isize item_size, isize max_capacity_or_negative_if_infinite)
{
    lazy_queue_deinit(queue);
    queue->item_size = (uint32_t) item_size;
    if(max_capacity_or_negative_if_infinite >= 0)
    {
        while((uint64_t) 1 << queue->max_capacity_log2 < (uint64_t) max_capacity_or_negative_if_infinite)
            queue->max_capacity_log2 ++;

        queue->max_capacity_log2 ++;
    }

    atomic_store(&queue->block, NULL);
}

LAZY_QUEUE_API_INLINE void* _lazy_queue_slot(Lazy_Queue_Block* block, uint64_t i, isize item_size)
{
    uint64_t mapped = i & block->mask;
    uint8_t* data = (uint8_t*) (void*) (block + 1);
    return data + mapped*item_size;
}

LAZY_QUEUE_INLINE_NEVER
LAZY_QUEUE_API Lazy_Queue_Block* _lazy_queue_reserve(Lazy_Queue* queue, isize to_size)
{
    Lazy_Queue_Block* old_block = atomic_load(&queue->block);
    Lazy_Queue_Block* out_block = old_block;
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

        Lazy_Queue_Block* new_block = (Lazy_Queue_Block*) malloc(sizeof(Lazy_Queue_Block) + new_cap*item_size);
        if(new_block)
        {
            new_block->next = old_block;
            new_block->mask = new_cap - 1;

            if(old_block)
            {
                uint64_t t = atomic_load(&queue->top);
                uint64_t b = atomic_load(&queue->bot);
                for(uint64_t i = t; (int64_t) (i - b) < 0; i++) //i < b
                    memcpy(_lazy_queue_slot(new_block, i, item_size), _lazy_queue_slot(old_block, i, item_size), item_size);
            }

            atomic_store(&queue->block, new_block);
            out_block = new_block;
        }
        
    }

    return out_block;
}

LAZY_QUEUE_API void lazy_queue_reserve(Lazy_Queue* queue, isize to_size)
{
    _lazy_queue_reserve(queue, to_size);
}

LAZY_QUEUE_API_INLINE Lazy_Queue_Result lazy_queue_result_st_push(Lazy_Queue *q, const void* item, isize item_size)
{
    _LAZY_QUEUE_USE_ATOMICS;
    ASSERT(atomic_load_explicit(&q->item_size, memory_order_relaxed) == item_size);

    Lazy_Queue_Block *a = atomic_load_explicit(&q->block, memory_order_relaxed);
    uint64_t b = atomic_load_explicit(&q->bot, memory_order_relaxed);
    uint64_t t = q->estimate_top;

    if (a == NULL || (int64_t)(b - t) > (int64_t) a->mask) { 
        t = atomic_load_explicit(&q->top, memory_order_acquire);
        q->estimate_top = t;
        if (a == NULL || (int64_t)(b - t) > (int64_t) a->mask) { 
            Lazy_Queue_Block* new_a = _lazy_queue_reserve(q, b - t + 1);
            if(new_a == a)
            {
                Lazy_Queue_Result out = {b, t, LAZY_QUEUE_FULL};
                return out;
            }

            a = new_a;
        }
    }
    
    void* slot = _lazy_queue_slot(a, b, item_size);
    memcpy(slot, item, item_size);

    atomic_store_explicit(&q->bot, b + 1, memory_order_release);
    Lazy_Queue_Result out = {b, t, LAZY_QUEUE_OK};
    return out;
}

LAZY_QUEUE_API_INLINE Lazy_Queue_Result lazy_queue_result_st_pop(Lazy_Queue *q, void* item, isize item_size)
{
    _LAZY_QUEUE_USE_ATOMICS;
    ASSERT(atomic_load_explicit(&q->item_size, memory_order_relaxed) == item_size);
    uint64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    uint64_t b = atomic_load_explicit(&q->estimate_bot, memory_order_relaxed);
    
    Lazy_Queue_Result out = {b, t, LAZY_QUEUE_EMPTY};

    //if empty reload bot estimate
    if ((int64_t) (b - t) <= 0) {
        b = atomic_load_explicit(&q->bot, memory_order_relaxed);
        atomic_store_explicit(&q->estimate_bot, b, memory_order_relaxed);
        out.bot = b;
        if ((int64_t) (b - t) <= 0) 
            return out;
    }
    
    //seq cst because we must ensure we dont get updated t,b and old block! 
    // Then we would assume there are items to pop, copy over uninitialized memory from old block and succeed. (bad!)
    // For x86 the generated assembly is identical even if we replace it by memory_order_acquire.
    // For weak memory model architectures it wont be. 
    // If you dont like this you can instead store all of the fields of queue (top, estimate_bot, bot...)
    //  in the block header instead. That way it will be again impossible to get top, bot and old block.
    //  I dont bother with this as I primarily care about x86 and I find the code written like this be easier to read. 
    Lazy_Queue_Block *a = atomic_load_explicit(&q->block, memory_order_seq_cst);

    void* slot = _lazy_queue_slot(a, t, item_size);
    memcpy(item, slot, item_size);

    atomic_store_explicit(&q->top, t + 1, memory_order_relaxed);
    out.state = LAZY_QUEUE_OK;

    return out;
}

LAZY_QUEUE_API_INLINE Lazy_Queue_Result lazy_queue_result_pop_weak(Lazy_Queue *q, void* item, isize item_size)
{
    _LAZY_QUEUE_USE_ATOMICS;
    ASSERT(atomic_load_explicit(&q->item_size, memory_order_relaxed) == item_size);
    uint64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    uint64_t b = atomic_load_explicit(&q->estimate_bot, memory_order_relaxed);
    
    Lazy_Queue_Result out = {b, t, LAZY_QUEUE_EMPTY};

    //if empty reload bot estimate
    if ((int64_t) (t - b) >= 0) {
        b = atomic_load_explicit(&q->bot, memory_order_relaxed);
        atomic_store_explicit(&q->estimate_bot, b, memory_order_relaxed);
        out.bot = b;
        if ((int64_t) (t - b) >= 0) 
            return out;
    }
    
    Lazy_Queue_Block *a = atomic_load_explicit(&q->block, memory_order_seq_cst);

    void* slot = _lazy_queue_slot(a, t, item_size);
    memcpy(item, slot, item_size);

    if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1, memory_order_seq_cst, memory_order_relaxed))
        out.state = LAZY_QUEUE_FAILED_RACE;
    else
        out.state = LAZY_QUEUE_OK;

    return out;
}

LAZY_QUEUE_API_INLINE Lazy_Queue_Result lazy_queue_result_pop(Lazy_Queue *q, void* item, isize item_size)
{
    for(;;) {
        Lazy_Queue_Result result = lazy_queue_result_pop_weak(q, item, item_size);
        if(result.state != LAZY_QUEUE_FAILED_RACE)
            return result;
    }
}

LAZY_QUEUE_API_INLINE bool lazy_queue_st_push(Lazy_Queue *q, const void* item, isize item_size)
{
    return lazy_queue_result_st_push(q, item, item_size).state == LAZY_QUEUE_OK;
}

LAZY_QUEUE_API_INLINE bool lazy_queue_st_pop(Lazy_Queue *q, void* items, isize item_size)
{
    return lazy_queue_result_st_pop(q, items, item_size).state == LAZY_QUEUE_OK;
}

LAZY_QUEUE_API_INLINE bool lazy_queue_pop(Lazy_Queue *q, void* item, isize item_size)
{
    return lazy_queue_result_pop(q, item, item_size).state == LAZY_QUEUE_OK;
}

LAZY_QUEUE_API_INLINE isize lazy_queue_capacity(const Lazy_Queue *q)
{
    _LAZY_QUEUE_USE_ATOMICS;
    Lazy_Queue_Block *a = atomic_load_explicit(&q->block, memory_order_relaxed);
    return a ? (isize) a->mask + 1 : 0;
}

LAZY_QUEUE_API_INLINE isize lazy_queue_count(const Lazy_Queue *q)
{
    _LAZY_QUEUE_USE_ATOMICS;
    uint64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    uint64_t b = atomic_load_explicit(&q->bot, memory_order_relaxed);
    uint64_t diff = (isize) (b - t);
    return diff >= 0 ? diff : 0;
}

#endif