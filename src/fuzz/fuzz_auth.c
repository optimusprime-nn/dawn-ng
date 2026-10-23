/*
 * libFuzzer target: dawn_auth_unwrap() on arbitrary bytes, plus a
 * wrap/unwrap round trip of the same bytes, which must always succeed.
 */
#include <gcrypt.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_auth.h"
#include "memory_utils.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static int initialised = 0;
    const time_t now = 1760000000;
    size_t plen = 0, wlen = 0;

    if (!initialised) {
        gcry_check_version(NULL);
        gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
        dawn_auth_set_key("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
        initialised = 1;
    }

    // Arbitrary input as a received message: must never crash
    const char *p = dawn_auth_unwrap((const char *)data, size, now, &plen, NULL);
    if (p && (p < (const char *)data || p + plen > (const char *)data + size))
        abort();

    // Anything we wrap must unwrap to itself
    char *w = dawn_auth_wrap((const char *)data, size, now, &wlen);
    if (w) {
        p = dawn_auth_unwrap(w, wlen, now, &plen, NULL);
        if (!p || plen != size || memcmp(p, data, size) != 0)
            abort();
        dawn_free(w);
    }

    return 0;
}
