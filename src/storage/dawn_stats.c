#include <string.h>

#include "dawn_stats.h"

struct dawn_stats_s dawn_stats;

static time_t real_clock(void) { return time(0); }
time_t (*dawn_stats_clock)(void) = real_clock;

static int policy_max_misses = 2;
static int policy_backoff = 3600;

void stats_init(time_t now) {
    memset(&dawn_stats, 0, sizeof(dawn_stats));
    dawn_stats.started = now;
}

void stats_set_backoff_policy(int max_misses, int backoff_seconds) {
    policy_max_misses = max_misses < 0 ? 0 : max_misses;
    policy_backoff = backoff_seconds < 0 ? 0 : backoff_seconds;
}

static struct client_steering *client_find(struct dawn_mac client) {
    for (int i = 0; i < dawn_stats.client_count; i++)
        if (mac_is_equal_bb(dawn_stats.clients[i].client, client))
            return &dawn_stats.clients[i];
    return NULL;
}

const struct client_steering *stats_client_lookup(struct dawn_mac client) {
    return client_find(client);
}

// Find or create; when full, reuse the least recently seen entry.
static struct client_steering *client_get(struct dawn_mac client, time_t now) {
    struct client_steering *c = client_find(client);

    if (!c) {
        if (dawn_stats.client_count < STATS_TRACKED_CLIENTS) {
            c = &dawn_stats.clients[dawn_stats.client_count++];
        }
        else {
            c = &dawn_stats.clients[0];
            for (int i = 1; i < STATS_TRACKED_CLIENTS; i++)
                if (dawn_stats.clients[i].last_seen < c->last_seen)
                    c = &dawn_stats.clients[i];
        }
        memset(c, 0, sizeof(*c));
        c->client = client;
    }

    c->last_seen = now;
    return c;
}

static void client_hit(struct client_steering *c) {
    c->misses = 0;
    c->backoff_until = 0;
}

static void client_miss(struct client_steering *c, time_t now) {
    if (c->misses < 255)
        c->misses++;

    if (policy_max_misses > 0 && c->misses >= policy_max_misses && c->backoff_until <= now) {
        c->backoff_until = now + policy_backoff;
        dawn_stats.backoffs_started++;
    }
}

int stats_client_in_backoff(struct dawn_mac client, time_t now) {
    struct client_steering *c;

    if (policy_max_misses == 0)
        return 0;

    c = client_find(client);
    if (!c || c->backoff_until == 0)
        return 0;

    if (now >= c->backoff_until) {
        // Backoff over: give the client a fresh start
        c->backoff_until = 0;
        c->misses = 0;
        return 0;
    }

    return 1;
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
        if (method != KICK_METHOD_FORCE_RECONNECT) {
            struct client_steering *c = client_get(client, now);
            if (c->requests < UINT16_MAX)
                c->requests++;
            if (method == KICK_METHOD_GENTLE)
                c->last_gentle = now;
        }
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

            struct client_steering *c = client_get(client, now);
            if (c->moved < UINT16_MAX)
                c->moved++;
            client_hit(c);
        }
        else if (now - r->time >= STATS_STAY_TIMEOUT) {
            r->outcome = KICK_OUTCOME_STAYED;
            r->outcome_time = now;
            dawn_stats.kicks_stayed++;

            struct client_steering *c = client_get(client, now);
            if (c->stayed < UINT16_MAX)
                c->stayed++;
            // A gentle request can be ignored without disrupting the client,
            // so only a stay after a forcing kick counts against it.
            if (r->method == KICK_METHOD_RSSI_COMPARE || r->btm_response == BTM_RESPONSE_REJECTED)
                client_miss(c, now);
        }
    }
}

void stats_btm_response(struct dawn_mac client, int status_code, time_t now) {
    // Attach it to this client's most recent request still waiting for one
    for (int i = 0; i < dawn_stats.recent_count; i++) {
        int idx = (dawn_stats.recent_next - 1 - i + STATS_RECENT_KICKS) % STATS_RECENT_KICKS;
        struct kick_record *r = &dawn_stats.recent[idx];

        if (!mac_is_equal_bb(r->client, client))
            continue;
        if (r->btm_response != BTM_RESPONSE_NONE)
            break; // newest request already answered: this one is unsolicited
        r->btm_status = status_code;
        r->btm_response = status_code == 0 ? BTM_RESPONSE_ACCEPTED : BTM_RESPONSE_REJECTED;
        break;
    }

    struct client_steering *c = client_get(client, now);
    if (status_code == 0) {
        dawn_stats.btm_accepted++;
        client_hit(c);
    }
    else {
        dawn_stats.btm_rejected++;
        if (c->rejected < UINT16_MAX)
            c->rejected++;
        client_miss(c, now);
    }
}

int stats_gentle_due(struct dawn_mac client, time_t now) {
    const struct client_steering *c = client_find(client);
    return !c || c->last_gentle == 0 || now - c->last_gentle >= STATS_GENTLE_INTERVAL;
}

const char *stats_method_name(int method) {
    switch (method) {
    case KICK_METHOD_RSSI_COMPARE: return "rssi_compare";
    case KICK_METHOD_ABSOLUTE_RSSI: return "absolute_rssi";
    case KICK_METHOD_FORCE_RECONNECT: return "force_reconnect";
    case KICK_METHOD_GENTLE: return "gentle";
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
