/* Isolated check using the same disk fixture and host stubs as drive_test.c. */
#define main drive_suite_main
#include "drive_test.c"
#undef main

int main(void)
{
    test_raw_d64_sector_origin();
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
