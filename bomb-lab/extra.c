#include <stdio.h>

int a, b, c, d, e, f;

int func100(int x, int y, int z) {
    return x % y + z;
}

int main() {
    scanf("%d %d %d %d %d %d", &a, &b, &c);
    printf("%d", func100(a, b, c));
    return 0;
}