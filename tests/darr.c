#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>

int arr[128];

int main() {
    memset(arr, 0x3f, sizeof(arr));
    printf("%x\n", (unsigned int)arr);
    for (int i = 0; i < 1000; i ++) {
        printf("%d %x\n", i, arr[i%128]);
        sleep(1);
    }
    return 0;
}
