int isprime(long n) {
    if (n < 2) return 0;
    for (long d = 2; d * d <= n; d++) {
        if (n % d == 0) return 0;
    }
    return 1;
}

int main(void) {
    long count = 0;
    for (long i = 2; i < 50000; i++) {
        count += isprime(i);
    }
    return (int)(count & 0xFF);
}
