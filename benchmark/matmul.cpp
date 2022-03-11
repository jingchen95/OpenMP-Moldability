
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

#define R1 1<<7            // number of rows in Matrix-1
#define C1 1<<7            // number of columns in Matrix-1
#define R2 1<<7            // number of rows in Matrix-2
#define C2 1<<7            // number of columns in Matrix-2

struct read_format {
  uint64_t nr;
  struct {
    uint64_t value;
    uint64_t id;
  } values[];
};

void mulMat(const int mat1[][C1], const int mat2[][C2]) {
    int rslt[R1][C2];
 
 
    for (int i = 0; i < R1; i++) {
        for (int j = 0; j < C2; j++) {
            rslt[i][j] = 0;
 
            for (int k = 0; k < R2; k++) {
                rslt[i][j] += mat1[i][k] * mat2[k][j];
            }
        }
    }
}


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


    int mat1[R1][C1] = {};
 
    int mat2[R2][C2] = {};

    ioctl(fd1, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(fd1, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
  
	mulMat(mat1, mat2);

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
