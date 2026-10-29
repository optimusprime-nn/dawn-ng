/*
 * Stand-in for libiwinfo in DAWN's daemon integration test.
 *
 * DAWN asks iwinfo for each hostapd interface's BSSID, SSID and frequency.
 * There is no Wi-Fi hardware in CI, so this library answers from the
 * FAKE_IWINFO environment variable instead:
 *
 *   FAKE_IWINFO="wlan0=02:00:00:00:0a:01,TestNet,2437;wlan1=02:00:00:00:0a:02,TestNet,5180"
 *
 * Station lists and surveys are empty (DAWN then has no bandwidth or
 * channel-utilisation data, which it handles).
 */
#include <iwinfo.h>

struct fake_if {
	char bssid[18];
	char ssid[IWINFO_ESSID_MAX_SIZE + 1];
	int freq;
};

static int lookup(const char *ifname, struct fake_if *out)
{
	const char *env = getenv("FAKE_IWINFO");
	size_t n = strlen(ifname);

	while (env && *env) {
		const char *end = strchr(env, ';');
		size_t len = end ? (size_t)(end - env) : strlen(env);

		if (len > n && strncmp(env, ifname, n) == 0 && env[n] == '=') {
			char buf[128];
			snprintf(buf, sizeof(buf), "%.*s", (int)(len - n - 1), env + n + 1);
			char *ssid = strchr(buf, ',');
			if (!ssid)
				return -1;
			*ssid++ = 0;
			char *freq = strchr(ssid, ',');
			if (freq)
				*freq++ = 0;
			snprintf(out->bssid, sizeof(out->bssid), "%.17s", buf);
			snprintf(out->ssid, sizeof(out->ssid), "%s", ssid);
			out->freq = freq ? atoi(freq) : 2412;
			return 0;
		}
		env = end ? end + 1 : NULL;
	}
	return -1;
}

static int f_bssid(const char *ifname, char *buf)
{
	struct fake_if f;
	if (lookup(ifname, &f))
		return -1;
	strcpy(buf, f.bssid);
	return 0;
}

static int f_ssid(const char *ifname, char *buf)
{
	struct fake_if f;
	if (lookup(ifname, &f))
		return -1;
	strcpy(buf, f.ssid);
	return 0;
}

static int f_frequency(const char *ifname, int *freq)
{
	struct fake_if f;
	if (lookup(ifname, &f))
		return -1;
	*freq = f.freq;
	return 0;
}

static int f_empty_list(const char *ifname, char *buf, int *len)
{
	(void)buf;
	*len = 0;
	return lookup(ifname, &(struct fake_if){0});
}

static int f_htmodelist(const char *ifname, int *modes)
{
	*modes = IWINFO_HTMODE_HT20 | IWINFO_HTMODE_HT40 | IWINFO_HTMODE_VHT80;
	return lookup(ifname, &(struct fake_if){0});
}

static int f_unsupported(const char *ifname, int *v)
{
	(void)ifname; (void)v;
	return -1;
}

static const struct iwinfo_ops fake_ops = {
	.name = "fake",
	.bssid = f_bssid,
	.ssid = f_ssid,
	.frequency = f_frequency,
	.channel = f_unsupported,
	.htmodelist = f_htmodelist,
	.hwmodelist = f_unsupported,
	.assoclist = f_empty_list,
	.survey = f_empty_list,
};

const struct iwinfo_ops *iwinfo_backend(const char *ifname)
{
	(void)ifname;
	return &fake_ops;
}

const struct iwinfo_ops *iwinfo_backend_by_name(const char *name)
{
	(void)name;
	return &fake_ops;
}

const char *iwinfo_type(const char *ifname)
{
	(void)ifname;
	return "fake";
}

void iwinfo_finish(void)
{
}
