#pragma once

#include "lc_pool.h"

#include "_test_chase_lev_queue.h"

enum {TEST_MAX_THREADS = 64};

void test_lc_pool_sequential(isize count)
{
    LC_Pool pool = {0};
    lc_pool_init(&pool, sizeof(isize), TEST_MAX_THREADS);

    int32_t thread = lc_pool_thread_add(&pool);
    isize dummy = 0;    
    TEST(lc_pool_pop(&pool, thread, &dummy, sizeof(isize)) == false);
    TEST(lc_pool_pop(&pool, thread, &dummy, sizeof(isize)) == false);

    for(isize i = 0; i < count; i++)
        TEST(lc_pool_push(&pool, thread, &i, sizeof(isize)));

    //cycle some of them        
    for(isize i = 0; i < count/2; i++)
    {
        TEST(lc_pool_pop(&pool, thread, &dummy, sizeof(isize)));
        TEST(lc_pool_push(&pool, thread, &dummy, sizeof(isize)));
    }

    Test_CL_Buffer buffer = {0};
    for(isize i = 0; i < count; i++)
    {
        isize popped = 0;
        TEST(lc_pool_pop(&pool, thread, &popped, sizeof(isize)));
        test_cl_buffer_push(&buffer, &popped, 1);
    }
    
    TEST(lc_pool_pop(&pool, thread, &dummy, sizeof(isize)) == false);
    TEST(lc_pool_pop(&pool, thread, &dummy, sizeof(isize)) == false);

    qsort(buffer.data, buffer.count, sizeof(isize), test_cl_isize_comp_func);
    for(isize i = 0; i < (isize) count; i++)
        TEST(buffer.data[i] == i);

    lc_pool_deinit(&pool);
}

typedef struct Test_Pool_Thread {
    CL_QUEUE_ATOMIC(isize)* started; 
    CL_QUEUE_ATOMIC(isize)* finished; 
    CL_QUEUE_ATOMIC(isize)* run_test; 

    LC_Pool* pool_a;
    LC_Pool* pool_b;
    int32_t thread_a;
    int32_t thread_b;
    isize iters;

    double reverse_chance;
} Test_Pool_Thread;

static void test_lc_pool_ping_pong_thread_func(void *arg)
{
    Test_Pool_Thread* thread = (Test_Pool_Thread*) arg;
    atomic_fetch_add(thread->started, 1);

    //wait to run
    while(*thread->run_test == 0); 
    
    //run for as long as we can
    while(*thread->run_test == 1)
    {
        double random = (double) rand() / RAND_MAX;
        isize item = 0;
        if(random < thread->reverse_chance)
        {
            if(lc_pool_pop(thread->pool_a, thread->thread_a, &item, sizeof item))
                lc_pool_push(thread->pool_b, thread->thread_b, &item, sizeof item);
        }
        else
        {
            if(lc_pool_pop(thread->pool_b, thread->thread_b, &item, sizeof item))
                lc_pool_push(thread->pool_a, thread->thread_a, &item, sizeof item);
        }

        thread->iters += 1;
    }
    
    atomic_fetch_add(thread->finished, 1);
}

#include <Windows.h>

//#pragma comment(lib, "kernel32.lib")
static void test_lc_pool_ping_pong(isize item_count, isize a_count, isize b_count, double time, double reverse_chance)
{
    LC_Pool pool_a = {0};
    LC_Pool pool_b = {0};
    
    lc_pool_init(&pool_a, sizeof(isize), TEST_MAX_THREADS);
    lc_pool_init(&pool_b, sizeof(isize), TEST_MAX_THREADS);
    
    //prefill pools
    {
        uint32_t handle_a = lc_pool_thread_add(&pool_a);
        uint32_t handle_b = lc_pool_thread_add(&pool_b);
        for(isize i = 0; i < item_count; i++)
        {
            lc_pool_push(&pool_a, handle_a, &i, sizeof i);
            lc_pool_push(&pool_b, handle_b, &i, sizeof i);
        }
        lc_pool_thread_remove(&pool_a, handle_a);
        lc_pool_thread_remove(&pool_b, handle_b);
    }

    CL_QUEUE_ATOMIC(isize) started = 0;
    CL_QUEUE_ATOMIC(isize) finished = 0;
    CL_QUEUE_ATOMIC(isize) run_test = 0;
    
    //start all threads
    Test_Pool_Thread threads[TEST_MAX_THREADS] = {0};
    for(isize i = 0; i < a_count + b_count; i++)
    {
        threads[i].pool_a = &pool_a;
        threads[i].pool_b = &pool_b;
        threads[i].thread_a = lc_pool_thread_add(&pool_a);
        threads[i].thread_b = lc_pool_thread_add(&pool_b);
        threads[i].started = &started;
        threads[i].finished = &finished;
        threads[i].run_test = &run_test;
        threads[i].reverse_chance = i < a_count ? reverse_chance : 1 - reverse_chance;

        //run the test func in separate thread in detached state
        test_cl_launch_thread(test_lc_pool_ping_pong_thread_func, &threads[i]);
    }
    
    double actual_time = 0;
    //run test
    {
        while(started != a_count + b_count);
        run_test = 1;
        isize clocks_before = clock();
        
        //void __declspec(dllimport) __stdcall Sleep(unsigned long dwMilliseconds);
        Sleep((unsigned long) (time*1000));

        run_test = 2;
        isize clocks_after = clock();
        while(finished != a_count + b_count);

        //sleep is very inacurrate on windows so we use the time the test really took
        actual_time = (double)(clocks_after - clocks_before)/CLOCKS_PER_SEC;
    }

    //pop all remaining items
    Test_CL_Buffer buffer = {0};
    uint32_t handle_a = lc_pool_thread_add(&pool_a);
    uint32_t handle_b = lc_pool_thread_add(&pool_b);
    {
        isize popped = 0;
        while(lc_pool_pop(&pool_a, handle_a, &popped, sizeof popped))
            test_cl_buffer_push(&buffer, &popped, 1);
            
        while(lc_pool_pop(&pool_b, handle_a, &popped, sizeof popped))
            test_cl_buffer_push(&buffer, &popped, 1);
    }
    
    TEST(buffer.count == 2*item_count);
        
    //test if items are valid
    qsort(buffer.data, buffer.count, sizeof(isize), test_cl_isize_comp_func);
    for(isize i = 0; i < item_count; i ++)
    {
        TEST(buffer.data[2*i] == i);
        TEST(buffer.data[2*i+1] == i);
    }
    
    isize total_iters = 0;
    for(isize i = 0; i < a_count + b_count; i++)
        total_iters += threads[i].iters;

    printf("a:%lli b:%lli total:%lli throughput:%.2lf millions/s\n", a_count, b_count, total_iters, (double) total_iters/(actual_time*1e6));
    
    free(buffer.data);
    lc_pool_deinit(&pool_a);
    lc_pool_deinit(&pool_b);
}


void test_lc_pool_stress(double time, isize max_threads) 
{
    for(isize deadline = clock() + (isize) (time*CLOCKS_PER_SEC);;)
    {
        isize now = clock();
        if(now >= deadline)
            break;

        isize max_single_test = (isize) (0.1*CLOCKS_PER_SEC);
        isize single_test_clocks = rand() % max_single_test;
        if(single_test_clocks > deadline - now)
            single_test_clocks = deadline - now;

        double single_test = (double) single_test_clocks / CLOCKS_PER_SEC;
        isize threads_a = rand() % (max_threads/2);
        isize threads_b = rand() % ((max_threads + 1)/2);
        isize items = rand() % 10000;
        double reverse_chance = (double) rand() / CLOCKS_PER_SEC / 10;

        test_lc_pool_ping_pong(items, threads_a, threads_b, single_test, reverse_chance);
    }
}

void test_lc_pool(double time, isize max_threads) 
{
    test_lc_pool_sequential(1);
    test_lc_pool_sequential(10);
    test_lc_pool_sequential(100);
    test_lc_pool_sequential(1000);
    
    test_lc_pool_stress(time, max_threads);
}


typedef struct Bench_Pool_Thread {
    CL_QUEUE_ATOMIC(isize)* started; 
    CL_QUEUE_ATOMIC(isize)* finished; 
    CL_QUEUE_ATOMIC(isize)* run_test; 
    CL_QUEUE_ATOMIC(uint64_t)* target; 

    LC_Pool* pool;
    int32_t thread;
    uint64_t iters;
    uint64_t ops;
    isize reserve_to;
    
    LC_Pool* pool_a;
    LC_Pool* pool_b;
    int32_t thread_a;
    int32_t thread_b;

    uint64_t user;
    bool is_push;
} Bench_Pool_Thread;

typedef struct Bench_Pool_Result {
    double time;
    uint64_t tries;
    uint64_t ops;
    uint64_t capacity_max;
    uint64_t capacity_sum;
    isize a_count;
    isize b_count;
    isize repeats;
} Bench_Pool_Result;

enum {
    BENCH_LC_POOL_FAA,
    BENCH_LC_POOL_CAS,
    BENCH_LC_POOL_HALF_FAA,
    BENCH_LC_POOL_HALF_CAS,
};

static void bench_lc_pool_faa_thread_func(void *arg)
{
    Bench_Pool_Thread* thread = (Bench_Pool_Thread*) arg;
    atomic_fetch_add(thread->started, 1);

    //wait to run
    while(*thread->run_test == 0); 
    
    uint64_t iters = 0;
    uint64_t ops = 0;
    CL_QUEUE_ATOMIC(uint64_t)* target = thread->target;
    CL_QUEUE_ATOMIC(isize)* run_test = thread->run_test;
    CL_QUEUE_ATOMIC(uint32_t)* target_lo = (CL_QUEUE_ATOMIC(uint32_t)*) (void*) thread->target;
    CL_QUEUE_ATOMIC(uint32_t)* target_hi = target_lo + 1;

    if(thread->user == BENCH_LC_POOL_FAA)
    {
        while(atomic_load_explicit(run_test, memory_order_relaxed) == 1)
        {
            atomic_fetch_add(target, 1);
            iters += 1;
            ops += 1;
        }
    }
    if(thread->user == BENCH_LC_POOL_CAS)
    {
        uint64_t my_value = (uint64_t) thread->thread;
        while(atomic_load_explicit(run_test, memory_order_relaxed) == 1)
        {
            uint64_t curr = atomic_load_explicit(target, memory_order_relaxed);
            ops += atomic_compare_exchange_strong(target, &curr, my_value);
            iters += 1;
        }
    }
    if(thread->user == BENCH_LC_POOL_HALF_FAA)
    {
        while(atomic_load_explicit(run_test, memory_order_relaxed) == 1)
        {
            atomic_fetch_add(target_lo, 1);
            atomic_fetch_add(target_hi, 1);
            iters += 2;
            ops += 2;
        }
    }
    if(thread->user == BENCH_LC_POOL_HALF_CAS)
    {
        uint32_t my_value = (uint32_t) thread->thread;
        while(atomic_load_explicit(run_test, memory_order_relaxed) == 1)
        {
            uint32_t curr_lo = atomic_load_explicit(target_lo, memory_order_relaxed);
            ops += atomic_compare_exchange_strong(target_lo, &curr_lo, my_value);
            
            uint32_t curr_hi = atomic_load_explicit(target_hi, memory_order_relaxed);
            ops += atomic_compare_exchange_strong(target_hi, &curr_hi, my_value);
            iters += 2;
        }
    }
    
    thread->iters = iters;
    thread->ops = ops;
    atomic_fetch_add(thread->finished, 1);
}

static void bench_lc_pool_50_50_thread_func(void *arg)
{
    Bench_Pool_Thread* thread = (Bench_Pool_Thread*) arg;
    atomic_fetch_add(thread->started, 1);

    //wait to run
    while(*thread->run_test == 0); 
    
    //number with 32 on bits and 32 off bits. 
    //We use this to generate a semi random sequence of push pop
    uint64_t random_mask = 0xE0349F24ABC58B2F;
    int32_t handle = thread->thread;
    LC_Pool* pool = thread->pool;
    uint64_t iters = 0;
    uint64_t ops = 0;

    //run for as long as we can
    while(atomic_load_explicit(thread->run_test, memory_order_relaxed) == 1)
    {
        uint64_t bit_i = ((uint64_t) handle + iters) % 64;

        isize item = 0;
        if(random_mask & ((uint64_t) 1 << bit_i))
            ops += lc_pool_push(pool, handle, &item, sizeof item);
        else
            ops += lc_pool_pop(pool, handle, &item, sizeof item);

        iters += 1;
    }
    
    thread->iters = iters;
    thread->ops = ops;
    atomic_fetch_add(thread->finished, 1);
}

static void bench_lc_pool_asymetric_thread_func(void *arg)
{
    Bench_Pool_Thread* thread = (Bench_Pool_Thread*) arg;
    isize index = atomic_fetch_add(thread->started, 1);
    srand((unsigned) index);

    //wait to run
    while(*thread->run_test == 0); 
    
    int32_t handle = thread->thread;
    LC_Pool* pool = thread->pool;
    uint64_t iters = 0;
    uint64_t ops = 0;

    //run for as long as we can
    if(thread->is_push)
    {
        isize item = 0;
        for(; atomic_load_explicit(thread->run_test, memory_order_relaxed) == 1;)
            lc_pool_push(pool, handle, &item, sizeof item);
    }
    else
    {
        isize item = 0;
        for(; atomic_load_explicit(thread->run_test, memory_order_relaxed) == 1; iters += 1)
        {
            ops += lc_pool_pop(pool, handle, &item, sizeof item);

            for(int i = 0; i < 30; i++)
                _mm_pause();
        }
            //ops += lc_pool_pop_others_from(pool, rand(), &item, sizeof item);
    }
    
    thread->iters = iters;
    thread->ops = ops;
    atomic_fetch_add(thread->finished, 1);
}

static void bench_lc_pool_ping_pong_thread_func(void *arg)
{
    Bench_Pool_Thread* thread = (Bench_Pool_Thread*) arg;
    isize index = atomic_fetch_add(thread->started, 1);
    srand((unsigned) index);

    //wait to run
    while(*thread->run_test == 0); 
    
    int32_t handle_a = thread->thread_a;
    int32_t handle_b = thread->thread_b;
    LC_Pool* pool_a = thread->pool_a;
    LC_Pool* pool_b = thread->pool_b;
    uint64_t iters = 0;
    uint64_t ops = 0;
    if(thread->is_push)
    {
        for(; atomic_load_explicit(thread->run_test, memory_order_relaxed) == 1; iters += 2) {
            isize item = 0;
            ops += lc_pool_pop(pool_a, handle_a, &item, sizeof item);
            //ops += lc_pool_pop_others_from(pool_a, rand(), &item, sizeof item);
            ops += lc_pool_push(pool_b, handle_b, &item, sizeof item);
        }
    }
    else
    {
        for(; atomic_load_explicit(thread->run_test, memory_order_relaxed) == 1; iters += 2) {
            isize item = 0;
            ops += lc_pool_pop(pool_b, handle_b, &item, sizeof item);
            //ops += lc_pool_pop_others_from(pool_b, rand(), &item, sizeof item);
            ops += lc_pool_push(pool_a, handle_a, &item, sizeof item);
        }
    }
    
    thread->iters = iters;
    thread->ops = ops;
    atomic_fetch_add(thread->finished, 1);
}

//#pragma comment(lib, "kernel32.lib")
static Bench_Pool_Result bench_lc_pool_single(uint64_t user, bool double_sided, isize item_count, isize a_count, isize b_count, double time, void (*func)(void*))
{
    LC_Pool pool_a = {0};
    LC_Pool pool_b = {0};
    lc_pool_init(&pool_a, sizeof(isize), TEST_MAX_THREADS);
    lc_pool_init(&pool_b, sizeof(isize), TEST_MAX_THREADS);

    CL_QUEUE_ATOMIC(isize) started = 0;
    CL_QUEUE_ATOMIC(isize) finished = 0;
    CL_QUEUE_ATOMIC(isize) run_test = 0;
    CL_QUEUE_ATOMIC(uint64_t) target = 0;
    
    //start all threads
    Bench_Pool_Thread threads[TEST_MAX_THREADS] = {0};
    for(isize i = 0; i < a_count + b_count; i++)
    {
        int32_t handle_a = 0;
        int32_t handle_b = 0;

        if(func == bench_lc_pool_ping_pong_thread_func)
        {
            //if(i < a_count)
            {
                handle_a = lc_pool_thread_add(&pool_a);
                lc_pool_reserve(&pool_a, handle_a, item_count, sizeof(isize));
            }
            //else
            {
                handle_b = lc_pool_thread_add(&pool_b);
                lc_pool_reserve(&pool_b, handle_b, item_count, sizeof(isize));
            }
        }
        else if(func == bench_lc_pool_asymetric_thread_func)
        {
            //if(i < a_count)
            {
                handle_a = lc_pool_thread_add(&pool_a);
                lc_pool_reserve(&pool_a, handle_a, item_count, sizeof(isize));
            }
        }
        else
        {
            handle_a = lc_pool_thread_add(&pool_a);
            handle_b = lc_pool_thread_add(&pool_b);
            lc_pool_reserve(&pool_a, handle_a, item_count, sizeof(isize));
            lc_pool_reserve(&pool_b, handle_b, item_count, sizeof(isize));
        }
        
        threads[i].started = &started;
        threads[i].finished = &finished;
        threads[i].run_test = &run_test;
        threads[i].target = &target;
        threads[i].is_push = i < a_count;
        threads[i].pool = &pool_a;
        threads[i].thread = handle_a;
        threads[i].user = user;

        threads[i].pool_a = &pool_a;
        threads[i].pool_b = &pool_b;
        threads[i].thread_a = handle_a;
        threads[i].thread_b = handle_b;

        test_cl_launch_thread(func, &threads[i]);
    }
    
    double actual_time = 0;
    //run test
    {
        while(started != a_count + b_count);
        run_test = 1;
        isize clocks_before = clock();
        
        //void __declspec(dllimport) __stdcall Sleep(unsigned long dwMilliseconds);
        Sleep((unsigned long) (time*1000));

        run_test = 2;
        isize clocks_after = clock();
        while(finished != a_count + b_count);

        //sleep is very inacurrate on windows so we use the time the test really took
        actual_time = (double)(clocks_after - clocks_before)/CLOCKS_PER_SEC;
    }
    
    Bench_Pool_Result result = {0};
    result.a_count = a_count;
    result.b_count = b_count;
    result.time = actual_time;
    result.repeats = 1;
    for(isize i = 0; i < a_count + b_count; i++) {
        int32_t handle_a = threads[i].thread_a;
        int32_t handle_b = threads[i].thread_b;
        isize capacity_a = cl_queue_capacity(&pool_a.threads[handle_a].queue);
        isize capacity_b = cl_queue_capacity(&pool_a.threads[handle_b].queue);

        result.tries += threads[i].iters;
        result.ops += threads[i].ops;
        result.capacity_sum += capacity_a + capacity_b;

        if(result.capacity_max < (uint64_t) capacity_a)
            result.capacity_max = (uint64_t) capacity_a;
        if(result.capacity_max < (uint64_t) capacity_b)
            result.capacity_max = (uint64_t) capacity_b;
    }

    lc_pool_deinit(&pool_a);
    lc_pool_deinit(&pool_b);
    return result;
}

static Bench_Pool_Result bench_lc_pool_repeated(uint64_t user, bool double_sided, isize item_count, isize a_count, isize b_count, double total_time, isize repeats, void (*func)(void*))
{
    double time = total_time / repeats;
    Bench_Pool_Result sum = {0}; 
    sum.repeats = repeats;
    sum.a_count = a_count;
    sum.b_count = b_count;
    for(isize i = 0; i < repeats; i++)
    {
        Bench_Pool_Result res = bench_lc_pool_single(user, double_sided, item_count, a_count, b_count, time, func);
        sum.time += res.time;
        sum.ops += res.ops;
        sum.tries += res.tries;

        if(sum.capacity_sum < res.capacity_sum)
            sum.capacity_sum = res.capacity_sum;
        if(sum.capacity_max < res.capacity_max)
            sum.capacity_max = res.capacity_max;
    }

    return sum;
}

void bench_lc_pool(double time, isize max_threads) 
{
    isize reserve_count = 1024*1024*2;
    isize repeats = 10;
    
    if(0)
    for(isize i = 2; i <= max_threads; i++)
    {
        Bench_Pool_Result res = bench_lc_pool_repeated(0, true, reserve_count, i/2, (i + 1)/2, time, repeats, bench_lc_pool_ping_pong_thread_func);
        printf("ping/pong: threads:%2lli throughput:%7.2lf millions/s total:%10lli (%4.2lf success rate) ", i, (double) res.ops/(res.time*1e6), res.ops, (double)res.ops/res.tries);

        if(reserve_count == res.capacity_max)
            printf("\n");
        else
            printf(" reserved:%lli MB max_capacity:%lli MB \n", reserve_count/(1024*1024), res.capacity_max/(1024*1024));
    }

    if(0)
    for(isize i = 2; i <= max_threads; i++)
    {
        Bench_Pool_Result res = bench_lc_pool_repeated(0, false, reserve_count, i/2, (i + 1)/2, time, repeats, bench_lc_pool_50_50_thread_func);
        printf("50/50: threads:%2lli throughput:%7.2lf millions/s total:%10lli (%4.2lf success rate) ", i, (double) res.ops/(res.time*1e6), res.ops, (double)res.ops/res.tries);

        if(reserve_count == res.capacity_max)
            printf("\n");
        else
            printf(" reserved:%lli MB max_capacity:%lli MB \n", reserve_count/(1024*1024), res.capacity_max/(1024*1024));
    }
    
    reserve_count = 1024*1024*16;
    if(0)
    for(isize i = 1; i < max_threads; i++)
    {
        Bench_Pool_Result res = bench_lc_pool_repeated(0, false, reserve_count, i, 1, time, repeats, bench_lc_pool_asymetric_thread_func);
        printf("N push 1 pop: threads:%2lli throughput:%7.2lf millions/s total:%10lli (%4.2lf success rate)", i+1, (double) res.ops/(res.time*1e6), res.ops, (double)res.ops/res.tries);
        
        if(reserve_count == res.capacity_max)
            printf("\n");
        else
            printf(" reserved:%lli MB max_capacity:%lli MB \n", reserve_count/(1024*1024), res.capacity_max/(1024*1024));
    }
    
    //if(0)
    for(isize i = 1; i < max_threads; i++)
    {
        Bench_Pool_Result res = bench_lc_pool_repeated(0, false, reserve_count, 1, i, time, repeats, bench_lc_pool_asymetric_thread_func);
        printf("1 push N pop: threads:%2lli throughput:%7.2lf millions/s total:%10lli (%4.2lf success rate)", i+1, (double) res.ops/(res.time*1e6), res.ops, (double)res.ops/res.tries);
        
        if(reserve_count == res.capacity_max)
            printf("\n");
        else
            printf(" reserved:%lli MB max_capacity:%lli MB \n", reserve_count/(1024*1024), res.capacity_max/(1024*1024));
    }
    
    //if(0)
    for(isize i = 1; i <= max_threads; i++)
    {
        Bench_Pool_Result res = bench_lc_pool_repeated(BENCH_LC_POOL_FAA, false, 0, i, 0, time, repeats, bench_lc_pool_faa_thread_func);
        printf("FAA: threads:%2lli throughput:%7.2lf millions/s total:%10lli (%4.2lf success rate) \n", i, (double) res.ops/(res.time*1e6), res.ops, (double)res.ops/res.tries);
    }
    
    //if(0)
    for(isize i = 1; i <= max_threads; i++)
    {
        Bench_Pool_Result res = bench_lc_pool_repeated(BENCH_LC_POOL_CAS, false, 0, i, 0, time, repeats, bench_lc_pool_faa_thread_func);
        printf("CAS: threads:%2lli throughput:%7.2lf millions/s total:%10lli (%4.2lf success rate) \n", i, (double) res.ops/(res.time*1e6), res.ops, (double)res.ops/res.tries);
    }
    
    //if(0)
    for(isize i = 1; i <= max_threads; i++)
    {
        Bench_Pool_Result res = bench_lc_pool_repeated(BENCH_LC_POOL_HALF_FAA, false, 0, i, 0, time, repeats, bench_lc_pool_faa_thread_func);
        printf("half FAA: threads:%2lli throughput:%7.2lf millions/s total:%10lli (%4.2lf success rate) \n", i, (double) res.ops/(res.time*1e6), res.ops, (double)res.ops/res.tries);
    }
    
    //if(0)
    for(isize i = 1; i <= max_threads; i++)
    {
        Bench_Pool_Result res = bench_lc_pool_repeated(BENCH_LC_POOL_HALF_CAS, false, 0, i, 0, time, repeats, bench_lc_pool_faa_thread_func);
        printf("half CAS: threads:%2lli throughput:%7.2lf millions/s total:%10lli (%4.2lf success rate) \n", i, (double) res.ops/(res.time*1e6), res.ops, (double)res.ops/res.tries);
    }
}