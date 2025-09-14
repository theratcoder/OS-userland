#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("Hello from YourOS init!\n");
    while (1) pause();
}