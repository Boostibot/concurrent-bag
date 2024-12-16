#pragma once

#include "chase_lev_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdlib.h>

#ifndef TEST
    #define TEST(x, ...) (!(x) ? printf("error %s!\n",#x), abort() : (void) 0)
#endif

static void test_chase_lev_sequential(isize count, isize reserve_to)
{
    CL_Queue q = {0};
    cl_queue_init(&q, sizeof(int), -1);

    //zero pop
    int dummy = 0;
    TEST(cl_queue_pop(&q, &dummy, sizeof(int)) == false);
    TEST(cl_queue_pop_back(&q, &dummy, sizeof(int)) == false);

    //test zero capacity and count
    TEST(cl_queue_capacity(&q) == 0);
    TEST(cl_queue_count(&q) == 0);
    cl_queue_reserve(&q, reserve_to);
    TEST(cl_queue_capacity(&q) >= reserve_to);
    TEST(cl_queue_count(&q) == 0);

    //still pops should fail
    TEST(cl_queue_pop(&q, &dummy, sizeof(int)) == false);
    TEST(cl_queue_pop_back(&q, &dummy, sizeof(int)) == false);
    TEST(cl_queue_count(&q) == 0);

    //push count
    for(int i = 0; i < count; i++)
        TEST(cl_queue_push(&q, &i, sizeof(int)));
    
    //push one more potentially causing realloc
    dummy = 10;
    TEST(cl_queue_push(&q, &dummy, sizeof(int)));
    TEST(cl_queue_count(&q) == count + 1);
    TEST(cl_queue_capacity(&q) >= count + 1);

    //pop count
    for(int i = 0; i < count; i++)
    {
        int popped = 0;
        TEST(cl_queue_pop(&q, &popped, sizeof(int)));
        TEST(popped == i);
    }

    //popping it back
    TEST(cl_queue_pop(&q, &dummy, sizeof(int)));
    TEST(dummy == 10);
        
    //push again
    for(int i = 0; i < count; i++)
        TEST(cl_queue_push(&q, &i, sizeof(int)));
        
    //pop back
    for(int i = 0; i < count; i++)
    {
        int popped = 0;
        TEST(cl_queue_pop_back(&q, &popped, sizeof(int)));
        TEST(popped == count - 1 - i);
    }

    //pop empty
    TEST(cl_queue_pop(&q, &dummy, sizeof(int)) == false);
    TEST(cl_queue_pop_back(&q, &dummy, sizeof(int)) == false);
    TEST(cl_queue_count(&q) == 0);
    TEST(cl_queue_capacity(&q) >= count + 1);

    //push some before dealloc
    dummy = 10;
    TEST(cl_queue_push(&q, &dummy, sizeof(int)));
    TEST(cl_queue_push(&q, &dummy, sizeof(int)));
    TEST(cl_queue_push(&q, &dummy, sizeof(int)));

    cl_queue_deinit(&q);
}

typedef struct Test_CL_Buffer {
    isize* data; 
    isize count;
    isize capacity;
} Test_CL_Buffer;

typedef struct Test_CL_Thread {
    CL_QUEUE_ATOMIC(isize)* started; 
    CL_QUEUE_ATOMIC(isize)* finished; 
    CL_QUEUE_ATOMIC(isize)* run_test; 
    CL_Queue* queue;

    Test_CL_Buffer popped;
} Test_CL_Thread;

//Helper functions
static int  test_cl_isize_comp_func(const void* a, const void* b);
static void test_cl_launch_thread(void (*func)(void*), void* context);
static void test_cl_buffer_push(Test_CL_Buffer* buffer, isize* val, isize count);

static void test_chase_lev_producer_consumers_thread_func(void *arg)
{
    Test_CL_Thread* thread = (Test_CL_Thread*) arg;
    atomic_fetch_add(thread->started, 1);

    //wait to run
    while(*thread->run_test == 0); 
    
    //run for as long as we can
    while(*thread->run_test == 1)
    {
        isize val = 0;
        if(cl_queue_pop(thread->queue, &val, sizeof(isize)))
            test_cl_buffer_push(&thread->popped, &val, 1);
    }

    atomic_fetch_add(thread->finished, 1);
}

static void test_chase_lev_producer_consumers(isize reserve_size, isize consumer_count, double time, double producer_pop_back_chance, double producer_pop_front_chance)
{
    CL_Queue queue = {0};
    cl_queue_init(&queue, sizeof(isize), -1);
    cl_queue_reserve(&queue, reserve_size);

    CL_QUEUE_ATOMIC(isize) started = 0;
    CL_QUEUE_ATOMIC(isize) finished = 0;
    CL_QUEUE_ATOMIC(isize) run_test = 0;
    
    //start all threads
    enum {MAX_THREADS = 64};
    Test_CL_Thread threads[MAX_THREADS] = {0};
    for(isize i = 0; i < consumer_count; i++)
    {
        threads[i].queue = &queue;
        threads[i].started = &started;
        threads[i].finished = &finished;
        threads[i].run_test = &run_test;

        //run the test func in separate thread in detached state
        test_cl_launch_thread(test_chase_lev_producer_consumers_thread_func, &threads[i]);
    }
    
    isize produced_counter = 0;
    Test_CL_Thread producer = {0};

    //run test
    {
        while(started != consumer_count);
        run_test = 1;

        isize deadline = clock() + (isize)(time*CLOCKS_PER_SEC);
        while(clock() < deadline)
        {
            cl_queue_push(&queue, &produced_counter, sizeof(isize));
            produced_counter += 1;

            double random = (double) rand() / RAND_MAX;
            if(random < producer_pop_back_chance)
            {
                isize popped = 0;
                if(cl_queue_pop_back(&queue, &popped, sizeof(isize)))
                    test_cl_buffer_push(&producer.popped, &popped, 1);
            }
            else if(random < producer_pop_back_chance + producer_pop_front_chance)
            {
                isize popped = 0;
                if(cl_queue_pop(&queue, &popped, sizeof(isize)))
                    test_cl_buffer_push(&producer.popped, &popped, 1);
            }
        }

        run_test = 2;
        while(finished != consumer_count);
    }

    //pop all remaining items
    {
        isize popped = 0;
        while(cl_queue_pop(&queue, &popped, sizeof(isize)))
            test_cl_buffer_push(&producer.popped, &popped, 1);
    }

    //Validate results
    {
        //copy all results into a single array
        Test_CL_Buffer buffer = {0};
        test_cl_buffer_push(&buffer, producer.popped.data, producer.popped.count);
        
        for(isize i = 0; i < consumer_count; i++)
        {
            Test_CL_Buffer* curr = &threads[i].popped;
            test_cl_buffer_push(&buffer, curr->data, curr->count);

            //items in popped must be well ordered
            for(isize k = 1; k < curr->count; k++)
                TEST(curr->data[k - 1] < curr->data[k]);
        }

        TEST(buffer.count == produced_counter);
        
        //test if items are valid
        qsort(buffer.data, buffer.count, sizeof(isize), test_cl_isize_comp_func);
        for(isize i = 0; i < produced_counter; i++)
            TEST(buffer.data[i] == i);

        printf("consumers:%lli total:%lli throughput:%.2lf millions/s\n", consumer_count, buffer.count, (double) buffer.count/(time*1e6));
        free(buffer.data);
    }
    
    //deinit everything
    free(producer.popped.data);
    for(isize i = 0; i < consumer_count; i++)
        free(threads[i].popped.data);

    cl_queue_deinit(&queue);
}

static void test_chase_lev_queue(double time)
{
    printf("test_chase_lev testing sequential\n");
    test_chase_lev_sequential(0, 0);
    test_chase_lev_sequential(1, 0);
    test_chase_lev_sequential(2, 1);
    test_chase_lev_sequential(10, 8);
    test_chase_lev_sequential(100, 100);
    test_chase_lev_sequential(1024, 1024);
    test_chase_lev_sequential(1024*1024, 1024);
    
    if(time > 0)
    {
        printf("test_chase_lev testing stress\n");
        enum {THREADS = 32};
        for(isize i = 1; i <= THREADS; i++) {
            test_chase_lev_producer_consumers(1000, i, time/THREADS, 0.1, 0.1);
        }
    }
    printf("test_chase_lev done!\n");
}


typedef struct Bench_CL_Thread {
    CL_QUEUE_ATOMIC(isize)* started; 
    CL_QUEUE_ATOMIC(isize)* finished; 
    CL_QUEUE_ATOMIC(isize)* run_test; 
    CL_Queue* queue;

    isize ops;
    isize tries;
    isize slowdown;
    isize loops;
} Bench_CL_Thread;

static void bench_chase_lev_thread_func(void *arg)
{
    Bench_CL_Thread* thread = (Bench_CL_Thread*) arg;
    //notify we started
    atomic_fetch_add(thread->started, 1);

    //wait to run
    while(*thread->run_test == 0); 
    
    CL_QUEUE_ATOMIC(isize) dummy = 0;
    //run for as long as we can
    while(atomic_load_explicit(thread->run_test, memory_order_relaxed) == 1)
    {
        isize val = 0;
        thread->ops += cl_queue_pop(thread->queue, &val, sizeof(isize));
        thread->tries += 1;

        for(isize i = 0; i < thread->slowdown; i++)
        {
            //_mm_pause();
            volatile isize a = atomic_load_explicit(thread->run_test, memory_order_relaxed);
            //atomic_fetch_add_explicit(&dummy, 1, memory_order_relaxed);
        }
    }

    atomic_fetch_add(thread->finished, 1);
}

typedef struct Bench_CL_Result {
    double time;
    uint64_t pop_tries;
    uint64_t pop_ops;
    uint64_t push_ops;
    uint64_t push_tries;
    uint64_t capacity;
} Bench_CL_Result;

static Bench_CL_Result bench_chase_lev_single(isize reserve_size, isize consumer_count, double time, isize slowdown)
{
    CL_Queue queue = {0};
    cl_queue_init(&queue, sizeof(isize), -1);
    cl_queue_reserve(&queue, reserve_size);

    CL_QUEUE_ATOMIC(isize) started = 0;
    CL_QUEUE_ATOMIC(isize) finished = 0;
    CL_QUEUE_ATOMIC(isize) run_test = 0;
    
    //start all threads
    enum {MAX_THREADS = 64};
    Bench_CL_Thread threads[MAX_THREADS] = {0};
    for(isize i = 0; i < consumer_count; i++)
    {
        threads[i].queue = &queue;
        threads[i].started = &started;
        threads[i].finished = &finished;
        threads[i].run_test = &run_test;
        threads[i].slowdown = slowdown;

        //run the test func in separate thread in detached state
        test_cl_launch_thread(bench_chase_lev_thread_func, &threads[i]);
    }
    
    isize push_ops = 0;
    //run test
    {
        while(started != consumer_count);
        run_test = 1;

        isize deadline = clock() + (isize)(time*CLOCKS_PER_SEC);
        for(; clock() < deadline; push_ops++)
            cl_queue_push(&queue, &push_ops, sizeof(isize));

        run_test = 2;
        while(finished != consumer_count);
    }
    
    Bench_CL_Result res = {0};
    res.capacity = cl_queue_capacity(&queue);
    res.time = (double)(isize)(time*CLOCKS_PER_SEC)/CLOCKS_PER_SEC;
    res.push_ops = push_ops;
    res.push_tries = push_ops;
    for(isize i = 0; i < consumer_count; i++) {
        res.pop_ops += threads[i].ops;
        res.pop_tries += threads[i].tries;
    }

    cl_queue_deinit(&queue);
    return res;
}

static Bench_CL_Result bench_chase_lev_repeated(isize reserve_size, isize consumer_count, double total_time, isize slowdown, isize repeats)
{
    double time = total_time / repeats;
    Bench_CL_Result sum = {0}; 
    for(isize i = 0; i < repeats; i++)
    {
        Bench_CL_Result res = bench_chase_lev_single(reserve_size, consumer_count, time, slowdown);
        sum.time += res.time;
        sum.pop_ops += res.pop_ops;
        sum.pop_tries += res.pop_tries;
        sum.push_ops += res.push_ops;
        sum.push_tries += res.push_tries;
        if(sum.capacity < res.capacity)
            sum.capacity = res.capacity;
    }

    return sum;
}

void bench_chase_lev(double time, isize max_threads) 
{
    isize reserve_count = 1024*1024*2;
    isize repeats = 10;
    isize slowdowns[4] = {0, 5, 10, 15};

    for(isize slow_i = 0; slow_i < 4; slow_i ++)
    {
        printf("slowdown: %lli \n", slowdowns[slow_i]);
        for(isize i = 2; i <= max_threads; i+= 2)
        {
            Bench_CL_Result res = bench_chase_lev_repeated(reserve_count, i-1, time, slowdowns[slow_i], repeats);
            printf("chase_lev (pop ): threads:%2lli throughput:%7.2lf millions/s total:%10lli (%4.2lf success rate) \n", i, (double) res.pop_ops/(res.time*1e6), res.pop_ops, (double)res.pop_ops/res.pop_tries);
            printf("chase_lev (push): threads:%2lli throughput:%7.2lf millions/s total:%10lli (%4.2lf success rate) ", i, (double) res.push_ops/(res.time*1e6), res.push_ops, (double)res.push_ops/res.push_tries);

            if(reserve_count == res.capacity)
                printf("\n");
            else
                printf(" reserved:%lli Mebi max_capacity:%lli Mebi \n", reserve_count/(1024*1024), res.capacity/(1024*1024));
        }
    }
}


//Helper functions IMPLS ================
static void test_cl_buffer_push(Test_CL_Buffer* buffer, isize* val, isize count) 
{
    if(buffer->count + count > buffer->capacity)
    {
        isize new_capacity = buffer->capacity*3/2 + 8;
        if(new_capacity < buffer->count + count)
            new_capacity = buffer->count + count;
        buffer->data = (isize*) realloc(buffer->data, sizeof(isize) * new_capacity);
        buffer->capacity = new_capacity;
    }

    memcpy(buffer->data + buffer->count, val, count * sizeof(isize));
    buffer->count += count;
}

static int test_cl_isize_comp_func(const void* a, const void* b)
{
    isize x = *(const isize*) a;
    isize y = *(const isize*) b;

    return (x > y) - (x < y);
}

#ifdef __cplusplus
    #include <thread>
    static void test_cl_launch_thread(void (*func)(void*), void* context)
    {
         std::thread(func, context).detach();
    }
#elif defined(_WIN32) || defined(_WIN64)
    #include <process.h>
    static void test_cl_launch_thread(void (*func)(void*), void* context)
    {
        uintptr_t _beginthread(
            void( __cdecl *start_address )( void * ),
            unsigned stack_size,
            void *arglist
        );

        _beginthread(func, 0, context);
    }

#else
    #include <pthread.h>
    static void* test_chase_lev_launch_caster(void* func_and_context)
    {
        typedef void (*Void_Func)(void* context);

        Void_Func func = (Void_Func) ((void**) func_and_context)[0];
        void* context =              ((void**) func_and_context)[1];
        func(context);
        free(func_and_context);
        return NULL;
    }

    static void test_cl_launch_thread(void (*func)(void* context), void* context)
    {
        void** func_and_context = (void**) malloc(sizeof(void*)*2);
        func_and_context[0] = func;
        func_and_context[1] = context;

        pthread_t handle = {0};
        pthread_attr_t attr = {0};
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int error = pthread_create(&handle, &attr, test_chase_lev_launch_caster, func_and_context);
        pthread_attr_deinit(&attr);
        assert(error == 0);
    }
#endif
