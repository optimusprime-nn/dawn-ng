'use strict';
'require view';
'require poll';
'require dawn.dawn-common as dawn';

/*
 * What DAWN has actually done: counters from `ubus call dawn get_stats` and
 * the most recent steering requests with their outcome.
 */

function fmtAge(s) {
	if (s < 60) return _('%ds ago').format(s);
	if (s < 3600) return _('%dm ago').format(Math.floor(s / 60));
	if (s < 86400) return _('%dh ago').format(Math.floor(s / 3600));
	return _('%dd ago').format(Math.floor(s / 86400));
}

function fmtDuration(s) {
	const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
	return (d ? d + 'd ' : '') + (d || h ? h + 'h ' : '') + m + 'm';
}

const METHOD = {
	rssi_compare: [ _('Better AP'), _('Another AP scored clearly higher (802.11v disassociation imminent)') ],
	absolute_rssi: [ _('Weak signal'), _('Signal fell below rssi_center (802.11v BSS transition request)') ],
	force_reconnect: [ _('Reconnect'), _('No information about the client: deauthenticated so it reconnects') ]
};

const OUTCOME = {
	moved: [ 'success', _('Moved') ],
	stayed: [ 'warning', _('Stayed') ],
	failed: [ 'important', _('Failed') ],
	pending: [ 'notice', _('Pending') ]
};

function tile(value, label, hint) {
	return E('div', { 'style': 'flex:1 1 9em; padding:.6em .8em; border:1px solid var(--border-color-medium, #ddd); border-radius:4px', 'title': hint || '' }, [
		E('div', { 'style': 'font-size:1.6em; font-weight:bold; line-height:1.2' }, String(value)),
		E('div', { 'style': 'opacity:.75' }, label)
	]);
}

return view.extend({
	handleSaveApply: null,
	handleSave: null,
	handleReset: null,

	load: function() {
		return dawn.isDawnRPCAvailable().then(function(available) {
			if (!available)
				return [ null, {} ];
			return Promise.all([
				L.resolveDefault(dawn.callDawnGetStats(), null),
				L.resolveDefault(dawn.callHostHints(), {})
			]);
		});
	},

	renderStats: function(stats, hosts) {
		if (!stats || !stats.steering)
			return E('div', { 'class': 'alert-message warning' },
				_('This DAWN version does not report statistics (get_stats). Install the matching dawn package.'));

		const st = stats.steering, peers = stats.peers || {};
		const sent = (st.sent_rssi_compare || 0) + (st.sent_absolute_rssi || 0);
		const resolved = (st.moved || 0) + (st.stayed || 0);

		const rows = (stats.recent_kicks || []).map(function(k) {
			const m = METHOD[k.method] || [ k.method, '' ];
			const o = OUTCOME[k.outcome] || [ 'notice', k.outcome ];
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, fmtAge(k.age)),
				E('td', { 'class': 'td' }, dawn.getHostnameFromMAC(hosts, k.client)),
				E('td', { 'class': 'td' }, k.from),
				E('td', { 'class': 'td' }, k.to || '-'),
				E('td', { 'class': 'td' }, E('span', { 'title': m[1] }, m[0] + (k.score_gain ? ' (+' + k.score_gain + ')' : ''))),
				E('td', { 'class': 'td' }, [
					E('span', { 'class': 'label ' + o[0],
						'style': k.outcome == 'failed' ? 'background-color:var(--error-color-high, #c9302c); color:#fff' : null }, o[1]),
					(k.outcome_after != null && k.outcome != 'failed') ? ' ' + _('after %ds').format(k.outcome_after) : ''
				])
			]);
		});

		return E('div', {}, [
			E('p', { 'class': 'cbi-section-descr' },
				_('Running for %s. Peer messages %s.').format(fmtDuration(stats.uptime || 0),
					stats.peer_auth ? _('are authenticated') : _('are NOT authenticated'))),

			E('h3', {}, _('Steering')),
			E('div', { 'style': 'display:flex; flex-wrap:wrap; gap:.6em; margin-bottom:1em' }, [
				tile(st.evaluations || 0, _('Client checks'), _('Times a connected client was evaluated')),
				tile(st.stays || 0, _('Already on best AP')),
				tile(sent, _('Steering requests sent')),
				tile(resolved ? Math.round(100 * (st.moved || 0) / resolved) + '%' : '–', _('Clients that moved'),
					_('Of resolved requests: moved to another AP vs. still on the same AP after 60 s')),
				tile((st.deferred || 0) + (st.skipped_active || 0), _('Requests held back'),
					_('Waiting for min_number_to_kick confirmations, or client was busy transferring data')),
				tile(st.failed || 0, _('Failed requests'), _('hostapd refused the request'))
			]),

			E('h3', {}, _('Recent steering requests')),
			rows.length
				? E('table', { 'class': 'table' }, [
					E('tr', { 'class': 'tr table-titles' }, [
						E('th', { 'class': 'th' }, _('When')),
						E('th', { 'class': 'th' }, _('Client')),
						E('th', { 'class': 'th' }, _('From AP')),
						E('th', { 'class': 'th' }, _('Suggested AP')),
						E('th', { 'class': 'th' }, _('Reason')),
						E('th', { 'class': 'th' }, _('Result'))
					])
				].concat(rows))
				: E('em', {}, _('No clients have been steered yet. That is normal until a client has a clearly better access point available.')),

			E('h3', { 'style': 'margin-top:1em' }, _('Messages from other access points')),
			E('div', { 'style': 'display:flex; flex-wrap:wrap; gap:.6em' }, [
				tile(peers.messages_received || 0, _('Received')),
				tile(peers.messages_rejected_auth || 0, _('Ignored: no / wrong key')),
				tile(peers.config_push_applied || 0, _('Settings applied from peers')),
				tile(peers.config_push_ignored || 0, _('Settings from peers ignored'))
			])
		]);
	},

	render: function(data) {
		const stats = data[0], hosts = data[1] || {};
		if (stats === null)
			return dawn.getDawnServiceNotRunningErrorMessage();

		const container = E('div', {}, this.renderStats(stats, hosts));

		poll.add(L.bind(function() {
			return Promise.all([
				L.resolveDefault(dawn.callDawnGetStats(), null),
				L.resolveDefault(dawn.callHostHints(), hosts)
			]).then(L.bind(function(r) {
				if (r[0])
					container.replaceChildren(this.renderStats(r[0], r[1] || hosts));
			}, this));
		}, this), 10);

		return E([
			E('h2', {}, _('Steering')),
			container
		]);
	}
});
