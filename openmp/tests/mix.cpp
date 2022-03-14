#include <stdio.h>
#include <omp.h>


int main() {

    omp_set_dynamic(0);     // Explicitly disable dynamic teams
    omp_set_num_threads(8);
    printf("%d\n", omp_get_num_threads());
    int over_it = 4;
    int tasks = 4;
    int iterations = 20000;
    int shared_array[tasks][iterations];
    int first_array[tasks][iterations];
    int second_array[tasks][iterations];
    int final_array[tasks][iterations];
    for (int i = 0; i < tasks; i++) {
        for (int j = 0; j < iterations; j++) {
            first_array[i][j] = i;
            second_array[i][j] = j;
        }
    }
#pragma omp parallel shared(final_array, first_array, second_array)
    {
#pragma omp single
        {
            for (int bigboi = 0; bigboi < over_it; bigboi++) {
                for (int i = 0; i < tasks; i++) {
#pragma omp task shared(final_array, first_array, second_array) firstprivate(i)
                    for (int j = 0; j < iterations; j++) {
                        final_array[i][j] = first_array[i][j] * second_array[i][j];
                    }
                }
//#pragma omp taskwait
                for (int i = 0; i < tasks; i++) {
#pragma omp task firstprivate(i)
                    for (int j = 0; j < iterations; j++) {
                        int k = i * j;
                        if (k > 10000000) {
                            k = j;
                        }
                    }
                }
//#pragma omp taskwait
                for (int i = 0; i < tasks; i++) {
#pragma omp task shared(shared_array) firstprivate(i)
                    for (int j = 0; j < iterations; j++) {
                        int tmp = i * j * 5;
                        tmp *= 5 + 2;
                        if (tmp % 2 != 0) {
                            shared_array[i][j] = tmp / (i + j + 1);
                        }
                    }
                }
#pragma omp taskwait
            }
        }
    }
}


