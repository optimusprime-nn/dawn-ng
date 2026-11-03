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
    c->backoff_level = 0;
}

// policy_backoff, doubled for each backoff since the last move that stuck
static time_t backoff_length(const struct client_steering *c) {
    time_t len = policy_backoff;
    for (int i = 1; i < c->backoff_level && len < STATS_BACKOFF_MAX; i++)
        len *= 2;
    return len < STATS_BACKOFF_MAX ? len : STATS_BACKOFF_MAX;
}

static time_t gentle_interval(const struct client_steering *c) {
    time_t len = STATS_GENTLE_INTERVAL;
    for (int i = 1; i < c->backoff_level && len < STATS_GENTLE_MAX; i++)
        len *= 2;
    return len < STATS_GENTLE_MAX ? len : STATS_GENTLE_MAX;
}

static void client_miss(struct client_steering *c, time_t now) {
    if (c->misses < 255)
        c->misses++;

    if (policy_max_misses > 0 && c->misses >= policy_max_misses && c->backoff_until <= now) {
        if (c->backoff_level < 16)
            c->backoff_level++;
        c->backoff_until = now + backoff_length(c);
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
        // Backoff over: try again (backoff_level stays, so the next one is longer)
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
            c->last_request = now;
            if (method == KICK_METHOD_GENTLE)
                c->last_gentle = now;
        }
    }

    dawn_stats.recent_next = (dawn_stats.recent_next + 1) % STATS_RECENT_KICKS;
    if (dawn_stats.recent_count < STATS_RECENT_KICKS)
        dawn_stats.recent_count++;
}

void stats_client_seen(struct dawn_mac client, struct dawn_mac bssid, time_t now) {
    // A move that didn't come back (stats_client_associated) has stuck
    struct client_steering *w = client_find(client);
    if (w && w->moved_at && now - w->moved_at > STATS_BOUNCE_WINDOW) {
        client_hit(w);
        w->moved_at = 0;
    }

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
            // Only a success once it sticks (see stats_client_seen above)
            c->moved_from = r->from;
            c->moved_at = now;
        }
        else if (now - r->time >= STATS_STAY_TIMEOUT) {
            r->outcome = KICK_OUTCOME_STAYED;
            r->outcome_time = now;
            dawn_stats.kicks_stayed++;

            struct client_steering *c = client_get(client, now);
            if (c->stayed < UINT16_MAX)
                c->stayed++;
            // Asked and stayed. A friendly request in backoff may be ignored
            // without counting (it's already backing off).
            // A rejection was already counted when it arrived.
            if ((r->method == KICK_METHOD_RSSI_COMPARE || r->method == KICK_METHOD_ABSOLUTE_RSSI) &&
                    r->btm_response != BTM_RESPONSE_REJECTED)
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
        // Not a success yet: some clients accept, move, and come straight back
        dawn_stats.btm_accepted++;
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
    return !c || c->last_gentle == 0 || now - c->last_gentle >= gentle_interval(c);
}

const char *stats_request_hold(struct dawn_mac client, time_t now) {
    const struct client_steering *c = client_find(client);

    if (!c)
        return NULL;
    if (stats_client_in_backoff(client, now))
        return stats_gentle_due(client, now) ? NULL : "backing off, asked recently";
    if (c->last_request && now - c->last_request < STATS_MIN_REQUEST_INTERVAL)
        return "asked less than a minute ago";
    if (c->moved_at && now - c->moved_at < STATS_GHOST_HOLD)
        return "just moved to another AP (this AP may still list it for a few minutes)";
    return NULL;
}

void stats_client_associated(struct dawn_mac client, struct dawn_mac bssid, time_t now) {
    struct client_steering *c = client_find(client);

    if (!c || !c->moved_at)
        return;
    if (mac_is_equal_bb(bssid, c->moved_from) && now - c->moved_at <= STATS_BOUNCE_WINDOW) {
        // Moved away, then associated to the old AP again: came straight back
        if (c->bounced < UINT16_MAX)
            c->bounced++;
        c->last_seen = now;
        c->moved_at = 0;
        client_miss(c, now);
    }
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
