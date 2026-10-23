#ifndef DAWN_STATS_H
#define DAWN_STATS_H

#include <stdint.h>
#include <time.h>

#include "mac_utils.h"

/*
 * Counters answering "is DAWN actually doing anything?", exposed by
 * `ubus call dawn get_stats` and the LuCI Steering page.
 *
 * Kick records and outcomes are updated from the kicking evaluation and from
 * client table updates, both of which hold client_array_mutex. The message
 * counters are only touched from the uloop thread (TCP) or are best-effort.
 */

#define STATS_RECENT_KICKS 32
// A kicked client still on the old AP after this long is counted as "stayed"
#define STATS_STAY_TIMEOUT 60

enum kick_method {
    KICK_METHOD_RSSI_COMPARE = 1,   // 802.11v disassociation imminent (kicking & 1)
    KICK_METHOD_ABSOLUTE_RSSI = 2,  // 802.11v BSS transition request (kicking & 2)
    KICK_METHOD_FORCE_RECONNECT = 3 // deauthenticated: no information about client
};

enum kick_outcome {
    KICK_OUTCOME_PENDING = 0,
    KICK_OUTCOME_MOVED,             // seen on a different BSSID afterwards
    KICK_OUTCOME_STAYED,            // still on the same BSSID after STATS_STAY_TIMEOUT
    KICK_OUTCOME_FAILED             // hostapd refused the request
};

struct kick_record {
    time_t time;
    struct dawn_mac client;
    struct dawn_mac from;
    struct dawn_mac to;             // best candidate offered, or null
    int method;
    int score_gain;                 // candidate score - own score (RSSI compare only)
    int outcome;
    time_t outcome_time;
};

struct dawn_stats_s {
    time_t started;

    uint32_t evaluations;           // client evaluations by the kicking logic
    uint32_t stays;                 // ... where the current AP was best
    uint32_t kicks_deferred;        // wanted to kick, below min_number_to_kick
    uint32_t kicks_skipped_active;  // wanted to kick, client was transferring data
    uint32_t kicks_sent[4];         // indexed by enum kick_method
    uint32_t kicks_failed;
    uint32_t kicks_moved;
    uint32_t kicks_stayed;

    uint32_t peer_msgs_received;
    uint32_t peer_msgs_rejected_auth;
    uint32_t config_push_applied;
    uint32_t config_push_ignored;

    struct kick_record recent[STATS_RECENT_KICKS];
    int recent_next;                // ring buffer write position
    int recent_count;
};

extern struct dawn_stats_s dawn_stats;

void stats_init(time_t now);

void stats_kick(time_t now, int method, struct dawn_mac client, struct dawn_mac from,
        const struct dawn_mac *to, int score_gain, int request_status);

/** Called whenever a client is (re)seen associated to bssid, locally or via a peer. */
void stats_client_seen(struct dawn_mac client, struct dawn_mac bssid, time_t now);

const char *stats_method_name(int method);
const char *stats_outcome_name(int outcome);

#endif
