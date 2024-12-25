
//#include "_test_pools.h"
#include "_test_chase_lev_queue.h"
//#include "_test_k_queue.h"

typedef enum Reread_Operation {
    REREAD_READ_CAS,
    REREAD_CAS,
    REREAD_FAA,
    REREAD_XCHG,
    REREAD_OR,
    REREAD_WRITE,
    REREAD_READ,
    REREAD_READ_PRIVATE,
    REREAD_READ_FAA_PRIVATE,
} Reread_Operation;

typedef struct Reread_Thread {
    alignas(64)
    CL_QUEUE_ATOMIC(isize)* started; 
    CL_QUEUE_ATOMIC(isize)* finished; 
    CL_QUEUE_ATOMIC(isize)* run_test; 
    CL_QUEUE_ATOMIC(isize)* deadline; 

    CL_QUEUE_ATOMIC(uint64_t)* shared_val;
    CL_QUEUE_ATOMIC(uint64_t) private_val;
    uint64_t private_val_nonatomic;

    isize start;
    isize stop;
    isize index;

    isize ops;
    isize tries;
    Reread_Operation operation;
} Reread_Thread;

static void bench_reread_thread_func(void *arg)
{
    Reread_Thread* thread = (Reread_Thread*) arg;
    atomic_fetch_add(thread->started, 1);

    while(*thread->run_test == 0); 
    
    thread->start = test_cl_clock_ns();
    isize deadline = atomic_load_explicit(thread->deadline, memory_order_relaxed);

    if(thread->operation == REREAD_READ_CAS)
    {
        while(test_cl_clock_ns() < deadline)
        {
            uint64_t val = atomic_load_explicit(thread->shared_val, memory_order_relaxed);
            thread->ops += atomic_compare_exchange_strong_explicit(thread->shared_val, &val, val + 1, memory_order_relaxed, memory_order_relaxed);
            thread->tries += 1;
        }
    }
    if(thread->operation == REREAD_CAS)
    {
        while(test_cl_clock_ns() < deadline)
        {
            uint64_t ops = thread->ops;
            thread->ops += atomic_compare_exchange_strong_explicit(thread->shared_val, &ops, ops + 1, memory_order_relaxed, memory_order_relaxed);
            thread->tries += 1;
        }
    }
    if(thread->operation == REREAD_XCHG)
    {
        while(test_cl_clock_ns() < deadline)
        {
            thread->private_val_nonatomic += atomic_exchange_explicit(thread->shared_val, 1, memory_order_relaxed);
            thread->ops += 1; 
            thread->tries += 1;
        }
    }
    if(thread->operation == REREAD_OR)
    {
        while(test_cl_clock_ns() < deadline)
        {
            atomic_fetch_or_explicit(thread->shared_val, 1, memory_order_relaxed);
            thread->ops += 1; 
            thread->tries += 1;
        }
    }
    if(thread->operation == REREAD_FAA)
    {
        while(test_cl_clock_ns() < deadline)
        {
            atomic_fetch_add_explicit(thread->shared_val, 1, memory_order_relaxed);
            thread->ops += 1; 
            thread->tries += 1;
        }
    }
    if(thread->operation == REREAD_WRITE)
    {
        while(test_cl_clock_ns() < deadline)
        {
            atomic_store_explicit(thread->shared_val, thread->ops, memory_order_relaxed);
            thread->ops += 1; 
            thread->tries += 1;
        }
    }
    if(thread->operation == REREAD_READ)
    {
        while(test_cl_clock_ns() < deadline)
        {
            uint64_t val = atomic_load_explicit(thread->shared_val, memory_order_relaxed);
            thread->private_val_nonatomic += val;
            thread->ops += 1; 
            thread->tries += 1;
        }
    }
    if(thread->operation == REREAD_READ_PRIVATE)
    {
        while(test_cl_clock_ns() < deadline)
        {
            uint64_t val = atomic_load_explicit(&thread->private_val, memory_order_relaxed);
            thread->private_val_nonatomic += val;
            thread->ops += 1; 
            thread->tries += 1;
        }
    }
    
    thread->stop = test_cl_clock_ns();
    atomic_fetch_add(thread->finished, 1);
}

typedef struct Bench_Reread_Result {
    isize ops1;
    isize tries1;
    
    isize ops2;
    isize tries2;

    double duration1;
    double duration2;
} Bench_Reread_Result;

Bench_Reread_Result bench_reread_single(Reread_Operation op1, isize count1, Reread_Operation op2, isize count2, double seconds)
{
    CL_QUEUE_ATOMIC(isize) started = 0;
    CL_QUEUE_ATOMIC(isize) finished = 0;
    CL_QUEUE_ATOMIC(isize) run_test = 0;
    CL_QUEUE_ATOMIC(isize) deadline = 0;

    struct {
        alignas(64) CL_QUEUE_ATOMIC(uint64_t) val;
    } shared = {0};
    
    Reread_Thread threads[64] = {0};
    for(isize i = 0; i < count1 + count2; i++)
    {
        threads[i].started = &started;
        threads[i].finished = &finished;
        threads[i].run_test = &run_test;
        threads[i].deadline = &deadline;
        threads[i].shared_val = &shared.val;
        threads[i].operation = i < count1 ? op1 : op2;
        threads[i].index = i;

        atomic_thread_fence(memory_order_seq_cst);
        test_cl_launch_thread(bench_reread_thread_func, &threads[i]);
    }
    
    {
        while(started != count1 + count2);
        deadline = test_cl_clock_ns() + (isize)(seconds * 1e9);
        run_test = 1;
        
        test_cl_sleep_thread(seconds);

        while(finished != count1 + count2);
    }
    
    isize total_clocks1 = 0;
    isize total_clocks2 = 0;
    Bench_Reread_Result result = {0};
    for(isize i = 0; i < count1 + count2; i++) 
    {
        if(i < count1)
        {
            result.tries1 += threads[i].tries;
            result.ops1 += threads[i].ops;
            total_clocks1 += threads[i].stop - threads[i].start;
        }
        else
        {
            result.tries2 += threads[i].tries;
            result.ops2 += threads[i].ops;
            total_clocks2 += threads[i].stop - threads[i].start;
        }
    }

    result.duration1 = total_clocks1/1e9;
    result.duration2 = total_clocks2/1e9;
    return result;
}

void bench_reread_all(double seconds, isize threads)
{
    for(isize i = 0; i < threads - 1; i++)
    {
        isize j = 1;
        Bench_Reread_Result res = bench_reread_single(REREAD_XCHG, j, REREAD_READ, i, seconds);

        printf("%2lli x %2lli: ops/tries %5.2lf/%5.2lf M/s (%.2lf) | ops/tries %5.2lf/%5.2lf M/s (%.2lf)\n", j, i,
            res.ops1/res.duration1/1e6, res.tries1/res.duration1/1e6, (double) res.ops1/res.tries1,
            res.ops2/res.duration2/1e6, res.tries2/res.duration2/1e6, (double) res.ops2/res.tries2
        );
    }
}


int main() {
    bench_reread_all(1, 12);
    //test_chase_lev_queue(2);
    //bench_chase_lev(1, 12);
    //test_lc_pool(3, 12);
    //bench_lc_pool(1, 12);

    //test_k_queue_queue(3);
}