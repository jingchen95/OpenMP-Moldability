#include <iostream>
#include <fstream>
#include <chrono>
#include <vector>
#include <ostream>
#include <omp.h>
#include <stdio.h>

using namespace std;
using namespace std::chrono;

int main(int argc, char const *argv[])
{
    omp_set_num_threads(6);
    int size = 256;
    int iterations = 100;
      if (argc >= 3){
        size = std::stoi(argv[1]); 
  	    iterations = std::stoi(argv[2]);
  }
    vector<vector<double>> a(size, vector<double> (size, 0));
    vector<vector<double>> b(size, vector<double> (size, 0));
    vector<vector<double>> c(size, vector<double> (size, 0));
    vector<vector<double>> d(size, vector<double> (size, 0));
    vector<vector<double>> e(size, vector<double> (size, 0));
    double maxDiff = 0.0;
    auto start = 
    std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::system_clock::now().time_since_epoch()).count();

    #pragma omp parallel
    {
        #pragma omp single
        {   
            //#pragma omp taskloop shared(a, b)
            //{
                for (int i = 0; i < size; i++) {
                    for (int j = 0; j < size; j++) {
                        a[i][j] = rand();
                        b[i][j] = rand();
                    }
                }
            //}
            //#pragma omp taskwait
            for(int iter = 0; iter < iterations; iter++){
                // Copy
                #pragma omp taskloop shared(e)
                for(int i = 0; i < size; i++){
                    for(int j = 0; j < size; j++){
                        e[i][j] = a[i][j];
                    }
                }
		#pragma omp taskwait
                // matrix multiplication
                #pragma omp taskloop shared(c)
                for(int i = 0; i < size; i++){
                    for (int k = 0; k < size; k++){
                        for(int j = 0; j < size; j++){
                            c[i][j] += a[i][k]*b[k][j];
                        }
                    }
                }
                // Jacobsi
		#pragma omp taskwait
                #pragma omp taskloop
                
                    for (int x = 0; x < size; x++) {
                        for (int y = 0; y < size; y++) {

                        // Start with "self" value
                        double sum = a[x][y];
                        double divisor = 1.0;

                    // Add the neighbors' values to average calculation
                        if (x > 0)     { sum += a[x - 1][y]; divisor++; }
                        if (x < size - 1) { sum += a[x + 1][y]; divisor++; }
                        if (y > 0)     { sum += a[x][y - 1]; divisor++; }
                        if (y < size - 1) { sum += a[x][y + 1]; divisor++; }

                    // Calculate new value, difference, and update maxDiff
                        d[x][y] = sum / (divisor);
                        double diff = abs(a[x][y] - d[x][y]);
                        if (diff > maxDiff) maxDiff = diff;
                        }
                    }
                
                #pragma omp taskwait
                // Swap in and out
                /*
                    double *ptr = &a;
                    a = &d;
                    d = *ptr;
                    */
            }
        }
    }

    auto end = 
    std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::system_clock::now().time_since_epoch()).count();
    cout << "start : " << start << endl;
    cout << "end : " << start << endl;
    cout << "matrix size : " << size << endl;
    cout << "Iterations : " << iterations << endl;
    cout << "total time: " << end - start << endl;
    ofstream outfile;
    outfile.open("result.txt", ios::out | ios::app); // append instead of overwrite
    outfile << start << ',' << end << ',' << size << ',' << iterations << ',' << end - start << "\n";
    return 0;
}



