#define _XOPEN_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* libalpm */
#include "alpm.h"
#include "alpm_list.h"
#include "handle.h"

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size);

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    if (Size == 0)
        return 0;

    alpm_handle_t handle;               // TODO/FIXME?
    const char* filename = "/dev/null"; // TODO/FIXME?

    alpm_list_t *keys = NULL;
    alpm_extract_keyid(&handle, filename, /* sig */ Data, /* len */ Size, &keys);

    return 0;
}
