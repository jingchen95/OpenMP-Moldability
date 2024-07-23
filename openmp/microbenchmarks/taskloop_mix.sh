#!/bin/bash

# Using 4 A57 cores on TX2
export KMP_DEBUG=8 
export OMP_NUM_THREADS=4
export OMP_PROC_BIND=true
export OMP_PLACES="{0},{3},{4},{5}"
export LD_LIBRARY_PATH=/home/nvidia/project/OpenMP-Moldability/openmp_ori/build/runtime/src/
export KMP_AFFINITY=verbose

# Print environment variables to verify they are set correctly
echo "KMP_DEBUG=$KMP_DEBUG"
echo "OMP_PROC_BIND=$OMP_PROC_BIND"
echo "OMP_PLACES=$OMP_PLACES"
echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
echo "OMP_NUM_THREADS=$OMP_NUM_THREADS"

clang++ -fopenmp taskloop_mix.cpp -L/home/nvidia/project/OpenMP-Moldability/openmp_ori/build/runtime/src/ -lomp -o taskloop_mix_ori

# Define the parameter values
sizes=(128 256)
iterations=2
for size in "${sizes[@]}"; do
        for ((i=1; i<=iterations; i++)); do
		echo "/*---------------------------------------------------------------*/"
                echo "/*--------- Start running size $size, iteration $i---------------*/"
		./taskloop_mix_ori "$size" 10000  2>> error.txt
                sleep 10
        done
done

sizes=(512)
for size in "${sizes[@]}"; do
        for ((i=1; i<=iterations; i++)); do
		echo "/*---------------------------------------------------------------*/"
                echo "/*--------- Start running size $size, iteration $i---------------*/"
		./taskloop_mix_ori "$size" 1000  2>> error.txt
                sleep 10
        done
done

export KMP_DEBUG=8 
export OMP_NUM_THREADS=4
export OMP_PROC_BIND=true
export OMP_PLACES="{0},{3},{4},{5}"
# export OMP_PLACES=cores
export LD_LIBRARY_PATH=/home/nvidia/project/OpenMP-Moldability/openmp/build/runtime/src/
export KMP_AFFINITY=verbose

# Print environment variables to verify they are set correctly
echo "KMP_DEBUG=$KMP_DEBUG"
echo "OMP_PROC_BIND=$OMP_PROC_BIND"
echo "OMP_PLACES=$OMP_PLACES"
echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
echo "OMP_NUM_THREADS=$OMP_NUM_THREADS"

clang++ -fopenmp taskloop_mix.cpp -L/home/nvidia/project/OpenMP-Moldability/openmp/build/runtime/src/ -lomp -o taskloop_mix_new 

# Define the parameter values
sizes=(128 256)
iterations=2
for size in "${sizes[@]}"; do
        for ((i=1; i<=iterations; i++)); do
		echo "/*---------------------------------------------------------------*/"
                echo "/*--------- Start running size $size, iteration $i---------------*/"
		./taskloop_mix_new "$size" 10000 > output_${size}_${i}.txt  2>> error.txt
                sleep 10
        done
done

sizes=(512)
for size in "${sizes[@]}"; do
        for ((i=1; i<=iterations; i++)); do
		echo "/*---------------------------------------------------------------*/"
                echo "/*--------- Start running size $size, iteration $i---------------*/"
		./taskloop_mix_new "$size" 1000 > output_${size}_${i}.txt  2>> error.txt
                sleep 10
        done
done

# Using all 6 cores on TX2
# export KMP_DEBUG=8 
# export OMP_NUM_THREADS=6
# export OMP_PROC_BIND=true
# export OMP_PLACES=cores
# export LD_LIBRARY_PATH=/home/nvidia/project/OpenMP-Moldability/openmp_ori/build/runtime/src/
# export KMP_AFFINITY=verbose

# # Print environment variables to verify they are set correctly
# echo "KMP_DEBUG=$KMP_DEBUG"
# echo "OMP_PROC_BIND=$OMP_PROC_BIND"
# echo "OMP_PLACES=$OMP_PLACES"
# echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
# echo "OMP_NUM_THREADS=$OMP_NUM_THREADS"

# clang++ -fopenmp taskloop_mix.cpp -L/home/nvidia/project/OpenMP-Moldability/openmp_ori/build/runtime/src/ -lomp -o taskloop_mix_ori

# # Define the parameter values
# sizes=(128 256)
# iterations=2
# for size in "${sizes[@]}"; do
#         for ((i=1; i<=iterations; i++)); do
# 		echo "/*---------------------------------------------------------------*/"
#                 echo "/*--------- Start running size $size, iteration $i---------------*/"
# 		./taskloop_mix_ori "$size" 10000  2>> error.txt
#                 sleep 10
#         done
# done

# sizes=(512)
# for size in "${sizes[@]}"; do
#         for ((i=1; i<=iterations; i++)); do
# 		echo "/*---------------------------------------------------------------*/"
#                 echo "/*--------- Start running size $size, iteration $i---------------*/"
# 		./taskloop_mix_ori "$size" 1000  2>> error.txt
#                 sleep 10
#         done
# done

# export KMP_DEBUG=8 
# export OMP_NUM_THREADS=6
# export OMP_PROC_BIND=true
# export OMP_PLACES=cores
# export LD_LIBRARY_PATH=/home/nvidia/project/OpenMP-Moldability/openmp/build/runtime/src/
# export KMP_AFFINITY=verbose

# # Print environment variables to verify they are set correctly
# echo "KMP_DEBUG=$KMP_DEBUG"
# echo "OMP_PROC_BIND=$OMP_PROC_BIND"
# echo "OMP_PLACES=$OMP_PLACES"
# echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
# echo "OMP_NUM_THREADS=$OMP_NUM_THREADS"

# clang++ -fopenmp taskloop_mix.cpp -L/home/nvidia/project/OpenMP-Moldability/openmp/build/runtime/src/ -lomp -o taskloop_mix_new 

# # Define the parameter values
# sizes=(128 256)
# iterations=4
# for size in "${sizes[@]}"; do
#         for ((i=1; i<=iterations; i++)); do
# 		echo "/*---------------------------------------------------------------*/"
#                 echo "/*--------- Start running size $size, iteration $i---------------*/"
# 		./taskloop_mix_new "$size" 10000 > output_${size}_${i}.txt  2>> error.txt
#                 sleep 10
#         done
# done

# sizes=(512)
# for size in "${sizes[@]}"; do
#         for ((i=1; i<=iterations; i++)); do
# 		echo "/*---------------------------------------------------------------*/"
#                 echo "/*--------- Start running size $size, iteration $i---------------*/"
# 		./taskloop_mix_new "$size" 1000 > output_${size}_${i}.txt  2>> error.txt
#                 sleep 10
#         done
# done