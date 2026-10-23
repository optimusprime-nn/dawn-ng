/*
 * Stubs for the ubus / uci / iwinfo functions msghandler.c and
 * datastorage.c call, so that peer message handling can run on a build host
 * (test_msghandler, fuzz_msghandler).
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
int uci_set_network(char* cmd) { if (getenv("DAWN_STUB_VERBOSE")) printf("  uci set %s\n", cmd); return 0; }
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

