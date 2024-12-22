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

    k_queue_deinit(&pool);
}


static void test_k_queue_queue(double time)
{
    printf("test_k_queue testing sequential\n");
    //test_k_queue_sequential(0, 1);
    //test_k_queue_sequential(1, 1);
    //test_k_queue_sequential(2, 1);
    //test_k_queue_sequential(2, 2);
    //test_k_queue_sequential(10, 2);
    test_k_queue_sequential(100, 10);
    test_k_queue_sequential(1024, 16);
    test_k_queue_sequential(1024*1024, 16);
    test_k_queue_sequential(1024*1024, 64);
}