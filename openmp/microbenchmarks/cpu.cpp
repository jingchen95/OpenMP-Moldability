#include <stdio.h>
#include <omp.h>


int main()
{

  omp_set_dynamic(0);     // Explicitly disable dynamic teams
  omp_set_num_threads(8);
  printf("%d\n", omp_get_num_threads());
  int tasks = 16;
  int iterations = 100000;
#pragma omp parallel
    {
    #pragma omp single
        {
        for (int i = 0; i < tasks; i++) {
        #pragma omp task firstprivate(i)
            for (int j = 0; j < iterations; j++) {
                int k = i * j;
                if (k < 10000000){
                    k = j;
                }
            }
        }
    }
}
}

