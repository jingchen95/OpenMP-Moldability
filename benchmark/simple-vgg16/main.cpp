#include <iostream>
#include <memory>
#include "Tensor.hpp"
#include "Layer.hpp"
#include "Network.hpp"
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


#ifdef BENCHMARK
long long int MEM, PARAM, MAC, MEMALL = 0, PARAMALL = 0, MACALL = 0;
#endif

struct read_format {
  uint64_t nr;
  struct {
    uint64_t value;
    uint64_t id;
  } values[];
};

int main()
{
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

/*
    std::cout << "The VGG16 Net" << std::endl;
#ifdef BENCHMARK
    std::cout << "-----------------------------" << std::endl;
    std::cout << "NAME:\tMEM\tPARAM\tMAC" << std::endl;
    std::cout << "-----------------------------" << std::endl;
#endif
*/
    // prepare the input data
    auto input = sv::Tensor<double>(224, 224, 3); // 224 x 224 x 3
    input.randam();

    // prepare the network
    auto network = sv::Network<double>();

    // add layers into the network
    auto *layer1_1 = new sv::ConvLayer<double>(3, 3, 64); //224 x 224 x 64
    network.addLayer(layer1_1);
    auto *layer1_2 = new sv::ConvLayer<double>(64, 3, 64); //224 x 224 x 64
    network.addLayer(layer1_2);
    auto *layer1_3 = new sv::MaxPoolLayer<double>(2, 2); // 112 x 112 x 64
    network.addLayer(layer1_3);

    auto *layer2_1 = new sv::ConvLayer<double>(64, 3, 128); // 112 x 112 x 128
    network.addLayer(layer2_1);
    auto *layer2_2 = new sv::ConvLayer<double>(128, 3, 128); // 112 x 112 x 128
    network.addLayer(layer2_2);
    auto *layer2_3 = new sv::MaxPoolLayer<double>(2, 2); // 56 x 56 x 128
    network.addLayer(layer2_3);

    auto *layer3_1 = new sv::ConvLayer<double>(128, 3, 256); // 56 x 56 x 256
    network.addLayer(layer3_1);
    auto *layer3_2 = new sv::ConvLayer<double>(256, 3, 256); // 56 x 56 x 256
    network.addLayer(layer3_2);
    auto *layer3_3 = new sv::MaxPoolLayer<double>(2, 2); // 28 x 28 x 256
    network.addLayer(layer3_3);

    auto *layer4_1 = new sv::ConvLayer<double>(256, 3, 512); // 28 x 28 x 512
    network.addLayer(layer4_1);
    auto *layer4_2 = new sv::ConvLayer<double>(512, 3, 512); // 28 x 28 x 512
    network.addLayer(layer4_2);
    auto *layer4_3 = new sv::MaxPoolLayer<double>(2, 2); // 14 x 14 x 512
    network.addLayer(layer4_3);

    auto *layer5_1 = new sv::ConvLayer<double>(512, 3, 512); // 14 x 14 x 512
    network.addLayer(layer5_1);
    auto *layer5_2 = new sv::ConvLayer<double>(512, 3, 512); // 14 x 14 x 512
    network.addLayer(layer5_2);
    auto *layer5_3 = new sv::MaxPoolLayer<double>(2, 2); // 7 x 7 x 512
    network.addLayer(layer5_3);

    auto *layer6_1 = new sv::FCLayer<double>(7 * 7 * 512, 4096); // 1 x 1 x 4096
    network.addLayer(layer6_1);
    auto *layer6_2 = new sv::FCLayer<double>(4096, 4096); // 1 x 1 x 4096
    network.addLayer(layer6_2);
    auto *layer6_3 = new sv::FCLayer<double>(4096, 1000); // 1 x 1 x 1000
    network.addLayer(layer6_3);

  ioctl(fd1, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
  ioctl(fd1, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);

    // predict the result by forwarding
    sv::Tensor<double> output;
    network.predict(input, output);
    
    ioctl(fd1, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);

/*
#ifdef BENCHMARK
    std::cout << "Total:\t" << MEMALL / 1000000 << "Mb,\t"
              << PARAMALL / 1000000 << "M,\t" << MACALL / 1000000 << "M" << std::endl;
    std::cout << "-----------------------------" << std::endl;
#endif

    // show the result
    std::cout << "@@ result @@" << std::endl;
    for (int i = 0; i < 5; i++)
        std::cout << output[i] << " ";
    std::cout << "... so many ... ";
    for (int i = 995; i < 1000; i++)
        std::cout << output[i] << " ";
    std::cout << std::endl;
*/

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
