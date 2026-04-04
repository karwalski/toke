long bsearch_idx(long *arr, long len, long target) {
    long lo = 0, hi = len - 1;
    while (lo <= hi) {
        long mid = lo + (hi - lo) / 2;
        if (arr[mid] < target) lo = mid + 1;
        else if (arr[mid] > target) hi = mid - 1;
        else return mid;
    }
    return -1;
}

int main(void) {
    long a[] = {2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50};
    long found = 0;
    for (long r = 0; r < 1000000; r++) {
        found += bsearch_idx(a, 25, 30);
    }
    return (int)(found & 0xFF);
}
