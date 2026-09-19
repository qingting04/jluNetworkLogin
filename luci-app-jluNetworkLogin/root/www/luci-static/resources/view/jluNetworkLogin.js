'use strict';

'require view';
'require form';
'require uci';
'require rpc';
'require poll';
'require ui';
'require tools.widgets as widgets';

var callStatus = rpc.declare({
	object: 'jlu-network-login',
	method: 'status',
	expect: { '': {} }
});

var callReconnect = rpc.declare({
	object: 'jlu-network-login',
	method: 'reconnect',
	expect: { '': {} }
});

var callNetworkReload = rpc.declare({
	object: 'network',
	method: 'reload',
	expect: { '': {} }
});

var callInitAction = rpc.declare({
	object: 'rc',
	method: 'init',
	params: [ 'name', 'action' ]
});

function state_label(state) {
	var map = {
		'idle': _('Idle'),
		'online': _('Online'),
		'challenge': _('Fetching challenge'),
		'login': _('Logging in'),
		'keepalive': _('Keeping alive')
	};

	return map[state] || state || '-';
}

/* 守护进程把「缺哪项配置」写进 last_error（如 ip required），这里翻成可读文案；
 * 未收录的原文照原样显示。 */
function err_label(err) {
	var map = {
		'disabled': _('Service is disabled'),
		'username required': _('Username is required'),
		'password required': _('Password is required'),
		'ip required': _('IP address is required'),
		'mac required': _('MAC address is required'),
		'manual reconnect': _('Manual reconnect')
	};

	return err ? (map[err] || err) : '-';
}

function updateStatusBox(res) {
	var set = function(id, val) {
		var el = document.getElementById(id);
		if (el)
			el.textContent = val;
	};

	set('drcom-status-state', state_label(res && res.state));
	set('drcom-status-err', err_label(res && res.last_error));
}

return view.extend({
	handleOneClick: function(m, ev) {
		return m.save().then(function() {
			var ifname = uci.get('jlu-network-login', 'main', 'interface');
			var ip = uci.get('jlu-network-login', 'main', 'ip');
			var mac = uci.get('jlu-network-login', 'main', 'mac');
			var gw = uci.get('jlu-network-login', 'main', 'gateway');

			/* 缺项时什么都不做：不写任何配置、不弹提示 */
			if (!ifname || !ip || !mac || !gw)
				return;

			mac = String(mac).toLowerCase();

			return Promise.all([ uci.load('network'), uci.load('dhcp') ]).then(function() {
				if (!uci.get('network', ifname))
					throw new Error(_('Network interface "%s" not found').format(ifname));

				var dnsmasq = uci.sections('dhcp', 'dnsmasq');
				var dnsmasqSid = (dnsmasq && dnsmasq.length) ? dnsmasq[0]['.name'] : null;
				if (!dnsmasqSid)
					dnsmasqSid = uci.add('dhcp', 'dnsmasq');

				var oldProto = uci.get('network', ifname, 'proto');
				var oldIpaddr = uci.get('network', ifname, 'ipaddr');
				var oldNetmask = uci.get('network', ifname, 'netmask');
				var oldGateway = uci.get('network', ifname, 'gateway');
				var oldDns = uci.get('network', ifname, 'dns');
				var oldMacaddr = uci.get('network', ifname, 'macaddr');
				var oldRebind = uci.get('dhcp', dnsmasqSid, 'rebind_protection');

				uci.set('jlu-network-login', 'main', 'backup_ifname', ifname);
				uci.set('jlu-network-login', 'main', 'backup_proto', oldProto || '');
				uci.set('jlu-network-login', 'main', 'backup_ipaddr', oldIpaddr || '');
				uci.set('jlu-network-login', 'main', 'backup_netmask', oldNetmask || '');
				uci.set('jlu-network-login', 'main', 'backup_gateway', oldGateway || '');
				uci.set('jlu-network-login', 'main', 'backup_dns', oldDns || '');
				uci.set('jlu-network-login', 'main', 'backup_macaddr', oldMacaddr || '');
				uci.set('jlu-network-login', 'main', 'backup_rebind_protection', (oldRebind != null) ? String(oldRebind) : '');
				uci.set('jlu-network-login', 'main', 'backup_time', String(Date.now()));

				uci.set('network', ifname, 'proto', 'static');
				uci.set('network', ifname, 'ipaddr', ip);
				uci.set('network', ifname, 'macaddr', mac);
				uci.set('network', ifname, 'gateway', gw);
				uci.set('network', ifname, 'dns', [ '10.10.10.10', '202.98.18.3' ]);
				if (!uci.get('network', ifname, 'netmask'))
					uci.set('network', ifname, 'netmask', '255.255.255.0');

				uci.set('dhcp', dnsmasqSid, 'rebind_protection', '0');

				return uci.save();
			}).then(function() {
				return Promise.all([
					callNetworkReload().catch(function(e) {
						ui.addNotification(null, E('p', [ _('Failed to reload the network: %s').format(String(e)) ]), 'warning');
					}),
					callInitAction('dnsmasq', 'restart').catch(function(e) {
						ui.addNotification(null, E('p', [ _('Failed to restart dnsmasq: %s').format(String(e)) ]), 'warning');
					})
				]);
			}).then(function() {
				ui.addNotification(null, E('p', [ _('One-click setup applied (static IP, MAC address, gateway and DNS are set, DNS rebind protection is disabled).') ]), 'info');
			});
		}).catch(function(e) {
			ui.addNotification(null, E('p', [ String((e && e.message) ? e.message : e) ]), 'danger');
		});
	},

	handleRestore: function(m, ev) {
		ui.showModal(_('Restore the previous settings?'), [
			E('p', [ _('This restores the interface settings that were backed up before the last one-click setup and re-enables DNS rebind protection.') ]),
			E('div', { 'class': 'right' }, [
				E('button', {
					'class': 'btn',
					'click': ui.createHandlerFn(this, ui.hideModal)
				}, [ _('Cancel') ]), ' ',
				E('button', {
					'class': 'btn cbi-button-action important',
					'click': ui.createHandlerFn(this, 'handleRestoreConfirm')
				}, [ _('Continue') ])
			])
		]);
	},

	handleRestoreConfirm: function(ev) {
		ui.hideModal();

		return Promise.all([ uci.load('jlu-network-login'), uci.load('network'), uci.load('dhcp') ]).then(function() {
			var ifname = uci.get('jlu-network-login', 'main', 'backup_ifname');
			if (!ifname)
				throw new Error(_('No backup found - run the one-click setup first.'));
			if (!uci.get('network', ifname))
				throw new Error(_('Network interface "%s" not found').format(ifname));

			var setOrUnset = function(conf, sid, opt, val) {
				if (Array.isArray(val)) {
					if (!val.length)
						return uci.unset(conf, sid, opt);
					return uci.set(conf, sid, opt, val);
				}
				if (val == null || val === '')
					return uci.unset(conf, sid, opt);
				return uci.set(conf, sid, opt, val);
			};

			setOrUnset('network', ifname, 'proto', uci.get('jlu-network-login', 'main', 'backup_proto'));
			setOrUnset('network', ifname, 'ipaddr', uci.get('jlu-network-login', 'main', 'backup_ipaddr'));
			setOrUnset('network', ifname, 'netmask', uci.get('jlu-network-login', 'main', 'backup_netmask'));
			setOrUnset('network', ifname, 'gateway', uci.get('jlu-network-login', 'main', 'backup_gateway'));
			setOrUnset('network', ifname, 'dns', uci.get('jlu-network-login', 'main', 'backup_dns'));
			setOrUnset('network', ifname, 'macaddr', uci.get('jlu-network-login', 'main', 'backup_macaddr'));

			var dnsmasq = uci.sections('dhcp', 'dnsmasq');
			var dnsmasqSid = (dnsmasq && dnsmasq.length) ? dnsmasq[0]['.name'] : null;
			if (dnsmasqSid) {
				var rp = uci.get('jlu-network-login', 'main', 'backup_rebind_protection');
				uci.set('dhcp', dnsmasqSid, 'rebind_protection', (rp != null && rp !== '') ? rp : '1');
			}

			return uci.save();
		}).then(function() {
			return Promise.all([
				callNetworkReload().catch(function(e) {
					ui.addNotification(null, E('p', [ _('Failed to reload the network: %s').format(String(e)) ]), 'warning');
				}),
				callInitAction('dnsmasq', 'restart').catch(function(e) {
					ui.addNotification(null, E('p', [ _('Failed to restart dnsmasq: %s').format(String(e)) ]), 'warning');
				})
			]);
		}).then(function() {
			ui.addNotification(null, E('p', [ _('Restore applied.') ]), 'info');
		}).catch(function(e) {
			ui.addNotification(null, E('p', [ String((e && e.message) ? e.message : e) ]), 'danger');
		});
	},

	load: function() {
		return Promise.all([
			uci.load('jlu-network-login'),
			callStatus().catch(function() { return {}; })
		]);
	},

	render: function(data) {
		var initialStatus = data[1] || {};

		var m = new form.Map('jlu-network-login', _('JLU Network Login'));

		var s = m.section(form.NamedSection, 'main', 'main');
		s.addremove = false;

		var o;
		o = s.option(form.Flag, 'enabled', _('Enable'));
		o.default = o.disabled;

		o = s.option(form.Value, 'username', _('Username'));
		o.datatype = 'string';

		o = s.option(form.Value, 'password', _('Password'));
		o.password = true;
		o.datatype = 'string';

		o = s.option(widgets.NetworkSelect, 'interface', _('Interface'));
		o.nocreate = true;
		o.default = 'wan';

		o = s.option(form.Value, 'ip', _('IP address'));
		o.datatype = 'ip4addr';
		o.placeholder = '10.100.61.100';

		o = s.option(form.Value, 'gateway', _('Gateway'));
		o.datatype = 'ip4addr';
		o.placeholder = '10.100.61.1';

		o = s.option(form.Value, 'mac', _('MAC address'));
		o.datatype = 'macaddr';
		o.placeholder = 'aa:bb:cc:dd:ee:ff';

		return m.render().then(function(mapEl) {
			/* 状态与操作按钮放在同一块里，插到页面标题之后、设置之前
			 * （版式与 hustNetworkLogin 的页面保持一致） */
			var box = E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, [ _('Service status') ]),
				E('div', { 'class': 'table' }, [
					E('div', { 'class': 'tr' }, [
						E('div', { 'class': 'td left' }, [ _('Connection state') ]),
						E('div', { 'class': 'td left', 'id': 'drcom-status-state' },
							[ state_label(initialStatus && initialStatus.state) ])
					]),
					E('div', { 'class': 'tr' }, [
						E('div', { 'class': 'td left' }, [ _('Last error') ]),
						E('div', { 'class': 'td left', 'id': 'drcom-status-err' },
							[ err_label(initialStatus && initialStatus.last_error) ])
					])
				]),
				E('div', { 'class': 'cbi-page-actions' }, [
					E('button', {
						'class': 'cbi-button cbi-button-positive',
						'click': ui.createHandlerFn(this, function(ev) {
							var btn = ev.currentTarget;
							btn.disabled = true;
							return this.handleOneClick(m, ev).catch(function() {}).then(function() {
								btn.disabled = false;
							});
						})
					}, [ _('One-click setup') ]),
					' ',
					E('button', {
						'class': 'cbi-button cbi-button-negative',
						'click': ui.createHandlerFn(this, function(ev) { return this.handleRestore(m, ev); })
					}, [ _('One-click restore') ]),
					' ',
					E('button', {
						'class': 'cbi-button cbi-button-action',
						'click': ui.createHandlerFn(this, function(ev) {
							var btn = ev.currentTarget;
							btn.disabled = true;
							return callReconnect().catch(function(e) {
								ui.addNotification(null, E('p', [ _('Reconnect failed: %s').format(String(e)) ]), 'warning');
							}).then(function() {
								btn.disabled = false;
							});
						})
					}, [ _('Reconnect') ])
				])
			]);

			poll.add(function() {
				return callStatus().then(updateStatusBox).catch(function() {});
			});

			mapEl.insertBefore(box, mapEl.querySelector('.cbi-section'));

			return mapEl;
		}.bind(this));
	}
});
