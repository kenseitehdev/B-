#include <stdint.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wuninitialized"
static __attribute__((noinline)) uint32_t uninitialized_value(void) {
    volatile uint32_t value;
    return value;
}
#pragma clang diagnostic pop

int main(void) {
    return uninitialized_value() == 0;
}
