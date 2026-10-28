'use strict';
'require view';
'require rpc';
'require uci';
'require ui';
'require network';
'require dawn.dawn-common as dawn';

/*
 * DAWN Setup Check
 *
 * DAWN silently does very little unless several things outside its own
 * config are right: a full hostapd (wpad) build, 802.11k/v enabled on each
 * access point, umdns for peer discovery, and kicking switched on. This page
 * checks each of those, says what is wrong in plain words, and offers a
 * one-click fix where LuCI can safely make the change.
 *
 * evaluate() is a pure function of the gathered state, so the logic can be
 * unit tested without a router.
 */

const STATUS_ORDER = { error: 0, warning: 1, info: 2, ok: 3 };

/* hostapd ubus method DAWN uses to steer clients (both kicking methods call it;
 * see bss_transition_request() / wnm_disassoc_imminent() in DAWN's ubus.c).
 * Only full wpad builds (CONFIG_WNM) have it. Do not also require
 * 'wnm_disassoc_imminent': hostapd in OpenWrt 25.12 no longer offers that
 * method, so requiring it reported every 25.12 router as wpad-basic. */
const STEERING_METHODS = [ 'bss_transition_request' ];

function isOn(value) {
	return value == '1' || value == 'true' || value == 'on' || value == 'yes';
}

function bandLabel(band) {
	switch (band) {
	case '2g': return '2.4 GHz';
	case '5g': return '5 GHz';
	case '6g': return '6 GHz';
	case '60g': return '60 GHz';
	default: return band || _('unknown band');
	}
}

function ifaceLabel(iface) {
	return '"%s" (%s, %s)'.format(iface.ssid || '?', bandLabel(iface.band), iface.section);
}

/*
 * state = {
 *   dawnRunning: bool,
 *   overview:    result of dawn get_network, or null,
 *   fullHostapd: bool|null   (L.hasSystemFeature('hostapd', 'eap')),
 *   umdns:       bool        (umdns ubus object present),
 *   ifaces: [ { section, ssid, band, ifname, bssid, up, ieee80211k, bss_transition,
 *               methods: { name: true } | null } ],
 *   config: { network_option, server_ip, tcp_port, kicking, use_symm_enc,
 *             loglevel, steering_ssid: [] }
 * }
 *
 * Returns [ { id, status, title, detail, items?, fix? } ]
 */
function evaluate(state) {
	const checks = [];
	const cfg = state.config || {};
	const steering = (cfg.steering_ssid || []).filter(function(s) { return s != null && s !== ''; });

	/* Only the SSIDs DAWN is told to manage matter (all of them by default) */
	const ifaces = (state.ifaces || []).filter(function(i) {
		return steering.length == 0 || steering.indexOf(i.ssid) > -1;
	});

	/* 1. Is DAWN itself running? */
	checks.push(state.dawnRunning ? {
		id: 'service', status: 'ok',
		title: _('DAWN is running')
	} : {
		id: 'service', status: 'error',
		title: _('DAWN is not running'),
		detail: _('The DAWN service is not answering on ubus. Enable and start it under System → Startup, then reload this page.'),
		fix: { type: 'link', label: _('Open Startup'), href: 'admin/system/startup' }
	});

	/* 2. Full hostapd: wpad-basic lacks the 802.11k/v features DAWN steers with */
	const upIfaces = ifaces.filter(function(i) { return i.up && i.methods != null; });
	const missingMethods = upIfaces.filter(function(i) {
		return STEERING_METHODS.some(function(m) { return !i.methods[m]; });
	});

	if (state.fullHostapd === false || (upIfaces.length > 0 && missingMethods.length == upIfaces.length)) {
		checks.push({
			id: 'wpad', status: 'error',
			title: _('A basic version of wpad/hostapd is installed'),
			detail: _('DAWN needs a full wpad build (for example wpad-mbedtls, wpad-wolfssl or wpad-openssl) to ask clients to move. With wpad-basic, clients are never steered. Replace the wpad package, then reboot.'),
			fix: { type: 'link', label: _('Open Software'), href: 'admin/system/package-manager?query=wpad' }
		});
	}
	else if (missingMethods.length > 0) {
		checks.push({
			id: 'wpad', status: 'warning',
			title: _('Some access points cannot steer clients'),
			detail: _('hostapd on these interfaces does not offer 802.11v transition requests. Check that 802.11v (BSS Transition) is enabled for them and that the wireless configuration has been applied.'),
			items: missingMethods.map(ifaceLabel)
		});
	}
	else {
		checks.push({
			id: 'wpad', status: upIfaces.length ? 'ok' : 'info',
			title: upIfaces.length ? _('hostapd supports client steering') : _('Could not check hostapd features'),
			detail: upIfaces.length ? null : _('No access point interface is currently up, so hostapd could not be asked which features it supports.')
		});
	}

	/* 3. Are there any access points to manage? */
	if (ifaces.length == 0) {
		checks.push({
			id: 'aps', status: 'error',
			title: steering.length ? _('None of the configured steering SSIDs exist here') : _('No enabled access point networks'),
			detail: steering.length
				? _('DAWN is restricted to these SSIDs, but no enabled access point on this device uses them: %s').format(steering.join(', '))
				: _('DAWN steers clients between access points. Enable at least one wireless network in Access Point mode.'),
			fix: { type: 'link', label: _('Open Wireless'), href: 'admin/network/wireless' }
		});
	}
	else {
		const down = ifaces.filter(function(i) { return !i.up; });
		if (down.length) {
			checks.push({
				id: 'aps', status: 'warning',
				title: _('Some access point networks are not up'),
				detail: _('These networks are enabled in the configuration but are not currently running.'),
				items: down.map(ifaceLabel),
				fix: { type: 'link', label: _('Open Wireless'), href: 'admin/network/wireless' }
			});
		}
	}

	/* 4. 802.11k (neighbor / beacon reports) and 802.11v (BSS transition) per network */
	const needsRoaming = ifaces.filter(function(i) {
		return !isOn(i.ieee80211k) || !isOn(i.bss_transition);
	});

	if (ifaces.length && needsRoaming.length) {
		checks.push({
			id: 'roaming', status: 'warning',
			title: _('802.11k/v is off on %d network(s)').format(needsRoaming.length),
			detail: _('Without 802.11k (Radio Resource Measurement) and 802.11v (BSS Transition), DAWN cannot learn which access points a client can hear or ask it to move. Enabling them restarts Wi-Fi briefly; older clients simply ignore them.'),
			items: needsRoaming.map(function(i) {
				const missing = [];
				if (!isOn(i.ieee80211k)) missing.push('802.11k');
				if (!isOn(i.bss_transition)) missing.push('802.11v');
				return ifaceLabel(i) + ': ' + _('missing') + ' ' + missing.join(' + ');
			}),
			fix: {
				type: 'action', action: 'enableRoaming',
				label: _('Enable 802.11k/v'),
				sections: needsRoaming.map(function(i) { return i.section; })
			}
		});
	}
	else if (ifaces.length) {
		checks.push({ id: 'roaming', status: 'ok', title: _('802.11k/v is enabled on all managed networks') });
	}

	/* 5. Band steering is only possible where an SSID exists on several bands */
	const bandsBySsid = {};
	ifaces.forEach(function(i) {
		(bandsBySsid[i.ssid] = bandsBySsid[i.ssid] || {})[i.band || '?'] = true;
	});
	const singleBand = Object.keys(bandsBySsid).filter(function(s) {
		return Object.keys(bandsBySsid[s]).length < 2;
	});
	if (singleBand.length && Object.keys(bandsBySsid).length) {
		checks.push({
			id: 'bands', status: 'info',
			title: _('Some SSIDs are only on one band here'),
			detail: _('DAWN can only move a client between bands on this device if the same SSID and password are used on each band. Roaming to other access points still works.'),
			items: singleBand.map(function(s) {
				return '"%s": %s'.format(s, Object.keys(bandsBySsid[s]).map(bandLabel).join(', '));
			})
		});
	}

	/* 6. Peer discovery */
	const netopt = String(cfg.network_option != null ? cfg.network_option : '2');
	if (netopt == '2' && !state.umdns) {
		checks.push({
			id: 'discovery', status: 'error',
			title: _('umdns is not running'),
			detail: _('DAWN is set to find other access points with umdns, but umdns is not running. Install and start the umdns package on every access point.'),
			fix: { type: 'link', label: _('Open Software'), href: 'admin/system/package-manager?query=umdns' }
		});
	}
	else if (netopt == '3' && !cfg.server_ip) {
		checks.push({
			id: 'discovery', status: 'error',
			title: _('No peer address set'),
			detail: _('DAWN is set to TCP without umdns discovery, but no Server IP is configured, so it cannot reach other access points.'),
			fix: { type: 'link', label: _('Open DAWN settings'), href: 'admin/network/dawn' }
		});
	}
	else if (netopt == '0' || netopt == '1') {
		checks.push({
			id: 'discovery', status: 'warning',
			title: _('Broadcast / multicast networking is less reliable'),
			detail: _('TCP with umdns discovery is the most tested way for DAWN instances to talk to each other. Consider switching Network option to it.'),
			fix: { type: 'link', label: _('Open DAWN settings'), href: 'admin/network/dawn' }
		});
	}
	else {
		checks.push({ id: 'discovery', status: 'ok', title: _('Peer discovery is configured') });
	}

	/* 7. Can we actually see other access points? */
	if (state.dawnRunning && state.overview) {
		const local = {};
		ifaces.concat(state.ifaces || []).forEach(function(i) {
			if (i.bssid) local[String(i.bssid).toUpperCase()] = true;
		});

		const remote = {};
		Object.keys(state.overview).forEach(function(ssid) {
			const aps = state.overview[ssid];
			if (!aps || typeof aps !== 'object') return;
			Object.keys(aps).forEach(function(bssid) {
				if (!local[bssid.toUpperCase()])
					remote[bssid.toUpperCase()] = (aps[bssid] && aps[bssid].hostname) || bssid;
			});
		});

		const remoteHosts = {};
		Object.keys(remote).forEach(function(b) { remoteHosts[remote[b]] = true; });
		const n = Object.keys(remote).length;

		checks.push(n ? {
			id: 'peers', status: 'ok',
			title: _('Connected to %d access point(s) on other devices').format(n),
			items: Object.keys(remoteHosts).sort()
		} : {
			id: 'peers', status: 'info',
			title: _('No other access points found'),
			detail: _('Only this device is visible. That is fine for band steering on a single router. If you have more access points, install DAWN on each, use the same settings, and make sure TCP port %s is reachable between them.').format(cfg.tcp_port || '1026')
		});
	}

	/* 8. Kicking: with it off DAWN only observes */
	if (cfg.kicking == null || String(cfg.kicking) == '0') {
		checks.push({
			id: 'kicking', status: 'warning',
			title: _('Client steering ("kicking") is turned off'),
			detail: _('DAWN is collecting information but will never move a client. Turn on kicking to let it steer clients to a better access point.'),
			fix: { type: 'action', action: 'enableKicking', label: _('Turn on steering') }
		});
	}
	else {
		checks.push({ id: 'kicking', status: 'ok', title: _('Client steering is turned on') });
	}

	/* 9. Peer authentication (dawn_auth) */
	const key = cfg.auth_key || '';
	const port = cfg.tcp_port || '1026';
	if (!key) {
		checks.push({
			id: 'auth', status: 'warning',
			title: _('Messages between access points are not authenticated'),
			detail: (String(cfg.accept_config_push) == '1'
				? _('Any device on your network that can reach TCP port %s can feed DAWN made-up data and change its settings on every access point.')
				: _('Any device on your network that can reach TCP port %s can feed DAWN made-up data.')).format(port)
				+ ' ' + _('Generate a shared key here, then enter the same key on every other access point (Network → DAWN → Peer security).'),
			fix: { type: 'action', action: 'generateKey', label: _('Generate shared key') }
		});
	}
	else if (!/^[0-9a-fA-F]{64}$/.test(key)) {
		checks.push({
			id: 'auth', status: 'error',
			title: _('The shared key is not valid'),
			detail: _('It must be exactly 64 hexadecimal characters. DAWN ignores an invalid key, so messages are not authenticated.'),
			fix: { type: 'link', label: _('Open DAWN settings'), href: 'admin/network/dawn' }
		});
	}
	else if (netopt == '0' || netopt == '1') {
		checks.push({
			id: 'auth', status: 'warning',
			title: _('The shared key has no effect in broadcast / multicast mode'),
			detail: _('Message authentication only works with TCP networking. Switch Network option to TCP with umdns discovery.'),
			fix: { type: 'link', label: _('Open DAWN settings'), href: 'admin/network/dawn' }
		});
	}
	else {
		checks.push({
			id: 'auth', status: 'ok',
			title: _('Messages between access points are authenticated'),
			detail: _('Every access point must use this same key. Messages from devices without it are ignored.'),
			fix: { type: 'reveal', label: _('Show key'), value: key }
		});
	}

	/* 10. What DAWN has actually done (needs get_stats) */
	if (state.stats && state.stats.steering) {
		const st = state.stats.steering;
		const sent = (st.sent_rssi_compare || 0) + (st.sent_absolute_rssi || 0);
		const moved = st.moved || 0, stayed = st.stayed || 0;
		const peersRejected = state.stats.peers ? (state.stats.peers.messages_rejected_auth || 0) : 0;

		checks.push({
			id: 'activity',
			status: (sent >= 3 && moved == 0 && stayed > 0) ? 'warning' : 'info',
			title: sent
				? _('Steering activity: %d request(s) sent, %d client(s) moved, %d stayed').format(sent, moved, stayed)
				: _('No clients have been steered yet'),
			detail: (sent >= 3 && moved == 0 && stayed > 0)
				? _('Clients are receiving requests but not moving. They may not support 802.11v, or the suggested access points may be too weak from where they are.')
				: (sent ? null : _('That is normal until a client has a clearly better access point available.')),
			items: peersRejected ? [ _('%d message(s) from devices without the shared key were ignored').format(peersRejected) ] : null,
			fix: { type: 'link', label: _('Open Steering'), href: 'admin/status/dawn/steering' }
		});
	}

	/* 11. Known-problem options */
	if (isOn(cfg.use_symm_enc)) {
		checks.push({
			id: 'encryption', status: 'warning',
			title: _('Encryption between DAWN instances is enabled'),
			detail: _('This option is reported to be broken and can stop access points from understanding each other. Turn off use_symm_enc unless you have confirmed it works for you.')
		});
	}

	if (parseInt(cfg.loglevel, 10) >= 2) {
		checks.push({
			id: 'loglevel', status: 'info',
			title: _('Debug logging is on'),
			detail: _('DAWN is writing detailed debug traces to the system log, which can fill it quickly on small devices.'),
			fix: { type: 'action', action: 'quietLogging', label: _('Reduce logging') }
		});
	}

	checks.sort(function(a, b) { return STATUS_ORDER[a.status] - STATUS_ORDER[b.status]; });
	return checks;
}

function summarise(checks) {
	const counts = { error: 0, warning: 0, info: 0, ok: 0 };
	checks.forEach(function(c) { counts[c.status]++; });

	if (counts.error)
		return { status: 'error', text: _('DAWN will not work until the problems below are fixed.') };
	if (counts.warning)
		return { status: 'warning', text: _('DAWN is running, but the warnings below will stop it from steering some or all clients.') };
	return { status: 'ok', text: _('Everything DAWN needs is in place.') };
}

const callUmdnsList = function() {
	return rpc.list('umdns').then(function(sig) {
		return L.isObject(sig) && 'umdns' in sig;
	}).catch(function() { return false; });
};

/* 'band' is the current option; older configs only have hwmode.
 * (WifiNetwork.getWifiDevice() returns a Promise, so read the radio's UCI
 * section directly.) */
function detectBand(radio, net) {
	const band = radio ? uci.get('wireless', radio, 'band') : null;
	if (band)
		return band;

	const freq = parseFloat(net.getFrequency ? net.getFrequency() : NaN); /* GHz */
	if (!isNaN(freq))
		return freq < 3 ? '2g' : (freq < 5.925 ? '5g' : (freq < 10 ? '6g' : '60g'));

	const hwmode = radio ? (uci.get('wireless', radio, 'hwmode') || '') : '';
	if (/a$/.test(hwmode)) return '5g';
	if (/[bg]$/.test(hwmode)) return '2g';
	return null;
}

function gatherIfaces() {
	return network.getWifiNetworks().then(function(nets) {
		const aps = nets.filter(function(n) {
			const mode = n.getMode();
			return !n.isDisabled() && (mode == 'ap' || mode == 'ap-wds');
		});

		return Promise.all(aps.map(function(n) {
			const radio = n.getWifiDeviceName();
			const ifname = n.getIfname();
			const iface = {
				section: n.getName(),
				ssid: n.getSSID(),
				band: detectBand(radio, n),
				ifname: ifname,
				bssid: n.getActiveBSSID ? n.getActiveBSSID() : null,
				up: !!(ifname && n.isUp()),
				ieee80211k: n.get('ieee80211k'),
				bss_transition: n.get('bss_transition'),
				methods: null
			};

			if (!iface.up)
				return iface;

			const obj = 'hostapd.' + ifname;
			return rpc.list(obj).then(function(sig) {
				iface.methods = {};
				if (L.isObject(sig) && L.isObject(sig[obj]))
					Object.keys(sig[obj]).forEach(function(m) { iface.methods[m] = true; });
				return iface;
			}).catch(function() { return iface; });
		}));
	});
}

return view.extend({
	handleSaveApply: null,
	handleSave: null,
	handleReset: null,

	/* exported for tests */
	evaluate: evaluate,
	summarise: summarise,

	load: function() {
		return Promise.all([
			L.resolveDefault(dawn.isDawnRPCAvailable(), false),
			uci.load('dawn'),
			L.resolveDefault(L.probeSystemFeatures(), null)
		]).then(function(r) {
			const running = r[0];
			return Promise.all([
				running ? L.resolveDefault(dawn.callDawnGetNetwork(), null) : null,
				callUmdnsList(),
				L.resolveDefault(gatherIfaces(), []),
				running ? L.resolveDefault(dawn.callDawnGetStats(), null) : null
			]).then(function(r2) {
				const local = uci.get_first('dawn', 'local') || {};
				const net = uci.get_first('dawn', 'network') || {};
				const hostapd = uci.get_first('dawn', 'hostapd') || {};
				const metric = uci.get('dawn', 'global') || {};
				const steering = hostapd.steering_ssid;

				return {
					dawnRunning: !!running,
					overview: r2[0],
					umdns: r2[1],
					ifaces: r2[2],
					stats: r2[3],
					fullHostapd: L.hasSystemFeature('hostapd', 'eap'),
					config: {
						network_option: net.network_option,
						server_ip: net.server_ip,
						tcp_port: net.tcp_port,
						use_symm_enc: net.use_symm_enc,
						auth_key: net.auth_key,
						accept_config_push: net.accept_config_push,
						loglevel: local.loglevel,
						kicking: metric.kicking,
						steering_ssid: Array.isArray(steering) ? steering : (steering ? [ steering ] : [])
					}
				};
			});
		});
	},

	/* --- one-click fixes: stage UCI changes, then apply with rollback --- */

	applyChanges: function() {
		return uci.save()
			.then(L.bind(ui.changes.init, ui.changes))
			.then(L.bind(ui.changes.apply, ui.changes, true));
	},

	enableRoaming: function(fix) {
		return uci.load('wireless').then(L.bind(function() {
			fix.sections.forEach(function(sid) {
				uci.set('wireless', sid, 'ieee80211k', '1');
				uci.set('wireless', sid, 'bss_transition', '1');
			});
			return this.applyChanges();
		}, this));
	},

	enableKicking: function() {
		/* 3 = both RSSI comparison and absolute RSSI, DAWN's documented default */
		if (!uci.get('dawn', 'global'))
			uci.add('dawn', 'metric', 'global');
		uci.set('dawn', 'global', 'kicking', '3');
		return this.applyChanges();
	},

	generateKey: function() {
		const sid = uci.get_first('dawn', 'network');
		const bytes = new Uint8Array(32);
		window.crypto.getRandomValues(bytes);
		const key = Array.prototype.map.call(bytes, function(b) { return ('0' + b.toString(16)).slice(-2); }).join('');

		if (sid)
			uci.set('dawn', sid['.name'], 'auth_key', key);
		else
			uci.set('dawn', uci.add('dawn', 'network'), 'auth_key', key);

		return this.applyChanges();
	},

	quietLogging: function() {
		const sid = uci.get_first('dawn', 'local');
		if (sid)
			uci.set('dawn', sid['.name'], 'loglevel', '0');
		return this.applyChanges();
	},

	/* --- rendering --- */

	renderFix: function(fix) {
		if (!fix)
			return '';

		if (fix.type == 'reveal') {
			const btn = E('button', { 'class': 'btn cbi-button' }, fix.label);
			btn.addEventListener('click', function() {
				btn.parentNode.replaceChild(E('code', { 'style': 'word-break:break-all; user-select:all' }, fix.value), btn);
			});
			return btn;
		}

		if (fix.type == 'link') {
			const parts = fix.href.split('?');
			return E('a', {
				'class': 'btn cbi-button',
				'href': L.url(parts[0]) + (parts[1] ? '?' + parts[1] : '')
			}, fix.label);
		}

		return E('button', {
			'class': 'btn cbi-button cbi-button-apply',
			'click': ui.createHandlerFn(this, this[fix.action], fix)
		}, fix.label);
	},

	renderCheck: function(c) {
		const badge = {
			error: [ 'important', _('Problem') ],
			warning: [ 'warning', _('Warning') ],
			info: [ 'notice', _('Note') ],
			ok: [ 'success', _('OK') ]
		}[c.status];

		return E('tr', { 'class': 'tr', 'data-check': c.id, 'data-status': c.status }, [
			E('td', { 'class': 'td', 'style': 'width:1%; vertical-align:top; white-space:nowrap' },
				E('span', {
					'class': 'label ' + badge[0],
					// Themes have no red label class; use the error colour where defined
					'style': c.status == 'error' ? 'background-color:var(--error-color-high, #c9302c); color:#fff' : null
				}, badge[1])),
			/* Fix button sits under the explanation so the text keeps its width on phones */
			E('td', { 'class': 'td' }, [
				E('strong', {}, c.title),
				c.detail ? E('p', { 'style': 'margin:.25em 0 0' }, c.detail) : '',
				(c.items && c.items.length)
					? E('ul', { 'style': 'margin:.25em 0 0 1.5em' }, c.items.map(function(t) { return E('li', {}, t); }))
					: '',
				c.fix ? E('div', { 'style': 'margin-top:.5em' }, this.renderFix(c.fix)) : ''
			])
		]);
	},

	render: function(state) {
		const checks = evaluate(state);
		const summary = summarise(checks);
		const alertClass = { error: 'danger', warning: 'warning', ok: 'success' }[summary.status];

		return E([
			E('h2', _('Setup Check')),
			E('p', { 'class': 'cbi-section-descr' },
				_('Checks that this device has everything DAWN needs to steer clients. Run it on each access point.')),
			E('div', { 'class': 'alert-message ' + alertClass }, summary.text),
			E('table', { 'class': 'table' }, checks.map(L.bind(this.renderCheck, this)))
		]);
	}
});
