#include <stdio.h>
#include <omp.h>

#define R1 1<<7            // number of rows in Matrix-1
#define C1 1<<7            // number of columns in Matrix-1
#define R2 1<<7            // number of rows in Matrix-2
#define C2 1<<7            // number of columns in Matrix-2



int main()
{

  omp_set_dynamic(0);     // Explicitly disable dynamic teams
  omp_set_num_threads(4);
  printf("%d\n", omp_get_num_threads());
  int tasks = 128;
  int iterations = 200000;
  int first_array[iterations];
  int second_array[iterations];
  int final_array[iterations];
  for (int i = 0; i < iterations; i++){
       first_array[i] = i;
       second_array[i] = i;
  }

#pragma omp parallel
    {
    #pragma omp single
        {
        	for(int i = 0; i < tasks; i++){
			#pragma omp taskloop
			{
			    for (int j = 0; j < iterations; j++) {
				final_array[j] = first_array[j] * second_array[j];
			    }
			}
			#pragma omp taskwait
		}
	}
   }
}

