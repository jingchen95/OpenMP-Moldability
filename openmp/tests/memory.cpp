#include <stdio.h>
#include <omp.h>


int main()
{

  omp_set_dynamic(0);     // Explicitly disable dynamic teams
  omp_set_num_threads(8);
  printf("%d\n", omp_get_num_threads());
  int tasks = 32;
  int iterations = 20000;
  int first_array[tasks][iterations];
  int second_array[tasks][iterations];
  int final_array[tasks][iterations];
  for (int i = 0; i < tasks; i++){
      for (int j = 0; j < iterations; j++){
          first_array[i][j] = i;
          second_array[i][j] = j;
      }
  }
#pragma omp parallel shared(final_array, first_array, second_array)
    {
    #pragma omp single
        {
        for (int i = 0; i < tasks; i++) {
        #pragma omp task shared(final_array, first_array, second_array) firstprivate(i)
            for (int j = 0; j < iterations; j++) {
                final_array[i][j] = first_array[i][j] * second_array[i][j];
            }
        }
    }
}
}


