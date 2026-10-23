#include <gcrypt.h>
#include <string.h>

#include "dawn_auth.h"
#include "memory_utils.h"
#include "utils.h"

static unsigned char auth_key[DAWN_AUTH_KEY_LEN];
static bool auth_on = false;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

enum dawn_auth_key_status dawn_auth_set_key(const char *hex_key) {
    unsigned char key[DAWN_AUTH_KEY_LEN];

    auth_on = false;
    memset(auth_key, 0, sizeof(auth_key));

    if (!hex_key || !*hex_key)
        return DAWN_AUTH_KEY_NONE;

    if (strlen(hex_key) != DAWN_AUTH_KEY_HEX_LEN)
        return DAWN_AUTH_KEY_INVALID;

    for (int i = 0; i < DAWN_AUTH_KEY_LEN; i++) {
        int hi = hexval(hex_key[2 * i]), lo = hexval(hex_key[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return DAWN_AUTH_KEY_INVALID;
        key[i] = (unsigned char)(hi << 4 | lo);
    }

    memcpy(auth_key, key, sizeof(auth_key));
    auth_on = true;
    return DAWN_AUTH_KEY_SET;
}

bool dawn_auth_enabled(void) {
    return auth_on;
}

static void put_be64(unsigned char *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (unsigned char)(v & 0xff);
        v >>= 8;
    }
}

static uint64_t get_be64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

// tag = HMAC-SHA256(key, magic | ts | payload)
static int compute_tag(const unsigned char *magic_ts, const char *payload, size_t len, unsigned char *tag) {
    gcry_md_hd_t h;

    if (gcry_md_open(&h, GCRY_MD_SHA256, GCRY_MD_FLAG_HMAC))
        return -1;

    if (gcry_md_setkey(h, auth_key, sizeof(auth_key))) {
        gcry_md_close(h);
        return -1;
    }

    gcry_md_write(h, magic_ts, DAWN_AUTH_MAGIC_LEN + DAWN_AUTH_TS_LEN);
    gcry_md_write(h, payload, len);
    memcpy(tag, gcry_md_read(h, GCRY_MD_SHA256), DAWN_AUTH_TAG_LEN);
    gcry_md_close(h);
    return 0;
}

char *dawn_auth_wrap(const char *payload, size_t len, time_t now, size_t *out_len) {
    if (!auth_on || !payload || len > SIZE_MAX - DAWN_AUTH_HDR_LEN)
        return NULL;

    unsigned char *out = dawn_malloc(DAWN_AUTH_HDR_LEN + len);
    if (!out)
        return NULL;

    memcpy(out, DAWN_AUTH_MAGIC, DAWN_AUTH_MAGIC_LEN);
    put_be64(out + DAWN_AUTH_MAGIC_LEN, (uint64_t)now);

    if (compute_tag(out, payload, len, out + DAWN_AUTH_MAGIC_LEN + DAWN_AUTH_TS_LEN)) {
        dawn_free(out);
        return NULL;
    }

    memcpy(out + DAWN_AUTH_HDR_LEN, payload, len);
    *out_len = DAWN_AUTH_HDR_LEN + len;
    return (char *)out;
}

const char *dawn_auth_unwrap(const char *buf, size_t len, time_t now, size_t *payload_len, const char **reason) {
    const unsigned char *b = (const unsigned char *)buf;
    unsigned char expected[DAWN_AUTH_TAG_LEN];
    const char *why = NULL;

    if (!auth_on)
        why = "authentication not configured";
    else if (!buf || len < DAWN_AUTH_HDR_LEN || memcmp(b, DAWN_AUTH_MAGIC, DAWN_AUTH_MAGIC_LEN) != 0)
        why = "message is not authenticated (peer has no auth_key?)";
    else {
        // The timestamp is attacker controlled: compare without signed
        // arithmetic, which overflowed for values near INT64_MIN (found by
        // fuzz_auth).
        uint64_t ts = get_be64(b + DAWN_AUTH_MAGIC_LEN);
        uint64_t unow = now > 0 ? (uint64_t)now : 0;
        uint64_t skew = ts > unow ? ts - unow : unow - ts;

        if (skew > DAWN_AUTH_MAX_SKEW)
            why = "timestamp outside allowed window (clocks out of sync, or replay)";
        else if (compute_tag(b, buf + DAWN_AUTH_HDR_LEN, len - DAWN_AUTH_HDR_LEN, expected))
            why = "HMAC failure";
        else {
            // Constant time comparison
            unsigned char diff = 0;
            for (int i = 0; i < DAWN_AUTH_TAG_LEN; i++)
                diff |= expected[i] ^ b[DAWN_AUTH_MAGIC_LEN + DAWN_AUTH_TS_LEN + i];
            if (diff)
                why = "bad tag (different auth_key?)";
        }
    }

    if (why) {
        if (reason)
            *reason = why;
        return NULL;
    }

    *payload_len = len - DAWN_AUTH_HDR_LEN;
    return buf + DAWN_AUTH_HDR_LEN;
}
