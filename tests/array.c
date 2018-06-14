#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>

int arr[128];

int main() {
    printf("%d\n", getpid());
    memset(arr, 0x3f, sizeof(arr));
    printf("%x\n", (unsigned int)arr);
    sleep(10);
    for (int i = 0; i < 1000; i ++) {
        printf("%d %x\n", i, arr[i%128]);
    }
    return 0;
}
