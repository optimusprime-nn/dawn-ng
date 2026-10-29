#include <gcrypt.h>
#include <string.h>

#include "dawn_auth.h"
#include "memory_utils.h"
#include "utils.h"

static unsigned char auth_key[DAWN_AUTH_KEY_LEN];
static unsigned char enc_key[DAWN_AUTH_KEY_LEN];
static bool auth_on = false;
static bool encrypt_on = false;

#define ENC_KEY_LABEL "DAWN peer encryption v1"

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
    memset(enc_key, 0, sizeof(enc_key));

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

    // Separate key for encryption, so the HMAC key is never used as a cipher key
    gcry_md_hd_t h;
    if (gcry_md_open(&h, GCRY_MD_SHA256, GCRY_MD_FLAG_HMAC))
        return DAWN_AUTH_KEY_INVALID;
    if (gcry_md_setkey(h, key, sizeof(key))) {
        gcry_md_close(h);
        return DAWN_AUTH_KEY_INVALID;
    }
    gcry_md_write(h, ENC_KEY_LABEL, strlen(ENC_KEY_LABEL));
    memcpy(enc_key, gcry_md_read(h, GCRY_MD_SHA256), sizeof(enc_key));
    gcry_md_close(h);

    memcpy(auth_key, key, sizeof(auth_key));
    auth_on = true;
    return DAWN_AUTH_KEY_SET;
}

bool dawn_auth_enabled(void) {
    return auth_on;
}

void dawn_auth_set_encrypt(bool on) {
    encrypt_on = on;
}

bool dawn_auth_encrypting(void) {
    return auth_on && encrypt_on;
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

static gcry_cipher_hd_t gcm_open(const unsigned char *nonce, const unsigned char *magic_ts) {
    gcry_cipher_hd_t h;

    if (gcry_cipher_open(&h, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, 0))
        return NULL;
    if (gcry_cipher_setkey(h, enc_key, sizeof(enc_key)) ||
            gcry_cipher_setiv(h, nonce, DAWN_ENC_NONCE_LEN) ||
            gcry_cipher_authenticate(h, magic_ts, DAWN_AUTH_MAGIC_LEN + DAWN_AUTH_TS_LEN)) {
        gcry_cipher_close(h);
        return NULL;
    }
    return h;
}

static char *encrypt_wrap(const char *payload, size_t len, time_t now, size_t *out_len) {
    if (len > SIZE_MAX - DAWN_ENC_OVERHEAD)
        return NULL;

    unsigned char *out = dawn_malloc(DAWN_ENC_OVERHEAD + len);
    if (!out)
        return NULL;

    unsigned char *nonce = out + DAWN_AUTH_MAGIC_LEN + DAWN_AUTH_TS_LEN;
    unsigned char *ct = out + DAWN_ENC_HDR_LEN;

    memcpy(out, DAWN_ENC_MAGIC, DAWN_AUTH_MAGIC_LEN);
    put_be64(out + DAWN_AUTH_MAGIC_LEN, (uint64_t)now);
    gcry_create_nonce(nonce, DAWN_ENC_NONCE_LEN);

    gcry_cipher_hd_t h = gcm_open(nonce, out);
    if (!h || gcry_cipher_encrypt(h, ct, len, payload, len) ||
            gcry_cipher_gettag(h, ct + len, DAWN_ENC_TAG_LEN)) {
        if (h)
            gcry_cipher_close(h);
        dawn_free(out);
        return NULL;
    }
    gcry_cipher_close(h);

    *out_len = DAWN_ENC_OVERHEAD + len;
    return (char *)out;
}

char *dawn_auth_wrap(const char *payload, size_t len, time_t now, size_t *out_len) {
    if (!auth_on || !payload || len > SIZE_MAX - DAWN_AUTH_HDR_LEN)
        return NULL;

    if (encrypt_on)
        return encrypt_wrap(payload, len, now, out_len);

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

static bool ts_ok(const unsigned char *b, time_t now) {
    // The timestamp is attacker controlled: compare without signed
    // arithmetic, which overflowed for values near INT64_MIN (found by
    // fuzz_auth).
    uint64_t ts = get_be64(b + DAWN_AUTH_MAGIC_LEN);
    uint64_t unow = now > 0 ? (uint64_t)now : 0;
    uint64_t skew = ts > unow ? ts - unow : unow - ts;

    return skew <= DAWN_AUTH_MAX_SKEW;
}

char *dawn_auth_unwrap(char *buf, size_t len, time_t now, size_t *payload_len, const char **reason) {
    unsigned char *b = (unsigned char *)buf;
    unsigned char expected[DAWN_AUTH_TAG_LEN];
    const char *why = NULL;

    if (!auth_on)
        why = "authentication not configured";
    else if (buf && len >= DAWN_ENC_OVERHEAD && memcmp(b, DAWN_ENC_MAGIC, DAWN_AUTH_MAGIC_LEN) == 0) {
        // Encrypted
        size_t ct_len = len - DAWN_ENC_OVERHEAD;
        unsigned char *ct = b + DAWN_ENC_HDR_LEN;

        if (!ts_ok(b, now))
            why = "timestamp outside allowed window (clocks out of sync, or replay)";
        else {
            gcry_cipher_hd_t h = gcm_open(b + DAWN_AUTH_MAGIC_LEN + DAWN_AUTH_TS_LEN, b);
            if (!h)
                why = "cipher failure";
            else {
                // In place; the plaintext is only used if the tag checks out
                if (gcry_cipher_decrypt(h, ct, ct_len, NULL, 0) ||
                        gcry_cipher_checktag(h, ct + ct_len, DAWN_ENC_TAG_LEN))
                    why = "bad tag (different auth_key?)";
                gcry_cipher_close(h);
            }
        }

        if (!why) {
            ct[ct_len] = '\0'; // first byte of the (checked) tag
            *payload_len = ct_len;
            return (char *)ct;
        }
    }
    else if (!buf || len < DAWN_AUTH_HDR_LEN || memcmp(b, DAWN_AUTH_MAGIC, DAWN_AUTH_MAGIC_LEN) != 0)
        why = "message is not authenticated (peer has no auth_key?)";
    else {
        if (!ts_ok(b, now))
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
