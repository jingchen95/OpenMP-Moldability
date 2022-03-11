CPP :=clang++
CXXFLAGS= -O3 -std=c++17 -Wall -fopenmp 

all:
	$(CPP) $(CXXFLAGS) matmul.cpp  -o matmul
	$(CPP) $(CXXFLAGS) jacobi.cpp  -o jacobi
	$(CPP) $(CXXFLAGS) memcpy.cpp  -o memcpy
	$(CPP) $(CXXFLAGS) dotprod.cpp  -o dotprod
	$(MAKE) -C simple-vgg16

clean:
	rm -rf matmul
	rm -rf jacobi
	rm -rf memcpy
	rm -rf dotprod
	$(MAKE) -C simple-vgg16 clean
