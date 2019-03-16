import sys

import matplotlib.pyplot as plt

timecodes = {   0 : "suspend", 
                1 : "cpuckpt", 
                2 : "memckpt", 
                3 : "fdckpt", 
                4 : "resume", 
                5 : "dump", 
                6 : "total",
                7 : "dumpsize",
                8 : "clflush",
            }

timevalues = {index : [] for index in range(9)}

def plot():
    if len(sys.argv) != 2:
        print("Usage: graph.py <filename>")
        sys.exit(0)
    with open(sys.argv[1]) as infile:
        numbers = dict()

        line = infile.readline()
        while line:
            # Assume valid input
            code, value = map(int, line.split(","))
            if not code in timecodes.keys():
                print("Invalid code for value")
                sys.exit(0)
            timevalues[code].append(value)
            line = infile.readline()
            
    plt.legend(loc="best")
    for key in range(7):
        #print(timecodes[key] + "\t" + str(timevalues[key]))
        print(len(timevalues[key]))
        plt.subplot(3, 4, key + 1)
        plt.plot(list(range(len(timevalues[key]))), 
                list(map(lambda x: x // 1000, timevalues[key])), 'ro')
        plt.ylabel("Time (us)")
        plt.title("Time series of cost for {}".format(timecodes[key]))

    key = 7
    plt.subplot(3, 4, key + 1)
    plt.plot(list(range(len(timevalues[key]))), 
            list(map(lambda x: x // (1024 * 1024), timevalues[key])), 'ro')
    plt.ylabel("Amount written (MB)")
    plt.title("Time series for {}".format(timecodes[key]))

    
    plt.subplot(3, 4, 9)
    gigabytes_per_second = map(lambda pair: (pair[2] // (pair[0] - pair[1])) * pow(1.024, 3), zip(timevalues[6], timevalues[0], timevalues[7]))
    plt.title("Throughput (GB/s)")
    plt.plot(list(range(len(timevalues[6]))), list(gigabytes_per_second), 'ro')


    key = 8
    plt.subplot(3, 4, 10)
    plt.subplot(3, 4, 10)
    plt.ylabel("Time (us)")
    plt.title("Time spent waiting for the workers")
    plt.plot(list(range(len(timevalues[key]))), 
             list(map(lambda x: x // 1000, timevalues[key])), 'ro')

    plt.savefig("intelbox.png")
    plt.show()

if __name__ == "__main__":
    plot()
