import os
import subprocess

matmul_input = [32, 64, 128, 256]
memory_input = [10000, 20000, 100000, 200000]
taskloop_input = [100000, 1000000, 10000000]
jacobi_input = [100, 500, 1000]

tests = [["./taskloop", taskloop_input],
         ["./taskloop_jacobi", jacobi_input],
         ["./taskloop_matmul", matmul_input],
         ["./taskloop_memory", memory_input],
         ["./taskloop_memory2", memory_input]]


def run_program(app, iter):
    print("Started benchmark {test}".format(test=app[0].strip("./")))
    for input in app[1]:
        for i in range(iter):
            subprocess.run(args=[app[0], "1000", str(input)], shell=False,
                           text=False,
                           capture_output=False, stdout=subprocess.DEVNULL)

    print("Done benchmarking {test}".format(test=app[0].strip("./")))


if __name__ == '__main__':
    os.environ['OMP_PLACES'] = 'threads'
    os.environ['OMP_PROC_BIND'] = 'true'
    for test in tests:
        run_program(test, 10)
