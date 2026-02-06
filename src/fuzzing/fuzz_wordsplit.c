#define _XOPEN_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "util-common.h"

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size);

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    if (Size == 0)
        return 0;

    // Prepare a null terminated string
    char* cstring = malloc(Size+1);
    memcpy(cstring, Data, Size);
    cstring[Size] = 0;

    char** ptr = wordsplit(cstring);

    // Free the memory allocated by wordsplit
    if (ptr) {
        int i = 0;
        char* p = ptr[i++];
        while (p) {
            free(p);
            p = ptr[i++];
        }
        free(ptr);
    }

    // Free the allocated cstring
    free(cstring);

    return 0;
}
