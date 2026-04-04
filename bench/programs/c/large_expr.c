long compute(long x) {
    return x*x + x*3 - x/2 + x*x*x - x*5 + x*7 - x*x + x*11 - x*13 + x*17 + x*x*2 - x*19 + x*23;
}

int main(void) {
    long s = 0;
    for (long i = 1; i < 5000000; i++) {
        s += compute(i);
    }
    return (int)(s & 0xFF);
}
