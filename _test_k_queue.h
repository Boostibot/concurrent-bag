#pragma once

#include "k_queue.h"

#include "_test_chase_lev_queue.h"

enum {TEST_MAX_THREADS = 64};

typedef struct Isize_Gen {
    isize val;
    uint64_t gen;
} Isize_Gen;

void test_k_queue_sequential(isize count, isize chunk_size)
{
    K_Queue pool = {0};
    k_queue_init(&pool, 0, sizeof(isize));
    isize k = chunk_size;

    K_Queue_Local local_pop = k_queue_local(&pool);
    K_Queue_Local local_push = k_queue_local(&pool);

    isize dummy = 0;    
    TEST(k_queue_pop(&pool, &local_pop, &dummy, sizeof(isize), k) == false);
    TEST(k_queue_pop(&pool, &local_pop, &dummy, sizeof(isize), k) == false);

    for(isize i = 0; i < count; i++)
        TEST(k_queue_push(&pool, &local_push, &i, sizeof(isize)));

    //cycle some of them        
    for(isize i = 0; i < count/2; i++)
    {
        TEST(k_queue_pop(&pool, &local_pop, &dummy, sizeof(isize), k));
        TEST(k_queue_push(&pool, &local_push, &dummy, sizeof(isize)));
    }

    Test_CL_Buffer buffer = {0};
    for(isize i = 0; i < count; i++)
    {
        isize popped = 0;
        TEST(k_queue_pop(&pool, &local_pop, &popped, sizeof(isize), k));
        test_cl_buffer_push(&buffer, &popped, 1);
    }
    
    TEST(k_queue_pop(&pool, &local_pop, &dummy, sizeof(isize), k) == false);
    TEST(k_queue_pop(&pool, &local_pop, &dummy, sizeof(isize), k) == false);

    qsort(buffer.data, buffer.count, sizeof(isize), test_cl_isize_comp_func);
    for(isize i = 0; i < (isize) count; i++)
        TEST(buffer.data[i] == i);

    test_cl_buffer_deinit(&buffer);
    k_queue_deinit(&pool);
}

typedef struct Test_K_Queue_Thread {
    ATOMIC(isize)* started; 
    ATOMIC(isize)* finished; 
    ATOMIC(isize)* run_test; 
    K_Queue* queue;

    Test_CL_Buffer buffer;
    isize chunk_size;
    isize reloads;
    isize ops;
    isize tries;
} Test_K_Queue_Thread;

static void test_k_queue_thread_func(void *arg)
{
    Test_K_Queue_Thread* thread = (Test_K_Queue_Thread*) arg;
    K_Queue_Local local = k_queue_local(thread->queue);

    atomic_fetch_add(thread->started, 1);
    while(*thread->run_test == 0); 
    
    while(atomic_load_explicit(thread->run_test, memory_order_relaxed) == 1)
    {
        isize val = 0;
        local.rand = rand();
        if(k_queue_pop(thread->queue, &local, &val, sizeof(isize), thread->chunk_size))
        {
            thread->ops += 1;
            test_cl_buffer_push(&thread->buffer, &val, 1);
        }

        thread->tries += 1;
    }

    thread->reloads = local.reloads;
    atomic_fetch_add(thread->finished, 1);
}

static void bench_k_queue_thread_func(void *arg)
{
    Test_K_Queue_Thread* thread = (Test_K_Queue_Thread*) arg;
    K_Queue_Local local = k_queue_local(thread->queue);

    atomic_fetch_add(thread->started, 1);
    while(*thread->run_test == 0); 
    
    while(atomic_load_explicit(thread->run_test, memory_order_relaxed) == 1)
    {
        isize val = 0;
        local.rand = rand();
        thread->ops += k_queue_pop(thread->queue, &local, &val, sizeof(isize), thread->chunk_size);
        thread->tries += 1;
    }
    
    thread->reloads = local.reloads;
    atomic_fetch_add(thread->finished, 1);
}

typedef struct Bench_K_Queue_Result {
    double time;
    uint64_t pop_tries;
    uint64_t pop_ops;
    uint64_t pop_reloads;
    uint64_t push_ops;
    uint64_t push_tries;
    uint64_t push_reloads;
    uint64_t capacity;
} Bench_K_Queue_Result;

static Bench_K_Queue_Result test_k_queue_single(isize reserve_size, isize consumer_count, isize chunk_size, double time, void(*func)(void*))
{
    K_Queue queue = {0};
    k_queue_init(&queue, 10000000, sizeof(isize));
    //TODO reserve size

    ATOMIC(isize) started = 0;
    ATOMIC(isize) finished = 0;
    ATOMIC(isize) run_test = 0;
    
    //start all threads
    enum {MAX_THREADS = 64};
    Test_K_Queue_Thread threads[MAX_THREADS] = {0};
    for(isize i = 0; i < consumer_count; i++)
    {
        threads[i].queue = &queue;
        threads[i].started = &started;
        threads[i].finished = &finished;
        threads[i].run_test = &run_test;
        threads[i].chunk_size = chunk_size;

        //run the test func in separate thread in detached state
        test_cl_launch_thread(func, &threads[i]);
    }
    
    isize push_reloads= 0;
    isize push_ops = 0;
    //run test
    {
        while(started != consumer_count);
        run_test = 1;
        
        K_Queue_Local local = k_queue_local(&queue);
        isize deadline = clock() + (isize)(time*CLOCKS_PER_SEC);
        for(; clock() < deadline; push_ops++)
            k_queue_push(&queue, &local, &push_ops, sizeof(isize));

        run_test = 2;
        while(finished != consumer_count);

        push_reloads = local.reloads;
    }
    
    if(0)
    if(func == test_k_queue_thread_func)
    {
        Test_CL_Buffer agregated = {0};
        K_Queue_Local local = k_queue_local(&queue);
        isize val = 0;
        while(k_queue_pop(&queue, &local, &val, sizeof(isize), chunk_size))
            test_cl_buffer_push(&agregated, &val, 1);

        for(isize i = 0; i < consumer_count; i++) 
            test_cl_buffer_push(&agregated, threads[i].buffer.data, threads[i].buffer.count);

        qsort(agregated.data, agregated.count, sizeof(isize), test_cl_isize_comp_func);
        for(isize i = 0; i < (isize) agregated.count; i++)
            TEST(agregated.data[i] == i);

        TEST(agregated.count == push_ops);
        test_cl_buffer_deinit(&agregated);
    }

    Bench_K_Queue_Result res = {0};
    //res.capacity = cl_queue_capacity(&queue);
    res.time = (double)(isize)(time*CLOCKS_PER_SEC)/CLOCKS_PER_SEC;
    res.push_ops = push_ops;
    res.push_tries = push_ops;
    res.push_reloads = push_reloads;
    for(isize i = 0; i < consumer_count; i++) {
        res.pop_ops += threads[i].ops;
        res.pop_tries += threads[i].tries;
        res.pop_reloads += threads[i].reloads;
        test_cl_buffer_deinit(&threads[i].buffer);
    }
    
    printf("k_queue: threads:%2lli throughput:%7.2lf/%7.2lf millions/s total:%10lli (%4.2lf success rate) \n", 
        consumer_count, (double) res.push_ops/(res.time*1e6), (double) res.pop_ops/(res.time*1e6), res.pop_ops, (double)res.pop_ops/res.pop_tries);

    printf("k_queue: reloads push:%3lli pop:%3lli\n", res.push_reloads, res.pop_reloads);
    k_queue_deinit(&queue);
    return res;
}

static void test_k_queue_queue(double time)
{
    printf("test_k_queue testing sequential\n");
    //test_k_queue_sequential(0, 1);
    //test_k_queue_sequential(1, 1);
    //test_k_queue_sequential(2, 1);
    //test_k_queue_sequential(2, 2);
    //test_k_queue_sequential(10, 2);
    //test_k_queue_sequential(100, 10);
    //test_k_queue_sequential(1024, 16);
    //test_k_queue_sequential(1024*1024, 16);
    //test_k_queue_sequential(1024*1024, 64);
    
    printf("test_k_queue testing stress\n");
    //test_k_queue_single(10, 1, 64, 1, test_k_queue_thread_func);
    //test_k_queue_single(10, 2, 64, 1, test_k_queue_thread_func);
    //test_k_queue_single(10, 3, 64, 1, test_k_queue_thread_func);
    //test_k_queue_single(10, 4, 64, 1, test_k_queue_thread_func);
    //test_k_queue_single(10, 5, 64, 1, test_k_queue_thread_func);
    //test_k_queue_single(10, 6, 64, 1, test_k_queue_thread_func);
    //test_k_queue_single(10, 7, 64, 1, test_k_queue_thread_func);
    //test_k_queue_single(10, 8, 64, 1, test_k_queue_thread_func);
    //test_k_queue_single(10, 16, 64, 1, test_k_queue_thread_func);
    
    //test_k_queue_single(10, 1, 64, 1, bench_k_queue_thread_func);
    //test_k_queue_single(10, 2, 64, 1, bench_k_queue_thread_func);
    //test_k_queue_single(10, 3, 64, 1, bench_k_queue_thread_func);
    //test_k_queue_single(10, 4, 64, 1, bench_k_queue_thread_func);
    //test_k_queue_single(10, 5, 64, 1, bench_k_queue_thread_func);
    //test_k_queue_single(10, 6, 64, 1, bench_k_queue_thread_func);
    //test_k_queue_single(10, 7, 64, 1, bench_k_queue_thread_func);
    //test_k_queue_single(10, 8, 64, 1, bench_k_queue_thread_func);
    test_k_queue_single(10, 15, 128, 1, bench_k_queue_thread_func);
}