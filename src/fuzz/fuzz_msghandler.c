/*
 * libFuzzer target: one input = one message received from a peer DAWN
 * instance, exactly as handle_network_msg() sees it after TCP framing.
 * Config pushes are enabled so their parser is covered too.
 *
 * Build with clang -fsanitize=fuzzer,address,undefined (see CMake option
 * DAWN_FUZZ / tools/ci/run-fuzz.sh).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "datastorage.h"
#include "memory_utils.h"
#include "msghandler.h"
#include "utils.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static int initialised = 0;
    static unsigned iterations = 0;

    if (!initialised) {
        init_mutex();
        network_config.accept_config_push = ACCEPT_CONFIG_PUSH_ANY;
        initialised = 1;
    }

    // The TCP reader always NUL-terminates the frame it hands over
    char *msg = malloc(size + 1);
    if (!msg)
        return 0;
    memcpy(msg, data, size);
    msg[size] = '\0';

    handle_network_msg(msg, (size & 1) != 0);
    free(msg);

    // Keep the tables from growing for the whole run: expire everything
    // every so often, which also exercises the removal paths.
    if (++iterations % 256 == 0) {
        // Same locking as the expiry timers in ubus.c
        dawn_mutex_lock(&probe_array_mutex);
        remove_old_probe_entries(INT32_MAX, 0);
        dawn_mutex_unlock(&probe_array_mutex);

        dawn_mutex_lock(&client_array_mutex);
        remove_old_client_entries(INT32_MAX, 0);
        dawn_mutex_unlock(&client_array_mutex);

        dawn_mutex_lock(&ap_array_mutex);
        remove_old_ap_entries(INT32_MAX, 0);
        dawn_mutex_unlock(&ap_array_mutex);
    }

    return 0;
}
