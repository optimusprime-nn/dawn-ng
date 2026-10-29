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
    KICK_METHOD_FORCE_RECONNECT = 3,// deauthenticated: no information about client
    KICK_METHOD_GENTLE = 4,         // RSSI compare, but the client is in backoff:
                                    // friendly request only, no forced disconnect
    __KICK_METHOD_MAX
};

enum btm_response {
    BTM_RESPONSE_NONE = 0,          // no BSS Transition Management response (yet)
    BTM_RESPONSE_ACCEPTED,          // status code 0
    BTM_RESPONSE_REJECTED           // any other status code
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
    int btm_response;               // enum btm_response
    int btm_status;                 // 802.11 status code from the client
};

/*
 * Per-client steering history, so DAWN can stop disrupting clients that do
 * not follow its requests. A "miss" is a request the client rejected, or a
 * kick after which it was found back on the same AP. After
 * steer_max_misses consecutive misses the client is in backoff for
 * steer_backoff seconds: it only gets friendly requests (no forced
 * disassociation). A successful move or an accepted request resets it.
 */
#define STATS_TRACKED_CLIENTS 128

struct client_steering {
    struct dawn_mac client;
    time_t last_seen;               // for replacing the least recently used entry
    uint16_t requests;
    uint16_t moved;
    uint16_t stayed;
    uint16_t rejected;
    uint8_t misses;                 // consecutive
    time_t backoff_until;
    time_t last_gentle;             // last friendly request while in backoff
};

// While in backoff, at most one friendly request per client this often
#define STATS_GENTLE_INTERVAL 300

struct dawn_stats_s {
    time_t started;

    uint32_t evaluations;           // client evaluations by the kicking logic
    uint32_t stays;                 // ... where the current AP was best
    uint32_t kicks_deferred;        // wanted to kick, below min_number_to_kick
    uint32_t kicks_skipped_active;  // wanted to kick, client was transferring data
    uint32_t kicks_sent[__KICK_METHOD_MAX]; // indexed by enum kick_method
    uint32_t btm_accepted;
    uint32_t btm_rejected;
    uint32_t backoffs_started;
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

    struct client_steering clients[STATS_TRACKED_CLIENTS];
    int client_count;
};

extern struct dawn_stats_s dawn_stats;

void stats_init(time_t now);

void stats_kick(time_t now, int method, struct dawn_mac client, struct dawn_mac from,
        const struct dawn_mac *to, int score_gain, int request_status);

/** Called whenever a client is (re)seen associated to bssid, locally or via a peer. */
void stats_client_seen(struct dawn_mac client, struct dawn_mac bssid, time_t now);

/** A BSS Transition Management response from client (hostapd 'bss-transition-response'). */
void stats_btm_response(struct dawn_mac client, int status_code, time_t now);

/**
 * True while client is in steering backoff (too many consecutive misses):
 * it should only get friendly requests. max_misses == 0 disables backoff.
 */
int stats_client_in_backoff(struct dawn_mac client, time_t now);

/** Backoff settings (from dawn_metric); set by the config loader. */
void stats_set_backoff_policy(int max_misses, int backoff_seconds);

const struct client_steering *stats_client_lookup(struct dawn_mac client);

/** In backoff: may this client be sent another friendly request yet? */
int stats_gentle_due(struct dawn_mac client, time_t now);

/** Clock used for kick records; tests replace it with a fake one. */
extern time_t (*dawn_stats_clock)(void);

const char *stats_method_name(int method);
const char *stats_outcome_name(int outcome);

#endif
