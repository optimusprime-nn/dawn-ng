/*
 * A stand-in for hostapd (and umdns) on ubus, for DAWN's daemon integration
 * test. It exports what DAWN talks to on a real access point:
 *
 *   hostapd.<ifname>  get_clients, rrm_nr_get_own, bss_transition_request, ...
 *                     and sends probe / bss-transition-response notifications
 *   umdns             browse: the other test APs' DAWN ports
 *
 * plus a control object for the test driver:
 *
 *   fake.<ifname>     connect / disconnect {address, signal?}
 *                     notify {type, data}     send a hostapd notification
 *                     calls                   what DAWN asked hostapd to do
 *                     clear                   forget those calls
 *                     set {btm: accept|reject|ignore}
 *                                             how the "client" answers BSS
 *                                             transition requests
 *
 * usage: fake_hostapd -s <ubus socket> -i <ifname> -b <bssid> -f <freq>
 *                     [-n <ssid>] [-p <peer tcp port>]...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>

#define MAX_CLIENTS 32
#define MAX_CALLS 256
#define MAX_PEERS 8

static struct ubus_context *ctx;
static struct blob_buf b;

static const char *ifname = "wlan0";
static const char *bssid = "02:00:00:00:00:01";
static const char *ssid = "TestNet";
static int freq = 2437;
static int peers[MAX_PEERS];
static int n_peers;

enum btm_mode { BTM_ACCEPT, BTM_REJECT, BTM_IGNORE };
static enum btm_mode btm_mode = BTM_IGNORE;

static struct {
	char addr[18];
	int signal;
} clients[MAX_CLIENTS];
static int n_clients;

static struct {
	char method[32];
	char *args;
} calls[MAX_CALLS];
static int n_calls;

static void log_call(const char *method, struct blob_attr *msg)
{
	if (n_calls >= MAX_CALLS)
		return;
	snprintf(calls[n_calls].method, sizeof(calls[n_calls].method), "%s", method);
	calls[n_calls].args = msg ? blobmsg_format_json(msg, true) : strdup("{}");
	n_calls++;
}

static int find_client(const char *addr)
{
	for (int i = 0; i < n_clients; i++)
		if (strcasecmp(clients[i].addr, addr) == 0)
			return i;
	return -1;
}

static void remove_client(const char *addr)
{
	int i = find_client(addr);
	if (i < 0)
		return;
	clients[i] = clients[--n_clients];
}

/* The hostapd object; filled in main() since its name depends on ifname. */
static struct ubus_object hostapd_obj;

/* ---------------------------------------------------------------- hostapd */

static int h_get_clients(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
			 const char *method, struct blob_attr *msg)
{
	log_call(method, msg);
	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "freq", freq);
	void *t = blobmsg_open_table(&b, "clients");
	for (int i = 0; i < n_clients; i++) {
		void *cl = blobmsg_open_table(&b, clients[i].addr);
		blobmsg_add_u8(&b, "auth", 1);
		blobmsg_add_u8(&b, "assoc", 1);
		blobmsg_add_u8(&b, "authorized", 1);
		blobmsg_add_u8(&b, "preauth", 0);
		blobmsg_add_u8(&b, "wds", 0);
		blobmsg_add_u8(&b, "wmm", 1);
		blobmsg_add_u8(&b, "ht", 1);
		blobmsg_add_u8(&b, "vht", 1);
		blobmsg_add_u8(&b, "wps", 0);
		blobmsg_add_u8(&b, "mfp", 0);
		blobmsg_add_u32(&b, "aid", i + 1);
		void *rrm = blobmsg_open_array(&b, "rrm");
		/* 802.11k: link measurement + beacon passive/active/table */
		blobmsg_add_u32(&b, NULL, 0x73);
		for (int k = 0; k < 4; k++)
			blobmsg_add_u32(&b, NULL, 0);
		blobmsg_close_array(&b, rrm);
		blobmsg_add_u32(&b, "signal", clients[i].signal);
		blobmsg_close_table(&b, cl);
	}
	blobmsg_close_table(&b, t);
	return ubus_send_reply(c, req, b.head);
}

static int h_rrm_nr_get_own(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
			    const char *method, struct blob_attr *msg)
{
	char nr[64], hex[13] = "";
	unsigned int m[6];

	log_call(method, msg);
	sscanf(bssid, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
	for (int i = 0; i < 6; i++)
		sprintf(hex + 2 * i, "%02x", m[i]);
	/* BSSID, BSSID info, operating class, channel, PHY type */
	snprintf(nr, sizeof(nr), "%s%s%02x%02x%02x", hex, "8f000000",
		 freq > 5000 ? 128 : 81, freq > 5000 ? (freq - 5000) / 5 : (freq - 2407) / 5, freq > 5000 ? 9 : 7);

	blob_buf_init(&b, 0);
	void *a = blobmsg_open_array(&b, "value");
	blobmsg_add_string(&b, NULL, bssid);
	blobmsg_add_string(&b, NULL, ssid);
	blobmsg_add_string(&b, NULL, nr);
	blobmsg_close_array(&b, a);
	return ubus_send_reply(c, req, b.head);
}

enum { BTM_ADDR, __BTM_MAX };
static const struct blobmsg_policy btm_policy[__BTM_MAX] = {
	[BTM_ADDR] = { .name = "addr", .type = BLOBMSG_TYPE_STRING },
};

static int h_bss_transition_request(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
				    const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__BTM_MAX];

	log_call(method, msg);
	blobmsg_parse(btm_policy, __BTM_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[BTM_ADDR])
		return UBUS_STATUS_INVALID_ARGUMENT;
	const char *addr = blobmsg_get_string(tb[BTM_ADDR]);
	if (find_client(addr) < 0)
		return UBUS_STATUS_NOT_FOUND;

	if (btm_mode == BTM_IGNORE)
		return 0;

	/* Like hostapd: the station's BSS Transition Management response
	 * arrives as a notification. */
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "address", addr);
	blobmsg_add_u8(&b, "dialog-token", 1);
	blobmsg_add_u8(&b, "status-code", btm_mode == BTM_ACCEPT ? 0 : 6);
	ubus_notify(c, &hostapd_obj, "bss-transition-response", b.head, -1);
	if (btm_mode == BTM_ACCEPT)
		remove_client(addr); /* the driver connects it to the other AP */
	return 0;
}

static int h_ok(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		const char *method, struct blob_attr *msg)
{
	log_call(method, msg);
	return 0;
}

static const struct ubus_method hostapd_methods[] = {
	UBUS_METHOD_NOARG("get_clients", h_get_clients),
	UBUS_METHOD_NOARG("rrm_nr_get_own", h_rrm_nr_get_own),
	UBUS_METHOD_NOARG("bss_transition_request", h_bss_transition_request),
	UBUS_METHOD_NOARG("rrm_nr_set", h_ok),
	UBUS_METHOD_NOARG("rrm_beacon_req", h_ok),
	UBUS_METHOD_NOARG("bss_mgmt_enable", h_ok),
	UBUS_METHOD_NOARG("del_client", h_ok),
	UBUS_METHOD_NOARG("notify_response", h_ok),
};
static struct ubus_object_type hostapd_type = UBUS_OBJECT_TYPE("hostapd", hostapd_methods);

/* ---------------------------------------------------------------- control */

enum { C_ADDRESS, C_SIGNAL, __C_MAX };
static const struct blobmsg_policy conn_policy[__C_MAX] = {
	[C_ADDRESS] = { .name = "address", .type = BLOBMSG_TYPE_STRING },
	[C_SIGNAL] = { .name = "signal", .type = BLOBMSG_TYPE_INT32 },
};

static int c_connect(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		     const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__C_MAX];
	blobmsg_parse(conn_policy, __C_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[C_ADDRESS])
		return UBUS_STATUS_INVALID_ARGUMENT;
	const char *addr = blobmsg_get_string(tb[C_ADDRESS]);

	if (strcmp(method, "disconnect") == 0) {
		remove_client(addr);
		return 0;
	}
	int i = find_client(addr);
	if (i < 0) {
		if (n_clients >= MAX_CLIENTS)
			return UBUS_STATUS_UNKNOWN_ERROR;
		i = n_clients++;
		snprintf(clients[i].addr, sizeof(clients[i].addr), "%s", addr);
	}
	clients[i].signal = tb[C_SIGNAL] ? (int)blobmsg_get_u32(tb[C_SIGNAL]) : -60;
	return 0;
}

enum { N_TYPE, N_DATA, __N_MAX };
static const struct blobmsg_policy notify_policy[__N_MAX] = {
	[N_TYPE] = { .name = "type", .type = BLOBMSG_TYPE_STRING },
	[N_DATA] = { .name = "data", .type = BLOBMSG_TYPE_TABLE },
};

static int c_notify(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		    const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__N_MAX], *cur;
	int rem;

	blobmsg_parse(notify_policy, __N_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[N_TYPE] || !tb[N_DATA])
		return UBUS_STATUS_INVALID_ARGUMENT;
	blob_buf_init(&b, 0);
	blobmsg_for_each_attr(cur, tb[N_DATA], rem)
		blobmsg_add_blob(&b, cur);
	/* hostapd waits for DAWN's verdict on probe/auth/assoc; we don't. */
	return ubus_notify(c, &hostapd_obj, blobmsg_get_string(tb[N_TYPE]), b.head, -1);
}

static int c_calls(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		   const char *method, struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	void *a = blobmsg_open_array(&b, "calls");
	for (int i = 0; i < n_calls; i++) {
		void *t = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "method", calls[i].method);
		blobmsg_add_string(&b, "args", calls[i].args);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, a);
	a = blobmsg_open_array(&b, "clients");
	for (int i = 0; i < n_clients; i++)
		blobmsg_add_string(&b, NULL, clients[i].addr);
	blobmsg_close_array(&b, a);
	return ubus_send_reply(c, req, b.head);
}

static int c_clear(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		   const char *method, struct blob_attr *msg)
{
	for (int i = 0; i < n_calls; i++)
		free(calls[i].args);
	n_calls = 0;
	return 0;
}

enum { S_BTM, __S_MAX };
static const struct blobmsg_policy set_policy[__S_MAX] = {
	[S_BTM] = { .name = "btm", .type = BLOBMSG_TYPE_STRING },
};

static int c_set(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		 const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__S_MAX];
	blobmsg_parse(set_policy, __S_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[S_BTM]) {
		const char *m = blobmsg_get_string(tb[S_BTM]);
		if (!strcmp(m, "accept"))
			btm_mode = BTM_ACCEPT;
		else if (!strcmp(m, "reject"))
			btm_mode = BTM_REJECT;
		else if (!strcmp(m, "ignore"))
			btm_mode = BTM_IGNORE;
		else
			return UBUS_STATUS_INVALID_ARGUMENT;
	}
	return 0;
}

static const struct ubus_method ctl_methods[] = {
	UBUS_METHOD("connect", c_connect, conn_policy),
	UBUS_METHOD("disconnect", c_connect, conn_policy),
	UBUS_METHOD("notify", c_notify, notify_policy),
	UBUS_METHOD_NOARG("calls", c_calls),
	UBUS_METHOD_NOARG("clear", c_clear),
	UBUS_METHOD("set", c_set, set_policy),
};
static struct ubus_object_type ctl_type = UBUS_OBJECT_TYPE("fake_hostapd", ctl_methods);
static struct ubus_object ctl_obj = {
	.type = &ctl_type, .methods = ctl_methods, .n_methods = ARRAY_SIZE(ctl_methods),
};

/* ---------------------------------------------------------------- umdns */

static int u_browse(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		    const char *method, struct blob_attr *msg)
{
	char name[32];

	blob_buf_init(&b, 0);
	void *svc = blobmsg_open_table(&b, "_dawn._tcp");
	for (int i = 0; i < n_peers; i++) {
		snprintf(name, sizeof(name), "test-ap-%d", peers[i]);
		void *h = blobmsg_open_table(&b, name);
		blobmsg_add_string(&b, "ipv4", "127.0.0.1");
		blobmsg_add_u32(&b, "port", peers[i]);
		blobmsg_close_table(&b, h);
	}
	blobmsg_close_table(&b, svc);
	return ubus_send_reply(c, req, b.head);
}

static int u_update(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		    const char *method, struct blob_attr *msg)
{
	return 0;
}

static const struct ubus_method umdns_methods[] = {
	UBUS_METHOD_NOARG("browse", u_browse),
	UBUS_METHOD_NOARG("update", u_update),
};
static struct ubus_object_type umdns_type = UBUS_OBJECT_TYPE("umdns", umdns_methods);
static struct ubus_object umdns_obj = {
	.name = "umdns", .type = &umdns_type, .methods = umdns_methods, .n_methods = ARRAY_SIZE(umdns_methods),
};

int main(int argc, char **argv)
{
	const char *sock = NULL;
	char hostapd_name[64], ctl_name[64];
	int opt;

	while ((opt = getopt(argc, argv, "s:i:b:f:n:p:")) != -1) {
		switch (opt) {
		case 's': sock = optarg; break;
		case 'i': ifname = optarg; break;
		case 'b': bssid = optarg; break;
		case 'f': freq = atoi(optarg); break;
		case 'n': ssid = optarg; break;
		case 'p':
			if (n_peers < MAX_PEERS)
				peers[n_peers++] = atoi(optarg);
			break;
		default:
			fprintf(stderr, "usage: %s -s sock -i ifname -b bssid -f freq [-n ssid] [-p peer_port]...\n", argv[0]);
			return 1;
		}
	}

	uloop_init();
	ctx = ubus_connect(sock);
	if (!ctx) {
		fprintf(stderr, "fake_hostapd: cannot connect to ubus at %s\n", sock ? sock : "(default)");
		return 1;
	}
	ubus_add_uloop(ctx);

	snprintf(hostapd_name, sizeof(hostapd_name), "hostapd.%s", ifname);
	hostapd_obj.name = hostapd_name;
	hostapd_obj.type = &hostapd_type;
	hostapd_obj.methods = hostapd_methods;
	hostapd_obj.n_methods = ARRAY_SIZE(hostapd_methods);
	snprintf(ctl_name, sizeof(ctl_name), "fake.%s", ifname);
	ctl_obj.name = ctl_name;

	if (ubus_add_object(ctx, &hostapd_obj) || ubus_add_object(ctx, &ctl_obj) ||
	    (n_peers && ubus_add_object(ctx, &umdns_obj))) {
		fprintf(stderr, "fake_hostapd: cannot register ubus objects\n");
		return 1;
	}
	printf("fake_hostapd %s (%s, %d MHz) ready\n", hostapd_name, bssid, freq);
	fflush(stdout);
	uloop_run();
	ubus_free(ctx);
	uloop_done();
	return 0;
}
