#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// TODO/FIXME: Fix the util.h include
//#include "util.h"
// And remove that function header from here
size_t string_length(const char *s);

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size);

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    if (Size == 0)
        return 0;

    // Prepare a null terminated string
    char* cstring = malloc(Size+1);
    memcpy(cstring, Data, Size);
    cstring[Size] = 0;

    string_length(cstring);

    free(cstring);

    return 0;
}
