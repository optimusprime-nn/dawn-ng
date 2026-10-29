#include <libubus.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include "memory_utils.h"
#include "datastorage.h"
#include "networksocket.h"
#include "ubus.h"
#include "dawn_uci.h"
#include "dawn_iwinfo.h"
#include "tcpsocket.h"
#include "crypto.h"
#include "dawn_stats.h"

void daemon_shutdown();

void signal_handler(int sig);

struct sigaction signal_action;

void daemon_shutdown() {
    // kill threads
    close_socket();
    uci_clear();
    uloop_cancelled = true;

    destroy_mutex();
}

void signal_handler(int sig) {
    switch (sig) {
        case SIGHUP:
            //daemon_shutdown();
            dawn_memory_audit();
            break;
        case SIGINT:
            daemon_shutdown();
            break;
        case SIGTERM:
            daemon_shutdown();
            exit(EXIT_SUCCESS);
        default:
            daemon_shutdown();
            break;
    }
}

int main(int argc, char **argv) {
    const char *ubus_socket = NULL;
    const char *loglevel_arg = NULL;
    const char *logdest_arg = NULL;

    // Options are read before the config so that -C can point at another
    // config directory; -l / -o are applied afterwards so they still
    // override the loglevel from the config.
    //   -l info|trace|debug   log level      -o stdio|syslog   log destination
    //   -s <path>             ubus socket    -C <dir>          UCI config directory
    int opt;
    while ((opt = getopt(argc, argv, "l:o:s:C:")) != -1) {
        switch (opt) {
        case 'l':
            loglevel_arg = optarg;
            break;
        case 'o':
            logdest_arg = optarg;
            break;
        case 's':
            ubus_socket = optarg;
            break;
        case 'C':
            dawn_uci_confdir = optarg;
            break;
        default: /* '?' */
            fprintf(stderr, "usage: %s [-l info|trace|debug] [-o stdio|syslog] [-s ubus_socket] [-C config_dir]\n", argv[0]);
            return 1;
        }
    }

    /* Load local config now so command line can override it */
    if (uci_init() < 0) {
        fprintf(stderr, "dawn: cannot load the 'dawn' UCI config%s%s\n",
                dawn_uci_confdir ? " from " : "", dawn_uci_confdir ? dawn_uci_confdir : "");
        return 1;
    }
    local_config = uci_get_local_config();

    int logdest = DAWNLOG_DEST_SYSLOG;

    if (loglevel_arg) {
        if (!strcmp(loglevel_arg, "info"))
            dawnlog_minlevel(DAWNLOG_INFO);
        else if (!strcmp(loglevel_arg, "trace"))
            dawnlog_minlevel(DAWNLOG_TRACE);
        else if (!strcmp(loglevel_arg, "debug"))
            dawnlog_minlevel(DAWNLOG_DEBUG);
        else
            dawnlog_warning("Unrecognised option for -l: %s\n", loglevel_arg);
    }

    if (logdest_arg) {
        if (!strcmp(logdest_arg, "stdio"))
            logdest = DAWNLOG_DEST_STDIO;
        else if (!strcmp(logdest_arg, "syslog"))
            logdest = DAWNLOG_DEST_SYSLOG;
        else
            dawnlog_warning("Unrecognised option for -o: %s\n", logdest_arg);
    }

    if (logdest == DAWNLOG_DEST_SYSLOG)
        openlog("dawn", LOG_CONS, LOG_DAEMON);

    dawnlog_dest(logdest);

    dawnlog_info("DAWN instance built around %s on %s starting...", __TIME__, __DATE__);

    // connect signals
    signal_action.sa_handler = signal_handler;
    sigemptyset(&signal_action.sa_mask);
    signal_action.sa_flags = 0;
    sigaction(SIGHUP, &signal_action, NULL);
    sigaction(SIGTERM, &signal_action, NULL);
    sigaction(SIGINT, &signal_action, NULL);

    // TODO: Why the extra loacl struct to retuen into?
    struct network_config_s net_config = uci_get_dawn_network();
    network_config = net_config;

    // init crypto
    gcrypt_init();
    gcrypt_set_key_and_iv(net_config.shared_key, net_config.iv);
    apply_peer_auth_config(&net_config);
    stats_init(time(0));

    // TODO: Why the extra loacl struct to retuen into?
    struct time_config_s time_config = uci_get_time_config();
    timeout_config = time_config; // TODO: Refactor...

    uci_get_dawn_hostapd_dir();
    uci_get_dawn_steering_ssids();

    init_mutex();

    switch (net_config.network_option) {
        case 0:
            init_socket_runopts(net_config.broadcast_ip, net_config.broadcast_port, 0);
            break;
        case 1:
            init_socket_runopts(net_config.broadcast_ip, net_config.broadcast_port, 1);
            break;
        default:
            break;
    }

    insert_macs_from_file();
    dawn_init_ubus(ubus_socket, hostapd_dir_glob);

    return 0;
}
