'use strict';
'require view';
'require form';
'require uci';
'require rpc';
'require ui';

var commitSettings = rpc.declare({
    object: 'uci', method: 'commit', params: [ 'config' ], reject: true
});

return view.extend({
    load: function() { return uci.load('xgspon'); },
    render: function() {
        var m = new form.Map('xgspon', _('XGS-PON Settings'),
            _('Saving commits and applies changed settings. Identity or optical startup changes restart the optical service and briefly interrupt Internet access. IPv4 passthrough changes apply separately, without restarting optics. Disabled services stay disabled. Edits on a RAM image last until reboot.'));
        this.map = m;
        var startup = m.section(form.NamedSection, 'service', 'service');
        startup.anonymous = true; startup.addremove = false;
        var o = startup.option(form.Flag, 'enabled', _('Start PON Internet service at boot'),
            _('Register with the provider using the identity below. A prepared service image also starts WAN networking automatically after registration and provisioning. Verified optical firmware and unit calibration are required.'));
        o.default = '1'; o.rmempty = false;
        o = startup.option(form.Flag, 'monitor', _('Load optical controller at boot'),
            _('When Internet service is disabled, read optical diagnostics with transmission disabled. Initialization waits for verified optical firmware and this unit’s calibration. Private service images include these inputs.'));
        o.default = '1'; o.rmempty = false;
        var passthrough = m.section(form.NamedSection, 'passthrough', 'passthrough', _('IPv4 passthrough'),
            _('Give the public WAN address to one downstream router while keeping the provider connection on this device. Connect the router WAN port to LAN and use DHCP. Both devices keep their own MAC addresses. IPv6 prefix delegation is configured separately under Network > Interfaces.'));
        passthrough.anonymous = true; passthrough.addremove = false;
        o = passthrough.option(form.ListValue, 'mode', _('Mode'));
        o.value('router', _('Router (default)'));
        o.value('l3', _('L3 IP passthrough'));
        o.default = 'router'; o.rmempty = false;
        o = passthrough.option(form.Value, 'client_mac', _('Downstream WAN MAC address'),
            _('Use the MAC address of the main router’s WAN port. Save to apply, then renew its DHCP lease. Returning to Router mode also requires renewing the downstream lease. Keep a separate static management address to access this device.'));
        o.depends('mode', 'l3'); o.rmempty = false;
        o.validate = function(section, v) {
            return !!v && v.length === 17 && /^[0-9A-Fa-f][02468aAcCeE](:[0-9A-Fa-f]{2}){5}$/.test(v) && v !== '00:00:00:00:00:00' ||
                _('Expected a nonzero unicast MAC address');
        };
        var s = m.section(form.NamedSection, 'identity', 'identity', _('PON identity'));
        s.anonymous = true;
        s.addremove = false;
        s.tab('pon', _('PON'));
        s.tab('compat', _('Compatibility'));
        s.tab('iphost', _('IP host identity'));
        var text = function(key, title, length, description, secret, tab) {
            var o = s.taboption(tab || 'pon', form.Value, key, _(title), _(description));
            o.rmempty = true;
            o.password = !!secret;
            o.validate = function(section, v) {
                return !v || (v.length <= length && /^[\x20-\x7e]+$/.test(v)) ||
                    _('Use at most %d printable ASCII characters').format(length);
            };
            return o;
        };
        o = text('serial', 'PON Serial Number (ONT ID)', 12,
            'GPON Serial Number sent to the OLT in various MEs (4 alphanumeric characters, followed by 8 hex digits).');
        o.validate = function(section, v) {
            return !v || (v.length === 12 && /^[A-Za-z0-9]{4}[0-9A-Fa-f]{8}$/.test(v)) ||
                _('Expected four vendor letters or digits and eight hexadecimal digits');
        };
        o = text('vendor_id', 'Vendor ID', 4,
            'PON Vendor ID sent in various MEs, automatically derived from the PON Serial Number if not set (4 alphanumeric characters).');
        o.validate = function(section, v) {
            return !v || (v.length === 4 && /^[A-Za-z0-9]{4}$/.test(v)) || _('Expected four vendor letters or digits');
        };
        text('equipment_id', 'Equipment ID', 20, 'PON Equipment ID field in the ONU2-G ME [257] (up to 20 characters).');
        text('hardware_version', 'Hardware Version', 14, 'Hardware version string sent in various MEs (up to 14 characters).');
        o = s.taboption('pon', form.Flag, 'sync_circuit_pack', _('Sync Circuit Pack Version'),
            _('Set the Version field of any Circuit Pack MEs [6] to match the Hardware Version (if set).'));
        o.default = '1'; o.rmempty = false;
        text('software_version_a', 'Software Version A', 14,
            'Image specific software version sent in the Software image MEs [7] (up to 14 characters).');
        text('software_version_b', 'Software Version B', 14,
            'Image specific software version sent in the Software image MEs [7] (up to 14 characters).');
        ['active', 'committed'].forEach(function(kind) {
            var title = kind === 'active' ? _('Override active firmware bank') : _('Override committed firmware bank');
            var o = s.taboption('pon', form.ListValue, kind + '_bank', title,
                kind === 'active' ? _('Override which software bank is marked as active in the Software image MEs [7].') : _('Override which software bank is marked as committed in the Software image MEs [7].'));
            o.value('', _('Native default (A)')); o.value('0', _('A')); o.value('1', _('B'));
            o.rmempty = true;
        });
        o = text('registration_id', 'Registration ID (HEX)', 72,
            'Registration ID (up to 36 bytes) sent to the OLT, in hex format. This is where you would set a ploam password (which is contained in the last 12 bytes).', true);
        o.validate = function(section, v) {
            return !v || (v.length % 2 === 0 && /^(?:[0-9A-Fa-f]{2}){1,36}$/.test(v)) || _('Expected 1–36 bytes in hexadecimal');
        };
        text('logical_onu_id', 'Logical ONU ID', 24, 'Logical ONU ID presented in the ONU-G ME [256] (up to 24 characters).', true);
        text('logical_password', 'Logical Password', 12, 'Logical Password presented in the ONU-G ME [256] (up to 12 characters).', true);
        o = text('wan_mac', 'WAN MAC override', 17, 'MAC address of the WAN interface (XX:XX:XX:XX:XX:XX format). Empty uses the factory value.');
        o.validate = function(section, v) {
            return !v || (/^[0-9A-Fa-f][02468aAcCeE](:[0-9A-Fa-f]{2}){5}$/.test(v) && v.length === 17 && v !== '00:00:00:00:00:00') ||
                _('Expected a nonzero unicast MAC address');
        };
        o = text('omcc_version', 'OMCC Version', 4,
            'The OMCC version to use in hexadecimal format between 0x80 and 0xBF.', false, 'compat');
        o.placeholder = '0xA0';
        o.validate = function(section, v) {
            return !v || /^0x[89aAbB][0-9a-fA-F]$/.test(v) && v.length === 4 ||
                _('Expected a hexadecimal version from 0x80 to 0xBF');
        };
        o = text('pon_slot', 'PON Slot', 3,
            'Change the slot number that the UNI port is presented on, needed on some ISPs.', false, 'compat');
        o.placeholder = '1';
        o.validate = function(section, v) {
            return !v || (/^[1-9][0-9]{0,2}$/.test(v) && !/[^0-9]/.test(v) && Number(v) <= 254 && Number(v) !== 128) ||
                _('Use a slot from 1 to 254, except reserved slot 128');
        };
        o = s.taboption('compat', form.ListValue, 'olt_profile', _('OLT interoperability profile'),
            _('Compatibility profile for the connected OLT. Automatically detected from the OLT-G identity if set to Automatic.'));
        [['auto', _('Automatic')], ['generic', _('Generic')], ['nokia', _('Nokia / Alcatel-Lucent')],
            ['dasan', _('DASAN')], ['huawei', _('Huawei')], ['fiberhome', _('FiberHome')], ['zte', _('ZTE')]]
            .forEach(function(v) { o.value(v[0], v[1]); });
        o.default = 'auto'; o.rmempty = false;
        o = text('iphost_mac', 'IP Host MAC Address', 17,
            'MAC address sent in the IP host config data ME [134] (XX:XX:XX:XX:XX:XX format).', false, 'iphost');
        o.validate = function(section, v) {
            return !v || (/^[0-9A-Fa-f][02468aAcCeE](:[0-9A-Fa-f]{2}){5}$/.test(v) && v.length === 17 && v !== '00:00:00:00:00:00') ||
                _('Expected a nonzero unicast MAC address');
        };
        text('iphost_hostname', 'IP Host Hostname', 25,
            'Hostname sent in the IP host config data ME [134] (up to 25 characters).', false, 'iphost');
        text('iphost_domain', 'IP Host Domain Name', 25,
            'Domain name sent in the IP host config data ME [134] (up to 25 characters).', false, 'iphost');
        o = s.taboption('compat', form.Flag, 'fix_vlans', _('Fix VLANs (untagged Internet)'),
            _('Allow untagged Internet traffic when the OLT uses priority-tagged VLAN 0, and remove the priority tag from downstream traffic.'));
        o.default = '0'; o.rmempty = false;
        text('omci_version', 'Legacy combined OMCI version', 14,
            'Legacy fallback for hardware and both software versions. Separate version fields above take precedence. Leave empty for new configurations.');
        return m.render();
    },
    handleSave: function() {
        return this.map.save(null, true).then(function() {
            return commitSettings('xgspon');
        }).then(function() {
            return ui.changes.init();
        }).then(function() {
            ui.addNotification(null, E('p', {}, _('Settings saved. The running services will apply changes automatically; optical changes may briefly interrupt Internet access.')), 'info');
        }).catch(function(error) {
            ui.addNotification(null, E('p', {}, _('Unable to save XGS-PON settings: %s').format(error.message)), 'error');
            throw error;
        });
    },
    handleSaveApply: function() {
        // Both buttons commit only xgspon; the section owners apply changes.
        return this.handleSave();
    }
});
