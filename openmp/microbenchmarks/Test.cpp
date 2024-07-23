#include <stdio.h>
#include <omp.h>
#include <sched.h>

int main() {
    #pragma omp parallel
    {
        int thread_id = omp_get_thread_num();
        int cpu_id = sched_getcpu();
        printf("Thread %d is running on CPU %d\n", thread_id, cpu_id);
    }
    return 0;
}

