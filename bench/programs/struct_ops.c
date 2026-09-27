typedef struct { long x, y, z; } Vec3;

Vec3 add(Vec3 a, Vec3 b) {
    return (Vec3){a.x + b.x, a.y + b.y, a.z + b.z};
}

long dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 scale(Vec3 v, long s) {
    return (Vec3){v.x * s, v.y * s, v.z * s};
}

/* keeps the components bounded: the 1M-iteration loop never overflows */
Vec3 wrap(Vec3 v) {
    return (Vec3){v.x % 1009, v.y % 1013, v.z % 1019};
}

int main(void) {
    Vec3 v = {1, 2, 3};
    long s = 0;
    for (long i = 0; i < 1000000; i++) {
        Vec3 w = scale(v, 3);
        v = wrap(add(v, w));
        s += dot(v, w);
    }
    return (int)(s & 0xFF);
}
