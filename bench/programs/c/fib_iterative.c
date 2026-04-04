#include <stdlib.h>

long fib(long n) {
    long a = 0, b = 1;
    for (long i = 0; i < n; i++) {
        long tmp = a + b;
        a = b;
        b = tmp;
    }
    return a;
}

int main(void) {
    long s = 0;
    for (long i = 0; i < 1000000; i++) {
        s += fib(50);
    }
    return (int)(s & 0xFF);
}
