'use strict';
'require view';
'require form';
'require uci';

return view.extend({
    load: function() { return uci.load('q1000k-xgspon'); },
    render: function() {
        var m = new form.Map('q1000k-xgspon', _('XGS-PON identity'),
            _('Empty overrides use this unit’s preserved factory identity. Optical service is not available in this development build; saving these settings does not activate the optical port.'));
        var s = m.section(form.NamedSection, 'identity', 'identity', _('Identity overrides'));
        s.anonymous = true;
        s.addremove = false;
        var o = s.option(form.Value, 'serial', _('PON serial override'),
            _('Four vendor letters or digits followed by eight hexadecimal digits. Leave empty to use the factory FSAN.'));
        o.rmempty = true;
        o.validate = function(section, v) {
            return !v || /^[A-Za-z0-9]{4}[0-9A-Fa-f]{8}$/.test(v) ||
                _('Expected four vendor letters or digits and eight hexadecimal digits');
        };
        o = s.option(form.Value, 'wan_mac', _('WAN MAC override'),
            _('Leave empty to use the factory WAN MAC.'));
        o.rmempty = true;
        o.validate = function(section, v) {
            return !v || (/^[0-9A-Fa-f][02468aAcCeE](:[0-9A-Fa-f]{2}){5}$/.test(v) && v !== '00:00:00:00:00:00') ||
                _('Expected a nonzero unicast MAC address');
        };
        return m.render();
    }
});
