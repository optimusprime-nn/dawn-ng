// based on:
// https://github.com/vedantk/gcrypt-example/blob/master/gcry.cc

#include <gcrypt.h>
#include <limits.h>
#include <string.h>

#include "utils.h"
#include "memory_utils.h"
#include "crypto.h"

#define GCRY_CIPHER GCRY_CIPHER_AES128   // Pick the cipher here
#define GCRY_C_MODE GCRY_CIPHER_MODE_ECB // Pick the cipher mode here

gcry_error_t gcry_error_handle;
gcry_cipher_hd_t gcry_cipher_hd;

void gcrypt_init() {
    if (!gcry_check_version(GCRYPT_VERSION)) {
        dawnlog_error("gcrypt: library version mismatch");
    }
    gcry_error_t err = 0;
    err = gcry_control(GCRYCTL_SUSPEND_SECMEM_WARN);
    err |= gcry_control(GCRYCTL_INIT_SECMEM, 16384, 0);
    err |= gcry_control(GCRYCTL_RESUME_SECMEM_WARN);
    err |= gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

    if (err) {
        dawnlog_error("gcrypt: failed initialization");
    }
}

void gcrypt_set_key_and_iv(const char *key, const char *iv) {
    size_t keylen = gcry_cipher_get_algo_keylen(GCRY_CIPHER);
    size_t blklen = gcry_cipher_get_algo_blklen(GCRY_CIPHER);

    gcry_error_handle = gcry_cipher_open(
            &gcry_cipher_hd, // gcry_cipher_hd_t *
            GCRY_CIPHER,   // int
            GCRY_C_MODE,   // int
            0);
    if (gcry_error_handle) {
        dawnlog_error("gcry_cipher_open failed:  %s/%s\n",
                gcry_strsource(gcry_error_handle),
                gcry_strerror(gcry_error_handle));
        return;
    }

    gcry_error_handle = gcry_cipher_setkey(gcry_cipher_hd, key, keylen);
    if (gcry_error_handle) {
        dawnlog_error("gcry_cipher_setkey failed:  %s/%s\n",
                gcry_strsource(gcry_error_handle),
                gcry_strerror(gcry_error_handle));
        return;
    }

    gcry_error_handle = gcry_cipher_setiv(gcry_cipher_hd, iv, blklen);
    if (gcry_error_handle) {
        dawnlog_error("gcry_cipher_setiv failed:  %s/%s\n",
                gcry_strsource(gcry_error_handle),
                gcry_strerror(gcry_error_handle));
        return;
    }
}

// free out buffer after using!
// The plaintext is zero-padded up to the cipher block size. The padding is
// done in a private buffer so we never read past the end of the caller's msg.
char *gcrypt_encrypt_msg(char *msg, size_t msg_length, int *out_length) {
    size_t padded_length = msg_length;
    if (0U != (padded_length & 0xfU))
        padded_length += 0x10U - (padded_length & 0xfU);

    if (padded_length == 0 || padded_length > INT_MAX) {
        dawnlog_error("gcry_cipher_encrypt error: invalid length %zu\n", msg_length);
        return NULL;
    }

    char *out = dawn_calloc(1, padded_length);
    if (!out){
        dawnlog_error("gcry_cipher_encrypt error: not enough memory\n");
        return NULL;
    }
    memcpy(out, msg, msg_length);

    // In-place encryption: libgcrypt allows in == NULL / inlen == 0
    gcry_error_handle = gcry_cipher_encrypt(gcry_cipher_hd, out, padded_length, NULL, 0);
    if (gcry_error_handle) {
        dawnlog_error("gcry_cipher_encrypt failed:  %s/%s\n",
                gcry_strsource(gcry_error_handle),
                gcry_strerror(gcry_error_handle));
        dawn_free(out);
        out = NULL;
        return NULL;
    }
    *out_length = (int)padded_length;
    return out;
}

// free out buffer after using!
// msg comes straight off the network, so its length is untrusted. Ciphertext
// must be a non-zero whole number of blocks; anything else is rejected rather
// than "padded", which used to read past the end of the input buffer.
// The result is always NUL-terminated.
char *gcrypt_decrypt_msg(char *msg, size_t msg_length) {
    if (msg_length == 0 || 0U != (msg_length & 0xfU)) {
        dawnlog_error("gcry_cipher_decrypt error: bad ciphertext length %zu\n", msg_length);
        return NULL;
    }

    char *out = dawn_malloc(msg_length + 1);
    if (!out){
        dawnlog_error("gcry_cipher_decrypt error: not enough memory\n");
        return NULL;
    }
    gcry_error_handle = gcry_cipher_decrypt(gcry_cipher_hd, out, msg_length, msg, msg_length);
    if (gcry_error_handle) {
        dawnlog_error("gcry_cipher_decrypt failed:  %s/%s\n",
                gcry_strsource(gcry_error_handle),
                gcry_strerror(gcry_error_handle));
        dawn_free(out);
        out = NULL;
        return NULL;
    }
    out[msg_length] = '\0';
    return out;
}
