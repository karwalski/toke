long gcd(long a, long b) {
    long x = a, y = b;
    while (y > 0) {
        long tmp = y;
        y = x % y;
        x = tmp;
    }
    return x;
}

long euler_totient(long n) {
    long count = 0;
    for (long i = 1; i < n; i++) {
        if (gcd(i, n) == 1) count++;
    }
    return count;
}

int main(void) {
    long s = 0;
    for (long i = 2; i < 10000; i++) {
        s += euler_totient(i);
    }
    return (int)(s & 0xFF);
}
