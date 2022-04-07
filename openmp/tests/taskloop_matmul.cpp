#include <stdio.h>
#include <omp.h>

#define R1 1<<6            // number of rows in Matrix-1
#define C1 1<<6            // number of columns in Matrix-1
#define R2 1<<6            // number of rows in Matrix-2
#define C2 1<<6            // number of columns in Matrix-2



int main()
{

  omp_set_dynamic(0);     // Explicitly disable dynamic teams
  omp_set_num_threads(4);
  printf("%d\n", omp_get_num_threads());
  int tasks = 1000;
  int iterations = 100000;
  
  int mat1[R1][C1] = {};
  int mat2[R2][C2] = {};
  int rslt[R1][C2];
#pragma omp parallel
    {
    #pragma omp single
        {
        	for(int task_it = 0; task_it < tasks; task_it++){
			#pragma omp taskloop
			{
			    for (int i = 0; i < R1; i++) {
				for (int j = 0; j < C2; j++) {
				    rslt[i][j] = 0;
			 
				    for (int k = 0; k < R2; k++) {
					rslt[i][j] += mat1[i][k] * mat2[k][j];
				    }
				}
			    }
			}
			#pragma omp taskwait
		}
	}
   }
}

