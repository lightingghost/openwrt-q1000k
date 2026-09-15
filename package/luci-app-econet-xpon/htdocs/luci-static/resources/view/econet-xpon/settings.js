'use strict';
'require view';
'require form';
'require uci';

return view.extend({
    load: function() { return uci.load('q1000k-xgspon'); },
    render: function() {
        var m = new form.Map('q1000k-xgspon', _('XGS-PON configuration'),
            _('Settings take effect at the next explicit PON stack startup. Saving does not start the optical port. On the RAM bench, settings last until reboot and optical TX remains inhibited.'));
        var s = m.section(form.NamedSection, 'identity', 'identity', _('PON identity'));
        s.anonymous = true;
        s.addremove = false;
        s.tab('pon', _('PON'));
        s.tab('compat', _('Compatibility'));
        var text = function(key, title, length, description, secret) {
            var o = s.taboption('pon', form.Value, key, _(title), _(description));
            o.rmempty = true;
            o.password = !!secret;
            o.validate = function(section, v) {
                return !v || (v.length <= length && /^[\x20-\x7e]+$/.test(v)) ||
                    _('Use at most %d printable ASCII characters').format(length);
            };
            return o;
        };
        var o = text('serial', 'PON Serial Number (ONT ID)', 12,
            'Four vendor letters or digits followed by eight hex digits. Empty uses this Q1000K’s factory FSAN. For AT&T, use the ONT ID from your BGW320 label or fiber status page.');
        o.validate = function(section, v) {
            return !v || (v.length === 12 && /^[A-Za-z0-9]{4}[0-9A-Fa-f]{8}$/.test(v)) ||
                _('Expected four vendor letters or digits and eight hexadecimal digits');
        };
        o = text('vendor_id', 'Vendor ID', 4,
            'Optional OMCI vendor ID, derived from the PON serial when empty. Does not change the registration serial.');
        o.validate = function(section, v) {
            return !v || (v.length === 4 && /^[A-Za-z0-9]{4}$/.test(v)) || _('Expected four vendor letters or digits');
        };
        text('equipment_id', 'Equipment ID', 20, 'ONU2-G equipment ID. BGW320-500 reference: iONT320500X.');
        text('hardware_version', 'Hardware Version', 14, 'ONU-G version. BGW320-500 reference: BGW320-500_2.1.');
        o = s.taboption('pon', form.Flag, 'sync_circuit_pack', _('Sync Circuit Pack Version'),
            _('Report the hardware version in every Circuit Pack. Disabled keeps the native OpenWrt Circuit Pack version.'));
        o.default = '1'; o.rmempty = false;
        text('software_version_a', 'Software Version A', 14,
            'Software Image A version. Copy your gateway’s value; BGW320_4.27.7 is a guide example.');
        text('software_version_b', 'Software Version B', 14,
            'Software Image B version, independent of A and the hardware version.');
        ['active', 'committed'].forEach(function(kind) {
            var title = kind === 'active' ? _('Override active firmware bank') : _('Override committed firmware bank');
            var o = s.taboption('pon', form.ListValue, kind + '_bank', title,
                _('Changes only the Software Image attributes advertised to the OLT. Does not select, write or boot a flash partition.'));
            o.value('', _('Native default (A)')); o.value('0', _('A')); o.value('1', _('B'));
            o.rmempty = true;
        });
        o = text('registration_id', 'Registration ID (HEX)', 72,
            '1–36 bytes encoded as pairs of hex digits, zero-padded on the right. For a PLOAM password in the final 12 bytes, enter the full 36-byte value with the leading padding. Empty leaves registration unconfigured.', true);
        o.validate = function(section, v) {
            return !v || (v.length % 2 === 0 && /^(?:[0-9A-Fa-f]{2}){1,36}$/.test(v)) || _('Expected 1–36 bytes in hexadecimal');
        };
        text('logical_onu_id', 'Logical ONU ID', 24, 'Logical ONU ID reported in ONU-G. Independent of the PON serial.', true);
        text('logical_password', 'Logical Password', 12, 'Logical password reported in ONU-G. Independent of the 36-byte registration ID.', true);
        o = text('wan_mac', 'WAN MAC override', 17, 'Empty uses this Q1000K’s factory WAN MAC.');
        o.validate = function(section, v) {
            return !v || (/^[0-9A-Fa-f][02468aAcCeE](:[0-9A-Fa-f]{2}){5}$/.test(v) && v !== '00:00:00:00:00:00') ||
                _('Expected a nonzero unicast MAC address');
        };
        o = s.taboption('compat', form.ListValue, 'mib_profile', _('MIB profile'),
            _('Native single PPTP Ethernet UNI. This supplies the role of the 8311 prx300_1U.ini profile; foreign platform MIB files are not loaded.'));
        o.value('native-pptp', _('Q1000K native PPTP UNI')); o.default = 'native-pptp'; o.rmempty = false;
        o = s.taboption('compat', form.Flag, 'fix_vlans', _('Fix VLANs (untagged Internet)'),
            _('Allow an untagged subscriber WAN when the OLT presents Internet on priority-tagged VLAN 0; strip the priority tag downstream. Optical VLANs and GEMs still come from the OLT. TV/voice VLAN remapping is not included.'));
        o.default = '0'; o.rmempty = false;
        text('omci_version', 'Legacy combined OMCI version', 14,
            'Legacy fallback for hardware and both software versions. Separate version fields above take precedence. Leave empty for new configurations.');
        return m.render();
    }
});
