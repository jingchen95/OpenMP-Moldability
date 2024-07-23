#include <stdio.h>
#include <omp.h>
#include <string>
#include <string.h>
#include <cstring>





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
  int rslt[size][size];
  
  
  memset( mat1, 1, sizeof(mat1));
  memset( rslt, 0, sizeof(rslt));
  
#pragma omp parallel
    {
    #pragma omp single
        {
        	for(int task_it = 0; task_it < tasks; task_it++){
			#pragma omp taskloop
			{
			    for (int i = 0; i < size; i++) {
				std::memcpy(&mat1[0][i], &rslt[0][i], size*sizeof(int));
			    }
			}
			#pragma omp taskwait
		}
	}
   }
   printf("tasks=%d,size=%d\n", tasks, size);
}

