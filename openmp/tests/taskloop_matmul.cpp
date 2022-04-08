#include <stdio.h>
#include <omp.h>
#include <string>
#include <string.h>





int main(int argc, char *argv[]){

  int size = 1<<6;
  int tasks = 1000;

  omp_set_dynamic(0);     // Explicitly disable dynamic teams
  omp_set_num_threads(4);
  printf("%d\n", omp_get_num_threads());
  
 
  if (argc >= 3){
  
  	tasks = std::stoi(argv[1]);
  	size = std::stoi(argv[2]); 
  	
  }
  
  int mat1[size][size];
  int mat2[size][size];
  int rslt[size][size];
  
  
  memset( mat1, 1, sizeof(mat1));
  memset( mat2, 2, sizeof(mat2));
  
#pragma omp parallel
    {
    #pragma omp single
        {
        	for(int task_it = 0; task_it < tasks; task_it++){
			#pragma omp taskloop
			{
			    for (int i = 0; i < size; i++) {
				for (int j = 0; j < size; j++) {
				    rslt[i][j] = 0;
			 
				    for (int k = 0; k < size; k++) {
					rslt[i][j] += mat1[i][k] * mat2[k][j];
				    }
				}
			    }
			}
			#pragma omp taskwait
		}
	}
   }
   printf("tasks=%d,size=%d\n", tasks, size);
}

