SOURCES = $(wildcard *.cpp)
EXECS = $(SOURCES:%.cpp=%)
CXXFLAGS :=  -O3 -fopenmp -std=c++17 -Wall
CXX := clang++


all: $(EXECS)

clean:
	rm -f $(EXECS)
