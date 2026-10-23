#ifndef DAWN_AUTH_H
#define DAWN_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/*
 * Authentication of messages between DAWN instances.
 *
 * Every DAWN instance in a network is given the same 256 bit key
 * (network.auth_key, 64 hex characters). Each TCP message is then sent as
 *
 *     "DWA1" | timestamp (8 bytes, big endian seconds) | HMAC-SHA256 tag | payload
 *
 * where the tag covers the magic, the timestamp and the payload. A receiver
 * with a key drops anything without a valid tag, or whose timestamp is more
 * than DAWN_AUTH_MAX_SKEW seconds from its own clock (limits replays; the
 * access points need roughly synchronised clocks, which NTP provides).
 *
 * The payload is whatever would have been sent without authentication
 * (plain JSON, or ciphertext when use_symm_enc is on).
 */

#define DAWN_AUTH_MAGIC "DWA1"
#define DAWN_AUTH_MAGIC_LEN 4
#define DAWN_AUTH_TS_LEN 8
#define DAWN_AUTH_TAG_LEN 32
#define DAWN_AUTH_HDR_LEN (DAWN_AUTH_MAGIC_LEN + DAWN_AUTH_TS_LEN + DAWN_AUTH_TAG_LEN)
#define DAWN_AUTH_KEY_LEN 32
#define DAWN_AUTH_KEY_HEX_LEN (2 * DAWN_AUTH_KEY_LEN)
#define DAWN_AUTH_MAX_SKEW 300

enum dawn_auth_key_status {
    DAWN_AUTH_KEY_NONE = 0,     // no key configured: authentication off
    DAWN_AUTH_KEY_SET = 1,      // key accepted: authentication on
    DAWN_AUTH_KEY_INVALID = -1  // key present but malformed: authentication off
};

/** Configure the shared key from its hex form. NULL or "" disables authentication. */
enum dawn_auth_key_status dawn_auth_set_key(const char *hex_key);

bool dawn_auth_enabled(void);

/**
 * Prefix payload with the authentication header.
 * Returns a newly allocated buffer (dawn_free() it) of *out_len bytes, or NULL.
 */
char *dawn_auth_wrap(const char *payload, size_t len, time_t now, size_t *out_len);

/**
 * Check buf's header and tag. On success returns a pointer to the payload
 * inside buf and sets *payload_len; on failure returns NULL and, if reason is
 * not NULL, points it at a short description for logging.
 */
const char *dawn_auth_unwrap(const char *buf, size_t len, time_t now, size_t *payload_len, const char **reason);

#endif
