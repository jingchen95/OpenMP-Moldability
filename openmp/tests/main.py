import numpy as np
from scipy.interpolate import interp1d
import matplotlib
import matplotlib.pyplot as plt
import sys

matplotlib.use('TkAgg')


def avg(arr1, arr2):
    arr = []
    limit = min(len(arr1), len(arr2))

    for i in range(limit):
        if arr1[i] == 0 or arr2[i] == 0:
            continue
        else:
            arr.append(arr1[i] / arr2[i])
    arr = np.array(arr)

    return np.average(arr)


def read_data(dir, file, samples):
    res = np.array([0] * samples)

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
            data_arr = data_arr.astype(int)
            res += data_arr

    y = (res / read_lines).astype(int)
    return y


def make_model(x, y):
    model = np.poly1d(np.polyfit(x, y, 3))
    return model


def predict(model, width, samples_vector, data):
    return model(width) * avg(samples_vector, data)


if __name__ == '__main__':
    dir = sys.argv[1];
    samples = int(sys.argv[2])

    data_cpu = read_data(dir, "CPU.txt", samples)
    x = np.array(range(1, samples + 1))
    model_cpu = make_model(x, data_cpu)

    print(str(model_cpu))
    x_model = np.linspace(0, samples, 50)
    ynew = model_cpu(x_model)
    plt.plot(x, data_cpu, 'o', x_model, ynew, '-', )
    plt.ylabel(str(model_cpu).strip())
    plt.show()

# See PyCharm help at https://www.jetbrains.com/help/pycharm/
