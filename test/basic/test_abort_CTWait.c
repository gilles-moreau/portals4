#include <portals4.h>
#include <support.h>
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include <unistd.h>
#include <pthread.h>
#include "testing.h"
#define NI_TYPE  PTL_NI_MATCHING

struct thread_data {
    int             name;
    ptl_handle_eq_t eq_h;
    ptl_event_t     event;
    ptl_handle_ct_t ct_h;
    ptl_ct_event_t  counter;
};

void* thread_test(void* arg) {
    printf("I'm a worker thread testing!\n");
    return (void*) 42;
}

    
void* thread_EQPoll(void* arg) {
    void * ret;
    int err;
    unsigned int which;
    unsigned polltime_ms = 2000;
    struct thread_data *data = (struct thread_data*) arg; 
    printf("worker[%d]: calling PtlEQPoll\n", data->name);
    
    err = PtlEQPoll(&data->eq_h, 1, polltime_ms, &data->event, &which);
    if (err == PTL_ABORTED) {
        printf("worker[%d]: PTL_ABORTED returned\n", data->name);
    } else {
        printf("worker[%d]: PTL_ABORTED NOT returned\n", data->name);
    }
    ret = (void*) err;
    return ret;
}

void* thread_CTPoll(void* arg) {
    void * ret;
    int err;
    unsigned int which;
    ptl_size_t count = -1;
    unsigned timeout = PTL_TIME_FOREVER;
    struct thread_data *data = (struct thread_data*) arg; 
    printf("worker[%d]: calling PtlCTPoll\n", data->name);
    
    err = PtlCTPoll(&data->ct_h, &count, 1, timeout, &data->counter, &which);
    if (err == PTL_ABORTED) {
        printf("worker[%d]: PTL_ABORTED returned\n", data->name);
    } else {
        printf("worker[%d]: PTL_ABORTED NOT returned\n", data->name);
    }
    ret = (void*) err;
    return ret;
}

void* thread_CTWait(void* arg) {
    void * ret;
    int err;
    ptl_size_t test = 10;
    ptl_ct_event_t foo;
    struct thread_data *data = (struct thread_data*) arg;
    
    PtlCTGet(data->ct_h, &foo);
    printf("worker[%d]: data->ct_h success field == %u\n",
           data->name, foo.success);
    
    printf("worker[%d]: calling PtlCTWait\n", data->name);
    err = PtlCTWait(data->ct_h, test, &foo);
    printf("worker[%d]: after PtlCTWait, err == %d\n", err);
    if (err == PTL_ABORTED) {
        printf("worker[%d]: PTL_ABORTED returned\n", data->name);
    } else if (err == PTL_ARG_INVALID) {
        printf("worker[%d]: PTL_ARG_INVALID returned\n", data->name);
    } else {
        printf("worker[%d]: PTL_ABORTED NOT returned\n", data->name);
    }
    ret = (void*) err;
    return ret;
}

void* thread_abort(void* arg) {
    struct thread_data *data = (struct thread_data*) arg; 
    sleep(1);
    printf("worker[%d]: calling PtlAbort\n", data->name);
    PtlAbort();
}


int main(int   argc, char *argv[])
{
    ptl_handle_ni_t     ni_h;
    ptl_pt_index_t      pt_index;
    int                 num_procs, ret;
    int                 rank;
    ptl_process_t      *procs;

     
    struct thread_data  tdata0;
    struct thread_data  tdata1;
    struct thread_data  tdata2;
    ptl_handle_eq_t     eq_h;
    ptl_event_t         event;
    ptl_handle_ct_t     ct_h;
    ptl_ct_event_t      counter;
    

    CHECK_RETURNVAL(PtlInit());
    CHECK_RETURNVAL(libtest_init());
    rank      = libtest_get_rank();
    num_procs = libtest_get_size();
    CHECK_RETURNVAL(PtlNIInit(PTL_IFACE_DEFAULT, NI_TYPE | PTL_NI_LOGICAL,
                              PTL_PID_ANY, NULL, NULL, &ni_h));

    procs     = libtest_get_mapping(ni_h);

    CHECK_RETURNVAL(PtlSetMap(ni_h, num_procs, procs));

    CHECK_RETURNVAL(PtlEQAlloc(ni_h, 8192, &eq_h));
    CHECK_RETURNVAL(PtlPTAlloc(ni_h, 0, eq_h, PTL_PT_ANY,
                               &pt_index));
    
    CHECK_RETURNVAL(PtlEQAlloc(ni_h, 8192, &tdata1.eq_h));
    CHECK_RETURNVAL(PtlPTAlloc(ni_h, 0, tdata1.eq_h, PTL_PT_ANY,
                               &pt_index));

    CHECK_RETURNVAL(PtlEQAlloc(ni_h, 8192, &tdata0.eq_h));
    CHECK_RETURNVAL(PtlPTAlloc(ni_h, 0, tdata0.eq_h, PTL_PT_ANY,
                               &pt_index));
    
    CHECK_RETURNVAL(PtlEQAlloc(ni_h, 8192, &tdata2.eq_h));
    CHECK_RETURNVAL(PtlPTAlloc(ni_h, 0, tdata2.eq_h, PTL_PT_ANY,
                               &pt_index));

    
    CHECK_RETURNVAL(PtlCTAlloc(ni_h, &ct_h));
    CHECK_RETURNVAL(PtlCTAlloc(ni_h, &tdata0.ct_h));
    CHECK_RETURNVAL(PtlCTAlloc(ni_h, &tdata1.ct_h));

    libtest_barrier();

                    
    if (rank == 0) {
        
        int err;
        int ret;
        unsigned timeout = PTL_TIME_FOREVER;
        unsigned int which;
        ptl_size_t test = 10;

        
        // TODO dkruse this is where we poll/wait for stuff then abort
        pthread_attr_t tattr0;
        pthread_t worker0;
        void *status0;

        pthread_attr_t tattr1;
        pthread_t worker1;
        void *status1;

        pthread_attr_t tattr2;
        pthread_t worker2;
        void *status2;

        tdata0.name = 0;
        tdata1.name = 1;
        tdata2.name = 2;
        
        /* parent thread aborts, 2 worker threads poll */
        printf("parent thread aborts, 2 worker threads poll\n");
        ret = pthread_create(&worker0, NULL, thread_CTWait, &tdata0);
        ret = pthread_create(&worker1, NULL, thread_CTWait, &tdata1);
        sleep(1);
        PtlAbort();

        ret = pthread_join(worker0, &status0);
        if (ret)
            printf("worker0 pthread_join error\n");
            
        ret = pthread_join(worker1, &status1);
        if (ret)
            printf("worker1 pthread_join error\n");

        printf("\n");    
        printf("PTL_ARG_INVALID is %d\n", PTL_ARG_INVALID);
        printf("PTL_ABORTED is %d\n", PTL_ABORTED);
        printf("worker0 returned %d\n", (int) status0);
        printf("worker1 returned %d\n", (int) status1);
        printf("\n");    
        assert((int)status0 == PTL_ABORTED);
        assert((int)status1 == PTL_ABORTED);

        ret = PtlCTFree(tdata0.ct_h);
        printf("worker0 ret = %d\n", ret);
        ret = PtlCTFree(tdata1.ct_h);
        printf("worker1 ret = %d\n", ret);
        printf("PTL_OK == %d\n", PTL_OK);
        printf("\n");    
  
        ret = PtlCTAlloc(ni_h, &ct_h);
        printf("CTAlloc: worker0 ret = %d\n", ret);
        ret = PtlPTAlloc(ni_h, 0, eq_h, PTL_PT_ANY,
                         &pt_index);
        printf("PTAlloc: worker0 ret = %d\n", ret);
    
        ret = PtlCTAlloc(ni_h, &tdata1.ct_h);
        printf("CTAlloc: worker1 ret = %d\n", ret);
        ret = PtlPTAlloc(ni_h, 0, tdata1.eq_h, PTL_PT_ANY,
                                   &pt_index);
        printf("PTAlloc: worker1 ret = %d\n", ret);
        printf("\n");    
        printf("\n");    



        printf("\n////////////////////////////////////////////////////////////////////////////////\n");
        printf("////////////////////////////////////////////////////////////////////////////////\n\n");


        /* sanity check */
        printf("sanity check:\n");
        ret = pthread_create(&worker2, NULL, thread_test, &tdata2);
        ret = pthread_join(worker2, &status2);
        printf("\n");
        printf("\n");    

        
        /* parent thread aborts, 2 worker threads poll again */
        printf("parent thread aborts, 2 worker threads poll again\n");
        ret = pthread_create(&worker0, NULL, thread_CTWait, &tdata0);
        if (ret)
            printf("worker0 pthread_create error\n");
        
        ret = pthread_create(&worker1, NULL, thread_CTWait, &tdata1);
        if (ret)
            printf("worker1 pthread_create error\n");
        sleep(1);
        PtlAbort();
        ret = pthread_join(worker0, &status0);
        if (ret)
            printf("worker0 pthread_join error\n");
            
        ret = pthread_join(worker1, &status1);
        if (ret)
            printf("worker1 pthread_join error\n");
        printf("\n");    

        printf("PTL_ABORTED is %d\n", PTL_ABORTED);
        printf("worker0 returned %d\n", (int) status0);
        printf("worker1 returned %d\n", (int) status1);
        assert((int)status0 == PTL_ABORTED);
        printf("\n");    
        printf("\n");    
        
        ret = PtlCTFree(tdata0.ct_h);
        printf("worker0 ret = %d\n", ret);
        ret = PtlCTFree(tdata1.ct_h);
        printf("worker1 ret = %d\n", ret);
        printf("PTL_OK == %d\n", PTL_OK);
        printf("\n");    
  
        ret = PtlCTAlloc(ni_h, &ct_h);
        printf("CTAlloc: worker0 ret = %d\n", ret);
        ret = PtlPTAlloc(ni_h, 0, eq_h, PTL_PT_ANY,
                         &pt_index);
        printf("PTAlloc: worker0 ret = %d\n", ret);
    
        ret = PtlCTAlloc(ni_h, &tdata1.ct_h);
        printf("CTAlloc: worker1 ret = %d\n", ret);
        ret = PtlPTAlloc(ni_h, 0, tdata1.eq_h, PTL_PT_ANY,
                                   &pt_index);
        printf("PTAlloc: worker1 ret = %d\n", ret);
        printf("\n");    
        printf("\n");    

        
        printf("\n////////////////////////////////////////////////////////////////////////////////\n");
        printf("////////////////////////////////////////////////////////////////////////////////\n\n");
        
        /* sanity check */
        printf("sanity check:\n");
        ret = pthread_create(&worker2, NULL, thread_test, &tdata2);
        ret = pthread_join(worker2, &status2);
        printf("\n");

        
        /* parent thread and worker0 poll, worker2 aborts */
        printf("parent thread and worker0 poll, worker1 aborts\n");
        ret = pthread_create(&worker0, NULL, thread_CTWait, &tdata0);
        if (ret)
            printf("worker0 pthread_create error\n");
        
        ret = pthread_create(&worker1, NULL, thread_abort, &tdata1);
        if (ret)
            printf("worker1 pthread_create error\n");
        printf("\n");
        
        err = PtlCTWait(ct_h, test, &counter);
        
        ret = pthread_join(worker0, &status0);
        if (ret)
            printf("worker0 pthread_join error\n");
            
        ret = pthread_join(worker1, &status1);
        if (ret)
            printf("worker1 pthread_join error\n");
        printf("\n");

        printf("PTL_ABORTED is %d\n", PTL_ABORTED);
        printf("worker0 returned %d\n", (int) status0);
        printf("parent returned %d\n", err);
        assert((int)status0 == PTL_ABORTED);
        assert(err == PTL_ABORTED);
        printf("\n");
        
        ret = PtlCTFree(tdata0.ct_h);
        printf("worker0 ret = %d\n", ret);
        ret = PtlCTFree(tdata1.ct_h);
        printf("worker1 ret = %d\n", ret);
        printf("PTL_OK == %d\n", PTL_OK);
        printf("\n");
  
        ret = PtlCTAlloc(ni_h, &ct_h);
        printf("CTAlloc: worker0 ret = %d\n", ret);
        ret = PtlPTAlloc(ni_h, 0, eq_h, PTL_PT_ANY,
                         &pt_index);
        printf("PTAlloc: worker0 ret = %d\n", ret);
    
        ret = PtlCTAlloc(ni_h, &tdata1.ct_h);
        printf("CTAlloc: worker1 ret = %d\n", ret);
        ret = PtlPTAlloc(ni_h, 0, tdata1.eq_h, PTL_PT_ANY,
                                   &pt_index);
        printf("PTAlloc: worker1 ret = %d\n", ret);
        printf("\n");
    }
    


    libtest_barrier();

    CHECK_RETURNVAL(PtlPTFree(ni_h, pt_index));
    CHECK_RETURNVAL(PtlNIFini(ni_h));
    CHECK_RETURNVAL(libtest_fini());
    PtlFini();
    return 0;
}