long collatz(long n) {
    long v = n;
    long steps = 0;
    while (v > 1) {
        if (v % 2 == 0) v = v / 2;
        else v = v * 3 + 1;
        steps++;
    }
    return steps;
}

int main(void) {
    long total = 0;
    for (long i = 1; i < 500000; i++) {
        total += collatz(i);
    }
    return (int)(total & 0xFF);
}
