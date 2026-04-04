int main(void) {
    long s = 0;
    for (long i = 0; i < 1000; i++) {
        for (long j = 0; j < 1000; j++) {
            s += i * j;
        }
    }
    return (int)(s & 0xFF);
}
