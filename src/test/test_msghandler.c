/*
 * test_msghandler: feeds peer-to-peer network messages (one JSON message per
 * line) into handle_network_msg(), with the ubus / uci / iwinfo side effects
 * stubbed out. It exists mainly to check that malformed or hostile messages
 * from another "DAWN" instance are rejected rather than crashing the daemon,
 * so it is most useful built with -fsanitize=address,undefined.
 *
 *   test_msghandler [file]    (reads stdin if no file given; '#' = comment)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datastorage.h"
#include "dawn_iwinfo.h"
#include "dawn_uci.h"
#include "msghandler.h"
#include "ubus.h"

/* --- Stubs for everything that would touch the real system --- */
int get_rssi_iwinfo(struct dawn_mac a) { return 0; }
int get_expected_throughput_iwinfo(struct dawn_mac a) { return 0; }
int get_bandwidth_iwinfo(struct dawn_mac a, float *rx, float *tx) { return 0; }
struct probe_metric_s uci_get_dawn_metric() { return dawn_metric; }
struct time_config_s uci_get_time_config() { return timeout_config; }
int uci_set_network(char* cmd) { printf("  uci set %s\n", cmd); return 0; }
int uci_reset() { return 0; }
void ubus_set_nr_from_clients(struct kicking_nr* ap_list) { }
int ubus_send_beacon_request(client *c, ap *a, int d, int id) { return 0; }
int ubus_send_probe_via_network(struct probe_entry_s *p, bool is_beacon) { return 0; }
void add_client_update_timer(time_t time) { }
void del_client_interface(uint32_t id, const struct dawn_mac c, uint32_t r, uint8_t d, uint32_t b) { }
int send_set_probe(struct dawn_mac c) { return 0; }
int wnm_disassoc_imminent(uint32_t id, const struct dawn_mac c, struct kicking_nr* n, int t, uint32_t d) { return 0; }
int bss_transition_request(uint32_t id, const struct dawn_mac c, struct kicking_nr* n, uint32_t d) { return 0; }
int parse_add_mac_to_file(struct blob_attr *msg) { return 0; } // lives in ubus.c

int main(int argc, char **argv) {
    FILE *f = stdin;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int count = 0;

    if (argc > 1 && !(f = fopen(argv[1], "r"))) {
        perror(argv[1]);
        return 1;
    }

    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[--n] = '\0';
        if (n == 0 || line[0] == '#')
            continue;

        printf("MSG %d: %.100s%s\n", ++count, line, n > 100 ? "..." : "");
        handle_network_msg(line);
    }

    free(line);
    if (f != stdin)
        fclose(f);

    printf("test_msghandler: processed %d messages\n", count);
    return 0;
}
