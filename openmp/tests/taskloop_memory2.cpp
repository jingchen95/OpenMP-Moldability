#include <stdio.h>
#include <omp.h>
#include <string>

#define R1 1<<7            // number of rows in Matrix-1
#define C1 1<<7            // number of columns in Matrix-1
#define R2 1<<7            // number of rows in Matrix-2
#define C2 1<<7            // number of columns in Matrix-2



int main(int argc, char *argv[]){

  omp_set_dynamic(0);     // Explicitly disable dynamic teams
  omp_set_num_threads(4);
  printf("%d\n", omp_get_num_threads());
  int tasks = 1000;
  int size = 20000;
  
  if (argc >= 3){
  
  	tasks = std::stoi(argv[1]);
  	size = std::stoi(argv[2]); 
  	
  }
  
  
  int first_array[size];
  int second_array[size];
  int final_array[size];
  
  
  for (int i = 0; i < size; i++){
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
			    for (int j = 0; j < size; j++) {
				final_array[j] = first_array[j];
				final_array[j] = second_array[j];
			    }
			}
			#pragma omp taskwait
		}
	}
   }
   printf("tasks=%d,size=%d\n", tasks, size);
}

