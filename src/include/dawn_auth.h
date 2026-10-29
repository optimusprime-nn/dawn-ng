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
 * (plain JSON, or ciphertext when the legacy use_symm_enc is on).
 *
 * With encryption on (network.encrypt_peers), messages are instead sent as
 *
 *     "DWE1" | timestamp (8 bytes) | nonce (12 bytes) | ciphertext | GCM tag (16 bytes)
 *
 * using AES-256-GCM with a key derived from auth_key
 * (HMAC-SHA256(auth_key, "DAWN peer encryption v1")), a random nonce per
 * message, and the magic and timestamp as additional authenticated data.
 * The same timestamp window applies. A receiver with a key accepts both
 * forms, so encryption can be switched on one access point at a time.
 */

#define DAWN_AUTH_MAGIC "DWA1"
#define DAWN_AUTH_MAGIC_LEN 4
#define DAWN_AUTH_TS_LEN 8
#define DAWN_AUTH_TAG_LEN 32
#define DAWN_AUTH_HDR_LEN (DAWN_AUTH_MAGIC_LEN + DAWN_AUTH_TS_LEN + DAWN_AUTH_TAG_LEN)
#define DAWN_AUTH_KEY_LEN 32
#define DAWN_AUTH_KEY_HEX_LEN (2 * DAWN_AUTH_KEY_LEN)
#define DAWN_AUTH_MAX_SKEW 300

#define DAWN_ENC_MAGIC "DWE1"
#define DAWN_ENC_NONCE_LEN 12
#define DAWN_ENC_TAG_LEN 16
#define DAWN_ENC_HDR_LEN (DAWN_AUTH_MAGIC_LEN + DAWN_AUTH_TS_LEN + DAWN_ENC_NONCE_LEN)
#define DAWN_ENC_OVERHEAD (DAWN_ENC_HDR_LEN + DAWN_ENC_TAG_LEN)

enum dawn_auth_key_status {
    DAWN_AUTH_KEY_NONE = 0,     // no key configured: authentication off
    DAWN_AUTH_KEY_SET = 1,      // key accepted: authentication on
    DAWN_AUTH_KEY_INVALID = -1  // key present but malformed: authentication off
};

/** Configure the shared key from its hex form. NULL or "" disables authentication. */
enum dawn_auth_key_status dawn_auth_set_key(const char *hex_key);

bool dawn_auth_enabled(void);

/** Encrypt outgoing messages (only has an effect while a key is set). */
void dawn_auth_set_encrypt(bool on);

/** True if outgoing messages are encrypted: a key is set and encryption is on. */
bool dawn_auth_encrypting(void);

/**
 * Authenticate (or, with encryption on, encrypt) payload for sending.
 * Returns a newly allocated buffer (dawn_free() it) of *out_len bytes, or NULL.
 */
char *dawn_auth_wrap(const char *payload, size_t len, time_t now, size_t *out_len);

/**
 * Check a received message, authenticated or encrypted. On success returns a
 * pointer to the payload inside buf and sets *payload_len; on failure returns
 * NULL and, if reason is not NULL, points it at a short description for
 * logging. Encrypted messages are decrypted in place (buf is modified), and
 * the decrypted payload is followed by a '\0'.
 */
char *dawn_auth_unwrap(char *buf, size_t len, time_t now, size_t *payload_len, const char **reason);

#endif
