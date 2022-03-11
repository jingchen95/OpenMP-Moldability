
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <asm/unistd.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <cstdint>

#define N 1000

struct read_format {
  uint64_t nr;
  struct {
    uint64_t value;
    uint64_t id;
  } values[];
};


int main(int argc, char* argv[]) {
    struct perf_event_attr pea;
    int fd1, fd2;
    uint64_t id1, id2;
    uint64_t val1, val2;
    char buf[4096];
    struct read_format* rf = (struct read_format*) buf;
    int i;

    memset(&pea, 0, sizeof(struct perf_event_attr));
    pea.type = PERF_TYPE_HARDWARE;
    pea.size = sizeof(struct perf_event_attr);
    pea.config = PERF_COUNT_HW_CPU_CYCLES;
    pea.disabled = 1;
    pea.exclude_kernel = 1;
    pea.exclude_hv = 1;
    pea.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
    fd1 = syscall(__NR_perf_event_open, &pea, 0, -1, -1, 0);
    ioctl(fd1, PERF_EVENT_IOC_ID, &id1);

    memset(&pea, 0, sizeof(struct perf_event_attr));
    pea.type = PERF_TYPE_HARDWARE;
    pea.size = sizeof(struct perf_event_attr);
    pea.config = PERF_COUNT_HW_CACHE_MISSES;
    pea.disabled = 1;
    pea.exclude_kernel = 1;
    pea.exclude_hv = 1;
    pea.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
    fd2 = syscall(__NR_perf_event_open, &pea, 0, -1, fd1 /*!!!*/, 0);
    ioctl(fd2, PERF_EVENT_IOC_ID, &id2);

    #define THRESHOLD 10.0

    double maxDiff;
    double **inMatrix = (double**)malloc(N * sizeof(double*));
    for (i = 0; i < N; i++){
        inMatrix[i] = (double*)malloc(N * sizeof(double));
    }


	double **outMatrix = (double**)malloc(N * sizeof(double*));
    for (i = 0; i < N; i++){
        outMatrix[i] = (double*)malloc(N * sizeof(double));
    }

	
	int count = 0;
    for (i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            inMatrix[i][j] = (double)++count;


    ioctl(fd1, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(fd1, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
  
    // Outer-Loop - Keep doing iterations
    do {

    // Reset maxDiff
    maxDiff = 0.0;

    // Update all values in the matrix ("inner-loop")
        for (int x = 0; x < N; x++) {
            for (int y = 0; y < N; y++) {

            // Start with "self" value
                double sum = inMatrix[x][y];
                int divisor = 1;

          // Add the neighbors' values to average calculation
                if (x > 0)     { sum += inMatrix[x - 1][y]; divisor++; }
                if (x < N - 1) { sum += inMatrix[x + 1][y]; divisor++; }
                if (y > 0)     { sum += inMatrix[x][y - 1]; divisor++; }
                if (y < N - 1) { sum += inMatrix[x][y + 1]; divisor++; }

          // Calculate new value, difference, and update maxDiff
                outMatrix[x][y] = sum / ((double)divisor);
                double diff = abs(inMatrix[x][y] - outMatrix[x][y]);
                if (diff > maxDiff) maxDiff = diff;
            }
        }

      // Swap in and out
        double** tmp = inMatrix;
        inMatrix = outMatrix;
        outMatrix = tmp;


    } while (maxDiff > THRESHOLD);

    ioctl(fd1, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    read(fd1, buf, sizeof(buf));

    for (i = 0; i < rf->nr; i++) {
        if (rf->values[i].id == id1) {
            val1 = rf->values[i].value;
        } else if (rf->values[i].id == id2) {
            val2 = rf->values[i].value;
        }
    }

  //printf("cpu cycles: %" PRIu64 "\n", val1);
  //printf("cache misses: %" PRIu64 "\n", val2);

	if (val2 == 0) val2 = 1;
	int AI = (val1 *30)/(val2 * 64);
	printf("%d\n", AI);

    return 0;
}
