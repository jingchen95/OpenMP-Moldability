#include <stdio.h>
#include <omp.h>


int main()
{

  omp_set_dynamic(0);     // Explicitly disable dynamic teams
  omp_set_num_threads(8);
  printf("%d\n", omp_get_num_threads());
  int tasks = 10;
  int iterations = 8*8;
#pragma omp parallel
    {
    #pragma omp single
        {
        	for(int task_it = 0; task_it < tasks; task_it++){
			#pragma omp taskloop
			{
				for (int i = 1; i < iterations; i++) {
					int k = i * 5;
					int pq = k*i;
					if( pq < (pq - (k + 1))) pq = 0;
			    	}
			}
			#pragma omp taskwait
		}
	}
   }
}

