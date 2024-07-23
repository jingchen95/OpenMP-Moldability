#include <stdio.h>
#include <omp.h>


int main()
{

  omp_set_dynamic(0);     // Explicitly disable dynamic teams
  omp_set_num_threads(8);
  printf("%d\n", omp_get_num_threads());
  int tasks = 16;
  int iterations = 100000;
  int shared_array[tasks][iterations];
#pragma omp parallel shared(shared_array)
    {
    #pragma omp single
        {
        for (int i = 0; i < tasks; i++) {
        #pragma omp task shared(shared_array) firstprivate(i)
            for (int j = 0; j < iterations; j++) {
                shared_array[i][j] = i * j;
            }
        }
    }
}
}

