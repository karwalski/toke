#include <stdlib.h>

long sum(long *arr, long len) {
    long s = 0;
    for (long i = 0; i < len; i++) {
        s += arr[i];
    }
    return s;
}

int main(void) {
    long a[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,
                21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                41,42,43,44,45,46,47,48,49,50};
    long total = 0;
    for (long r = 0; r < 100000; r++) {
        total += sum(a, 50);
    }
    return (int)(total & 0xFF);
}
