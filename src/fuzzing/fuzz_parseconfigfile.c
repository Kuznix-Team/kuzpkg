#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <sys/mman.h>
#include <unistd.h>

// TODO/FIXME: Fix the util.h include
//#include "conf.h"
// And remove that function header from here
int parseconfigfile(const char *s);
extern void *config;
void *config_new(void);

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size);

// TODO/FIXME: This fuzzer should always be run from a chroot
// without any other files in it; otherwise the configfile may refer
// to other files
int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    static void* config_object = 0;

    // TODO/FIXME: The harness needs to be run with -detect_leaks=0
    // because the config object here is detected as a leak
    if (!config_object) {
        config = config_object = config_new();
    }

    if (Size == 0)
        return 0;

    int fd = memfd_create("input", 0);
    write(fd, Data, Size);

    char path[64] = {0};
    sprintf(path, "/proc/self/fd/%d", fd);

    parseconfigfile(path);

    close(fd);

    return 0;
}
