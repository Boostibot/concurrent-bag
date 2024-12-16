#pragma once

#ifndef __cplusplus
    #include <stdatomic.h>
#endif
#include "chase_lev_queue.h"

typedef struct LC_Pool LC_Pool;

typedef struct LC_Pool_Thread {
    alignas(64)
    CL_Queue queue;
    isize stealing_from;

    bool pushed;
    //upon the call to lc_pool_thread_remove is set to true.
    // This has no effect on any logic in push/pop operations
    // but lc_pool_thread_add can reuse this thread
    CL_QUEUE_ATOMIC(bool) removed; 
} LC_Pool_Thread;

typedef struct LC_Pool {
    LC_Pool_Thread* threads;
    int32_t threads_capacity; 
    CL_QUEUE_ATOMIC(int32_t) threads_count;
    
    union {
        struct {
            CL_QUEUE_ATOMIC(uint32_t) threads_added; 
            CL_QUEUE_ATOMIC(uint32_t) threads_removed; 
        };
        CL_QUEUE_ATOMIC(uint64_t) threads_added_removed;
    };

    isize max_capacity;
    isize initial_capacity;
    isize item_size;

    CL_QUEUE_ATOMIC(uint64_t) alive_mask;
    CL_QUEUE_ATOMIC(uint64_t) pusher_empty_mask; //top 32 bits are pusher bot 32 bits are empty mask
} LC_Pool;

void lc_pool_init(LC_Pool* pool, isize item_size, isize thread_capacity);
void lc_pool_deinit(LC_Pool* pool);

//returns -1 if we used up all thread_capacity from lc_pool_init
int32_t lc_pool_thread_add(LC_Pool* pool);
void lc_pool_thread_remove(LC_Pool* pool, int32_t thread);

CL_QUEUE_API_INLINE void lc_pool_reserve(LC_Pool* pool, int32_t thread, isize to_size, isize item_size);
CL_QUEUE_API_INLINE bool lc_pool_push(LC_Pool* pool, int32_t thread, const void* data, isize item_size);
CL_QUEUE_API_INLINE bool lc_pool_pop(LC_Pool* pool, int32_t thread, void* data, isize item_size);
CL_QUEUE_API_INLINE bool lc_pool_pop_self(LC_Pool* pool, int32_t thread, void* data, isize item_size);
CL_QUEUE_API_INLINE bool lc_pool_pop_others(LC_Pool* pool, int32_t thread, void* data, isize item_size);
CL_QUEUE_API_INLINE bool lc_pool_pop_others_from(LC_Pool* pool, isize steal_base, void* data, isize item_size);

CL_QUEUE_API_INLINE bool lc_pool_push(LC_Pool* pool, int32_t thread, const void* data, isize item_size)
{
    pool->threads[thread].pushed = true;
    return cl_queue_push(&pool->threads[thread].queue, data, item_size);
}

CL_QUEUE_API_INLINE bool lc_pool_pop_self(LC_Pool* pool, int32_t thread, void* data, isize item_size)
{
    return cl_queue_pop_back(&pool->threads[thread].queue, data, item_size);
}

CL_QUEUE_API_INLINE bool lc_pool_pop_others_old(LC_Pool* pool, int32_t thread, void* data, isize item_size)
{
    //todo make dynamic
    enum {MAX_THREADS = 128};
    uint64_t bots[MAX_THREADS]; //1024 B

    LC_Pool_Thread* self = &pool->threads[thread];
    isize steal_base = self->stealing_from;

    isize threads_count = atomic_load_explicit(&pool->threads_count, memory_order_relaxed);
    for(isize round = 0; round < 2; round++) {
        isize steal = steal_base;
        for(isize k = 0; k < threads_count; k++)
        {
            //increment the steal from thread. We could also use modulo above but
            // we strive to make the happy path as low latency as possible.
            steal += 1;
            if(steal >= threads_count)
                steal = 0;

            //dont steal from self
            if(steal != thread)
            {
                LC_Pool_Thread* steal_thread = &pool->threads[steal];
                CL_Queue_Result result = cl_queue_result_pop(&steal_thread->queue, data, item_size);
                if(result.state == CL_QUEUE_OK) {
                
                    self->stealing_from = steal;
                    return true;
                }

                uint64_t ticket = result.bot + atomic_load_explicit(&steal_thread->queue.bot_ticket, memory_order_relaxed);

                //if is my first time around save the position of bot
                if(round == 0)
                    bots[steal] = ticket;
                //... so that the second round I can detect if someone else pushed 
                // and if they did we have to go start the search anew.
                // Yes, this is necessary to stay linearizable.
                else if(bots[steal] != ticket)
                {
                    round = -1;
                    break;
                }
            }
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

    return false;
}

CL_QUEUE_API_INLINE int32_t _lc_pool_pop_others_from(LC_Pool* pool, isize steal_base, int32_t thread, bool filter_thread, void* data, isize item_size)
{
    //todo make dynamic
    enum {MAX_THREADS = 128};
    uint64_t bots[MAX_THREADS]; //1024 B

    isize threads_count = atomic_load_explicit(&pool->threads_count, memory_order_relaxed);
    for(isize round = 0; round < 2; round++) {
        isize steal = filter_thread ? steal_base : steal_base % threads_count;
        for(isize k = 0; k < threads_count; k++)
        {
            //increment the steal from thread. We could also use modulo above but
            // we strive to make the happy path as low latency as possible.
            steal += 1;
            if(steal >= threads_count)
                steal = 0;

            //dont steal from self
            if(filter_thread == false || steal != thread)
            {
                LC_Pool_Thread* steal_thread = &pool->threads[steal];
                CL_Queue_Result result = cl_queue_result_pop(&steal_thread->queue, data, item_size);
                if(result.state == CL_QUEUE_OK) {
                    return (int32_t) steal;
                }

                uint64_t ticket = result.bot + atomic_load_explicit(&steal_thread->queue.bot_ticket, memory_order_relaxed);

                //if is my first time around save the position of bot
                if(round == 0)
                    bots[steal] = ticket;
                //... so that the second round I can detect if someone else pushed 
                // and if they did we have to go start the search anew.
                // Yes, this is necessary to stay linearizable.
                else if(bots[steal] != ticket)
                {
                    round = -1;
                    break;
                }
            }
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

    return -1;
}


CL_QUEUE_API_INLINE bool lc_pool_pop_others(LC_Pool* pool, int32_t thread, void* data, isize item_size)
{
    LC_Pool_Thread* self = &pool->threads[thread];
    isize steal_base = self->stealing_from;
    int32_t finished = _lc_pool_pop_others_from(pool, steal_base, thread, true, data, item_size);
    if(finished == -1)
        return false;

    self->stealing_from = finished;
    return true;
}

CL_QUEUE_API_INLINE bool lc_pool_pop_others_from(LC_Pool* pool, isize steal_base, void* data, isize item_size)
{
    return _lc_pool_pop_others_from(pool, steal_base, 0, false, data, item_size) != -1;
}

CL_QUEUE_API_INLINE bool lc_pool_pop(LC_Pool* pool, int32_t thread, void* data, isize item_size)
{
    if(pool->threads[thread].pushed) {
        if(lc_pool_pop_self(pool, thread, data, item_size))
            return true;
        else
            pool->threads[thread].pushed = false;
    }

    return lc_pool_pop_others(pool, thread, data, item_size);
}

#if 0
static inline uint32_t _lc_pool_find_rotate_left32(uint32_t x, uint32_t bits)
{
    return (x << bits) | (x >> (32 - bits));
}

static inline uint32_t _lc_pool_find_rotate_right32(uint32_t x, uint32_t bits)
{
    return (x >> bits) | (x << (32 - bits));
}

static int32_t _lc_pool_find_last_set_bit64(uint64_t num);
static int32_t _lc_pool_find_first_set_bit64(uint64_t num);

CL_QUEUE_API_INLINE bool lc_pool_pop_others_new(LC_Pool* pool, int32_t thread, void* data, isize item_size)
{
    //todo make dynamic
    enum {MAX_THREADS = 128};
    uint64_t bots[MAX_THREADS]; //1024 B

    LC_Pool_Thread* self = &pool->threads[thread];
    uint32_t steal_base = (uint32_t) self->stealing_from;
    
    uint64_t pusher_empty_mask = atomic_load_explicit(&pool->pusher_empty_mask, memory_order_relaxed);
    uint32_t empty_mask = (pusher_empty_mask & 0xffffffffu);
    uint32_t rot_empty_mask = _lc_pool_find_rotate_right32(empty_mask, steal_base);

    isize threads_count = atomic_load_explicit(&pool->threads_count, memory_order_relaxed);
    for(isize round = 0; round < 2; round++) {

        uint32_t prev_pos = 0;
        for(;;)
        {
            uint64_t mask_so_far = steal_base >> prev_pos;
            if(mask_so_far == 0)
                break;

            uint32_t first_bit = (uint32_t) _lc_pool_find_first_set_bit64(mask_so_far);
            uint32_t pos = (first_bit + steal_base) % 32 + prev_pos;
            prev_pos = first_bit + 1;

            //dont steal from self
            if(pos != thread)
            {
                LC_Pool_Thread* steal_thread = &pool->threads[pos];
                CL_Queue_Result result = cl_queue_result_pop(&steal_thread->queue, data, item_size);
                if(result.state == CL_QUEUE_OK) {
                
                    self->stealing_from = pos;
                    return true;
                }

                uint64_t ticket = result.bot + atomic_load_explicit(&steal_thread->queue.bot_ticket, memory_order_relaxed);

                //if is my first time around save the position of bot
                if(round == 0)
                    bots[pos] = ticket;
                //... so that the second round I can detect if someone else pushed 
                // and if they did we have to go start the search anew.
                // Yes, this is necessary to stay linearizable.
                else if(bots[pos] != ticket)
                {
                    round = -1;
                    break;
                }
            }


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

    return false;
}
#endif

CL_QUEUE_API_INLINE void lc_pool_reserve(LC_Pool* pool, int32_t thread, isize to_size, isize item_size)
{
    (void) item_size;
    cl_queue_reserve(&pool->threads[thread].queue, to_size);
}

void lc_pool_init(LC_Pool* pool, isize item_size, isize thread_capacity)
{
    //todo asserts
    lc_pool_deinit(pool);
    memset(pool, 0, sizeof *pool);
    pool->item_size = item_size;

    pool->threads = (LC_Pool_Thread*) calloc(thread_capacity, sizeof(LC_Pool_Thread));
    pool->threads_capacity = (int32_t) thread_capacity;

    atomic_store(&pool->threads_count, 0);
}

void lc_pool_deinit(LC_Pool* pool)
{
    isize threads_count = pool->threads_count;
    for(isize i = 0; i < threads_count; i++)
        cl_queue_deinit(&pool->threads[i].queue);
    
    free(pool->threads);
    memset(pool, 0, sizeof *pool);
    atomic_store(&pool->threads_count, 0);
}

int32_t lc_pool_thread_add(LC_Pool* pool)
{
    //todo asserts
    int32_t threads_count = atomic_load(&pool->threads_count);
    LC_Pool_Thread* threads = pool->threads;

    //try to look for removed slot
    int32_t thread = -1;
    for(int32_t i = 0; i < threads_count; i++)
    {
        if(threads[i].removed)
        {
            bool old_val = true;
            if(atomic_compare_exchange_strong(&threads[i].removed, &old_val, false))
            {
                thread = i;
                break;
            }
        }
    }

    //if we havent found anything add a new one
    if(thread == -1)
    {
        for(;;) {
            threads_count = atomic_load(&pool->threads_count);
            if(threads_count == pool->threads_capacity)
                break;

            if(atomic_compare_exchange_weak(&pool->threads_count, &threads_count, threads_count + 1))
            {
                thread = threads_count;
                cl_queue_init(&threads[thread].queue, pool->item_size, -1);
                threads[thread].stealing_from = thread;
                break;
            }
        }    
    }
    
    return thread;
}

void lc_pool_thread_remove(LC_Pool* pool, int32_t thread)
{
    //todo asserts
    atomic_store(&pool->threads[thread].removed, true);
}


#if defined(_MSC_VER)
    #include <intrin.h>
    static int32_t _lc_pool_find_last_set_bit64(uint64_t num)
    {
        ASSERT(num != 0);
        unsigned long out = 0;
        _BitScanReverse64(&out, (unsigned long long) num);
        return (int32_t) out;
    }
    
    static int32_t _lc_pool_find_first_set_bit64(uint64_t num)
    {
        ASSERT(num != 0);
        unsigned long out = 0;
        _BitScanForward64(&out, (unsigned long long) num);
        return (int32_t) out;
    }
    
#elif defined(__GNUC__) || defined(__clang__)
    static int32_t _lc_pool_find_last_set_bit64(uint64_t num)
    {
        ASSERT(num != 0);
        return 64 - __builtin_clzll((unsigned long long) num) - 1;
    }
    
    static int32_t _lc_pool_find_first_set_bit64(uint64_t num)
    {
        ASSERT(num != 0);
        return __builtin_ffsll((long long) num) - 1;
    }

#else
    #error unsupported compiler!
#endif