import numpy as np
from scipy.interpolate import interp1d
import matplotlib
import matplotlib.pyplot as plt
import sys


def parse_data(dir, file, samples):
    res_matrix = []
    for i in range(samples):
        res_matrix.append([])

    read_lines = 0
    with open(dir + "/" + file) as fp:
        while True:
            line = fp.readline()

            if not line:
                break
            read_lines += 1
            data = list(map(int, line.split()))
            data_arr = np.array(data, dtype=float)
            data_arr *= 100.0 / data_arr.max()

            for i in range(samples):
                res_matrix[i].append(data_arr[i])

    return res_matrix


def add_boilerplate(output_file, samples):

    output_file = open(output_file, "w")

    output_file.writelines(["\n", "\n"])
    output_file.write("PARAMETER  Resource_width\n")

    output_file.write("POINTS")
    for i in range(samples):
        output_file.write(" ({data})".format(data=i + 1))
    output_file.writelines(["\n", "\n"])

    output_file.close()


def export_data(region, output_file, parsed_data):
    output_file = open(output_file, "a")
    output_file.write("REGION {region}\n".format(region=region))
    output_file.write("METRIC  Execution_time\n")

    for entry in parsed_data:
        output_file.write("DATA")
        for data in entry:
            output_file.write(" {:.2f}".format(data))

        output_file.write("\n")

    output_file.write("\n")
    output_file.close()


if __name__ == '__main__':
    dir = sys.argv[1]
    samples = int(sys.argv[2])

    output = "PMNF_input.txt"

    add_boilerplate(output, samples)

    res = parse_data(dir, "CPU.txt", samples)
    export_data("CPU",output , res)

    res = parse_data(dir, "CACHE.txt", samples)
    export_data("CACHE", output, res)

    res = parse_data(dir, "MEMORY.txt", samples)
    export_data("MEMORY", output, res)
