"""Mock router state for the LuCI tests: a BPI-R4-like AP with 2.4 + 5 GHz radios."""
import json, re, sys

def parse_uci(text, cfgname):
    out, cur, n = {}, None, 0
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        m = re.match(r"config\s+(\S+)(?:\s+'?([^']*)'?)?", line)
        if m:
            name = m.group(2)
            anon = not name
            if anon:
                name = 'cfg%02x%04x' % (n, 0x1000 + n); n += 1
            cur = out[name] = {'.type': m.group(1), '.name': name, '.anonymous': anon, '.index': len(out) - 1}
            continue
        m = re.match(r"(option|list)\s+(\S+)\s+'?([^']*)'?", line)
        if m and cur is not None:
            if m.group(1) == 'list':
                cur.setdefault(m.group(2), []).append(m.group(3))
            else:
                cur[m.group(2)] = m.group(3)
    return out

def build(dawn_config_path, variant):
    dawn = parse_uci(open(dawn_config_path).read(), 'dawn')

    wireless = {
      'radio0': {'.type': 'wifi-device', '.name': 'radio0', '.anonymous': False, 'band': '2g', 'channel': '6'},
      'radio1': {'.type': 'wifi-device', '.name': 'radio1', '.anonymous': False, 'band': '5g', 'channel': '36'},
      'default_radio0': {'.type': 'wifi-iface', '.name': 'default_radio0', '.anonymous': False, 'device': 'radio0', 'mode': 'ap', 'ssid': 'Home', 'network': 'lan'},
      'default_radio1': {'.type': 'wifi-iface', '.name': 'default_radio1', '.anonymous': False, 'device': 'radio1', 'mode': 'ap', 'ssid': 'Home', 'network': 'lan'},
    }
    if variant == 'healthy':
        for s in ('default_radio0', 'default_radio1'):
            wireless[s].update({'ieee80211k': '1', 'bss_transition': '1'})
        for sec in dawn.values():
            if sec['.type'] == 'network':
                sec['auth_key'] = '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff'

    # Methods of a full wpad on OpenWrt 25.12 (no wnm_disassoc_imminent any more)
    STEER = {'bss_transition_request': {}, 'rrm_beacon_req': {}, 'rrm_nr_get_own': {}, 'rrm_nr_set': {}}
    radios = {
      'radio0': {'up': True, 'config': {'band': '2g'}, 'interfaces': [
         {'section': 'default_radio0', 'ifname': 'phy0-ap0', 'config': {'mode': 'ap', 'ssid': 'Home'},
          'iwinfo': {'bssid': 'AA:AA:AA:AA:AA:01', 'frequency': 2437, 'mode': 'Master', 'ssid': 'Home'}}]},
      'radio1': {'up': True, 'config': {'band': '5g'}, 'interfaces': [
         {'section': 'default_radio1', 'ifname': 'phy1-ap0', 'config': {'mode': 'ap', 'ssid': 'Home'},
          'iwinfo': {'bssid': 'AA:AA:AA:AA:AA:02', 'frequency': 5180, 'mode': 'Master', 'ssid': 'Home'}}]},
    }
    devs = {n: {'name': n, 'up': True, 'flags': {'up': True}, 'type': 1, 'stats': {}, 'wireless': True}
            for n in ('phy0-ap0', 'phy1-ap0')}

    overview = {'Home': {
      'AA:AA:AA:AA:AA:01': {'freq': 2437, 'hostname': 'bpi-r4', 'iface': 'phy0-ap0', 'num_sta': 1, 'channel_utilization': 40, 'ht_support': True, 'vht_support': False},
      'AA:AA:AA:AA:AA:02': {'freq': 5180, 'hostname': 'bpi-r4', 'iface': 'phy1-ap0', 'num_sta': 2, 'channel_utilization': 60, 'ht_support': True, 'vht_support': True},
    }}
    if variant == 'healthy':
        overview['Home']['BB:BB:BB:BB:BB:01'] = {'freq': 5500, 'hostname': 'upstairs-ap', 'iface': 'phy1-ap0', 'num_sta': 3}

    stats = {'uptime': 7342, 'peer_auth': variant == 'healthy',
      'steering': {'evaluations': 418, 'stays': 371, 'deferred': 31, 'skipped_active': 7, 'sent_rssi_compare': 6,
                   'sent_absolute_rssi': 3, 'force_reconnects': 0, 'failed': 1, 'moved': 6, 'stayed': 1},
      'peers': {'messages_received': 2210, 'messages_rejected_auth': 0 if variant == 'healthy' else 0,
                'config_push_applied': 1, 'config_push_ignored': 0},
      'recent_kicks': [
        {'age': 95, 'client': '3C:22:FB:10:20:30', 'from': 'AA:AA:AA:AA:AA:01', 'to': 'AA:AA:AA:AA:AA:02', 'method': 'rssi_compare', 'score_gain': 34, 'outcome': 'moved', 'outcome_after': 9},
        {'age': 1260, 'client': 'F0:99:B6:44:55:66', 'from': 'AA:AA:AA:AA:AA:02', 'to': 'BB:BB:BB:BB:BB:01', 'method': 'absolute_rssi', 'outcome': 'moved', 'outcome_after': 4},
        {'age': 2405, 'client': '00:1A:11:22:33:44', 'from': 'AA:AA:AA:AA:AA:01', 'to': 'AA:AA:AA:AA:AA:02', 'method': 'rssi_compare', 'score_gain': 22, 'outcome': 'stayed', 'outcome_after': 60},
        {'age': 3900, 'client': 'A4:83:E7:01:02:03', 'from': 'AA:AA:AA:AA:AA:01', 'to': 'AA:AA:AA:AA:AA:02', 'method': 'rssi_compare', 'score_gain': 28, 'outcome': 'failed', 'outcome_after': 0},
        {'age': 4100, 'client': '00:1A:11:22:33:44', 'from': 'AA:AA:AA:AA:AA:01', 'to': 'AA:AA:AA:AA:AA:02', 'method': 'gentle', 'outcome': 'stayed', 'outcome_after': 60, 'response': 'rejected', 'response_status': 6},
      ],
      'clients': [
        {'client': '3C:22:FB:10:20:30', 'requests': 1, 'moved': 1, 'stayed': 0, 'rejected': 0, 'misses': 0, 'backoff_remaining': 0},
        {'client': '00:1A:11:22:33:44', 'requests': 3, 'moved': 1, 'bounced': 1, 'stayed': 2, 'rejected': 1, 'misses': 3, 'backoff_remaining': 2700},
      ]}
    hosts = {'3C:22:FB:10:20:30': {'name': 'pixel-8'}, 'F0:99:B6:44:55:66': {'name': 'macbook'}}

    return {
      'uci': {'dawn': dawn, 'wireless': wireless, 'network': {'lan': {'.type': 'interface', '.name': 'lan', '.anonymous': False, 'proto': 'static'}},
              'luci': {}, 'system': {'cfg01': {'.type': 'system', '.name': 'cfg01', '.anonymous': True, 'hostname': 'bpi-r4'}}},
      'features': {'wifi': True, 'hostapd': {'eap': True, '11k': True, '11v': True}},
      'objects': dict({'dawn': {'get_network': {}, 'get_hearing_map': {}, 'get_stats': {}, 'reload_config': {}},
                       'umdns': {'browse': {}}},
                      **{'hostapd.' + i: STEER for i in ('phy0-ap0', 'phy1-ap0')}),
      'calls': {
        'luci-rpc getWirelessDevices': radios, 'luci-rpc getNetworkDevices': devs, 'luci-rpc getBoardJSON': {},
        'luci-rpc getHostHints': hosts, 'network.interface dump': {'interface': []}, 'network get_proto_handlers': {},
        'dawn get_network': overview, 'dawn get_stats': stats, 'dawn get_hearing_map': {}},
    }
