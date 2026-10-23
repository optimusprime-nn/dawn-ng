/* Unit tests for dawn_auth (peer message authentication). */
#include <stdio.h>
#include <string.h>
#include <gcrypt.h>

#include "dawn_auth.h"
#include "memory_utils.h"

static int failures = 0;
#define CHECK(cond, what) do { if (cond) printf("  ok - %s\n", what); \
    else { printf("  FAIL - %s (line %d)\n", what, __LINE__); failures++; } } while (0)

static const char *KEY_A = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
static const char *KEY_B = "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";

int main(void) {
    const char msg[] = "{\"method\":\"probe\",\"data\":\"{}\"}";
    size_t len = sizeof(msg); /* includes NUL, as on the wire */
    time_t now = 1760000000;
    size_t wlen = 0, plen = 0;
    const char *reason = NULL;

    gcry_check_version(NULL);
    gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

    CHECK(dawn_auth_set_key(NULL) == DAWN_AUTH_KEY_NONE && !dawn_auth_enabled(), "no key: disabled");
    CHECK(dawn_auth_set_key("abc") == DAWN_AUTH_KEY_INVALID && !dawn_auth_enabled(), "short key rejected");
    CHECK(dawn_auth_set_key("zz112233445566778899aabbccddeeff00112233445566778899aabbccddeeff") == DAWN_AUTH_KEY_INVALID,
          "non-hex key rejected");
    CHECK(dawn_auth_wrap(msg, len, now, &wlen) == NULL, "wrap refuses without key");

    CHECK(dawn_auth_set_key(KEY_A) == DAWN_AUTH_KEY_SET && dawn_auth_enabled(), "valid key accepted");

    char *w = dawn_auth_wrap(msg, len, now, &wlen);
    CHECK(w && wlen == len + DAWN_AUTH_HDR_LEN && !memcmp(w, "DWA1", 4), "wrap adds header");

    const char *p = dawn_auth_unwrap(w, wlen, now, &plen, &reason);
    CHECK(p && plen == len && !memcmp(p, msg, len), "round trip");
    CHECK(dawn_auth_unwrap(w, wlen, now + 299, &plen, NULL) != NULL, "accepted within skew window");
    CHECK(dawn_auth_unwrap(w, wlen, now - 299, &plen, NULL) != NULL, "accepted slightly in the future");
    CHECK(dawn_auth_unwrap(w, wlen, now + 301, &plen, &reason) == NULL && strstr(reason, "timestamp"), "replay / old rejected");
    CHECK(dawn_auth_unwrap(w, wlen, now - 301, &plen, NULL) == NULL, "far future rejected");

    w[wlen - 3] ^= 1;
    CHECK(dawn_auth_unwrap(w, wlen, now, &plen, &reason) == NULL && strstr(reason, "tag"), "tampered payload rejected");
    w[wlen - 3] ^= 1;

    {
        /* Timestamps near the int64 limits must be rejected without signed
         * overflow (found by fuzz_auth; UBSan flags the old code). */
        char forged[DAWN_AUTH_HDR_LEN + 4] = "DWA1";
        memset(forged + 4, 0x80, 1);
        memset(forged + 5, 0x00, 7);
        CHECK(dawn_auth_unwrap(forged, sizeof(forged), now, &plen, &reason) == NULL && strstr(reason, "timestamp"),
              "timestamp 0x8000000000000000 rejected");
        memset(forged + 4, 0xff, 8);
        CHECK(dawn_auth_unwrap(forged, sizeof(forged), now, &plen, NULL) == NULL, "timestamp 0xffffffffffffffff rejected");
    }

    w[8] ^= 1; /* timestamp byte: changes the MAC input and the time */
    CHECK(dawn_auth_unwrap(w, wlen, now, &plen, NULL) == NULL, "tampered timestamp rejected");
    w[8] ^= 1;

    CHECK(dawn_auth_unwrap(w, DAWN_AUTH_HDR_LEN - 1, now, &plen, NULL) == NULL, "truncated header rejected");
    CHECK(dawn_auth_unwrap(w, DAWN_AUTH_HDR_LEN, now, &plen, NULL) == NULL, "header without payload rejected (tag mismatch)");
    CHECK(dawn_auth_unwrap(msg, len, now, &plen, &reason) == NULL && strstr(reason, "not authenticated"), "plain message rejected");

    dawn_auth_set_key(KEY_B);
    CHECK(dawn_auth_unwrap(w, wlen, now, &plen, &reason) == NULL && strstr(reason, "tag"), "wrong key rejected");

    dawn_auth_set_key("");
    CHECK(dawn_auth_unwrap(w, wlen, now, &plen, NULL) == NULL && !dawn_auth_enabled(), "key removed: disabled");

    dawn_free(w);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "test_auth passed", failures);
    return failures ? 1 : 0;
}
