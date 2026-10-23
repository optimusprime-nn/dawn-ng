#include <string.h>

#include "dawn_stats.h"

struct dawn_stats_s dawn_stats;

void stats_init(time_t now) {
    memset(&dawn_stats, 0, sizeof(dawn_stats));
    dawn_stats.started = now;
}

void stats_kick(time_t now, int method, struct dawn_mac client, struct dawn_mac from,
        const struct dawn_mac *to, int score_gain, int request_status) {
    struct kick_record *r = &dawn_stats.recent[dawn_stats.recent_next];

    if (method >= 0 && method < (int)(sizeof(dawn_stats.kicks_sent) / sizeof(dawn_stats.kicks_sent[0])))
        dawn_stats.kicks_sent[method]++;

    memset(r, 0, sizeof(*r));
    r->time = now;
    r->client = client;
    r->from = from;
    if (to)
        r->to = *to;
    r->method = method;
    r->score_gain = score_gain;

    if (request_status != 0) {
        r->outcome = KICK_OUTCOME_FAILED;
        r->outcome_time = now;
        dawn_stats.kicks_failed++;
    }
    else {
        r->outcome = KICK_OUTCOME_PENDING;
    }

    dawn_stats.recent_next = (dawn_stats.recent_next + 1) % STATS_RECENT_KICKS;
    if (dawn_stats.recent_count < STATS_RECENT_KICKS)
        dawn_stats.recent_count++;
}

void stats_client_seen(struct dawn_mac client, struct dawn_mac bssid, time_t now) {
    for (int i = 0; i < dawn_stats.recent_count; i++) {
        struct kick_record *r = &dawn_stats.recent[i];

        if (r->outcome != KICK_OUTCOME_PENDING || !mac_is_equal_bb(r->client, client))
            continue;

        if (!mac_is_equal_bb(r->from, bssid)) {
            r->outcome = KICK_OUTCOME_MOVED;
            r->outcome_time = now;
            dawn_stats.kicks_moved++;
        }
        else if (now - r->time >= STATS_STAY_TIMEOUT) {
            r->outcome = KICK_OUTCOME_STAYED;
            r->outcome_time = now;
            dawn_stats.kicks_stayed++;
        }
    }
}

const char *stats_method_name(int method) {
    switch (method) {
    case KICK_METHOD_RSSI_COMPARE: return "rssi_compare";
    case KICK_METHOD_ABSOLUTE_RSSI: return "absolute_rssi";
    case KICK_METHOD_FORCE_RECONNECT: return "force_reconnect";
    default: return "unknown";
    }
}

const char *stats_outcome_name(int outcome) {
    switch (outcome) {
    case KICK_OUTCOME_PENDING: return "pending";
    case KICK_OUTCOME_MOVED: return "moved";
    case KICK_OUTCOME_STAYED: return "stayed";
    case KICK_OUTCOME_FAILED: return "failed";
    default: return "unknown";
    }
}
