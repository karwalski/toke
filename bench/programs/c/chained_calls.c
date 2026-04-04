long inc(long x) { return x + 1; }
long dbl(long x) { return x * 2; }
long sqr(long x) { return x * x; }
long sub3(long x) { return x - 3; }
long add7(long x) { return x + 7; }
long chain(long x) {
    return add7(sub3(sqr(dbl(inc(x)))));
}

int main(void) {
    long s = 0;
    for (long i = 0; i < 10000000; i++) {
        s += chain(i);
    }
    return (int)(s & 0xFF);
}
