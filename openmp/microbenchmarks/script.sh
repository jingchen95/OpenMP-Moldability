#!/bin/bash

export KMP_DEBUG=8 
#export OMP_PROC_BIND=true
#export OMP_PLACES="{0},{1},{2},{3},{4},{5}"
#export OMP_PLACES=cores
edxport KMP_USE_HWLOC=true
export LD_LIBRARY_PATH=/home/nvidia/project/OpenMP-Moldability/openmp/build/runtime/src/
export KMP_AFFINITY=verbose
export OMP_NUM_THREADS=6

# Print environment variables to verify they are set correctly
echo "KMP_DEBUG=$KMP_DEBUG"
#echo "OMP_PROC_BIND=$OMP_PROC_BIND"
#echo "OMP_PLACES=$OMP_PLACES"
echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
echo "OMP_NUM_THREADS=$OMP_NUM_THREADS"

hwloc-bind 0,3,4,5,1,2 ./omp_test

#clang++ -fopenmp taskloop_mix.cpp -L/home/nvidia/project/OpenMP-Moldability/openmp/build/runtime/src/ -lomp -o taskloop_mix

#GOMP_CPU_AFFINITY="0 3 4 5 1 2" 
#./taskloop_mix 1024 100 > taskloop_mix.txt  2> error.txt
