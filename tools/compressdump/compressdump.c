#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../kmod/dump.h"

int read_dump(struct dump *dump, int fd)
{
    size_t count;
    int ret;

    count = sizeof(struct dump);
    while (count) {
        ret = read(fd, dump + count, count);
        if (ret < 0) {
            perror("read");
            exit(0);
        }

        count -= ret; 
    }

    return ret;
}



int gather_dump(struct dump *dump, int fd)
{

    return read_dump(dump, fd);
}


int main(int argc, char *argv[])
{
    int *fds;
    int i;
    int error;
    char **inputs;
    int ninputs;
    struct dump *dump;


    if (argc < 2) {
        printf("Usage: ./compressdump infile [infiles]\n");
        exit(0);
    }

    inputs = &argv[1];
    ninputs = argc - 1;

    fds = malloc(sizeof(int) * ninputs);
    if (!fds) {
        perror("malloc");
        exit(0);
    }

    for (i = 0; i < ninputs; i++) {
        fds[i] = open(inputs[i], O_RDONLY);
        if (!fds[i]) {
            perror("open");
            exit(0);
        }
    }

    
    dump = malloc(sizeof(struct dump));
    if (!dump) {
        perror("malloc dump");
        exit(0);
    }
    
    for (i = 0; i < ninputs; i++) {
        gather_dump(dump, fds[i]);
    }

    return 0;
}
