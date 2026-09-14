'use strict';
'require view';
'require rpc';
'require poll';
'require dom';

var callStatus = rpc.declare({ object: 'econet-xpon', method: 'status', expect: {} });

function value(v) {
    return v == null || v === '' ? _('Unavailable') : String(v);
}
function state(v, yes, no) {
    return typeof v !== 'boolean' ? _('Unavailable') : (v ? yes : no);
}
function flag(v, yes, no) {
    return state(v === 0 ? false : v === 1 ? true : null, yes, no);
}
function rows(d) {
    var f = d.factory || {}, i = d.identity || {}, fw = d.firmware || {}, m = d.modules || {}, c = d.controller || {}, o = d.omci || {};
    return [
        [ _('Device'), value(d.model) ],
        [ _('SoC'), value(d.soc) ],
        [ _('Optical controllers'), value(d.optics) ],
        [ _('Mode'), value(d.mode) ],
        [ _('Optical service'), state(d.activation_supported, _('Supported'), _('Not available in this build')) ],
        [ _('Controller driver'), state(c.available, _('Available'), _('Not loaded')) ],
        [ _('GPON detection (last check)'), state(c.gpon_detected, _('Detected'), _('Not detected')) ],
        [ _('XGS-PON detection (last check)'), state(c.xgspon_detected, _('Detected'), _('Not detected')) ],
        [ _('Controller power mode'), value(c.mode) ],
        [ _('Controller initialization'), value(c.stage) ],
        [ _('Controller error'), value(c.last_error) ],
        [ _('Optical MCU'), state(c.md32_enabled, _('Enabled'), _('Disabled')) ],
        [ _('Optical transmission'), state(c.tx_disabled, _('Disabled'), _('Enabled')) ],
        [ _('Controller memory verification'), state(c.firmware_verified, _('Verified'), _('Not initialized')) ],
        [ _('Factory identity and calibration'), state(f.available, _('Available'), _('Unavailable or invalid')) ],
        [ _('Factory serial'), value(f.serial) ],
        [ _('Factory WAN MAC'), value(f.wan_mac) ],
        [ _('Selected serial'), value(i.serial) ],
        [ _('Selected WAN MAC'), value(i.wan_mac) ],
        [ _('Selected identity'), state(i.valid, _('Valid'), _('Incomplete or invalid')) ],
        [ _('OEM program firmware'), state(fw.program_verified, _('Verified'), _('Missing or different version')) ],
        [ _('OEM data firmware'), state(fw.data_verified, _('Verified'), _('Missing or different version')) ],
        [ _('PON PHY module'), state(m.phy_loaded, _('Loaded'), _('Not loaded')) ],
        [ _('PON MAC module'), state(m.mac_loaded, _('Loaded'), _('Not loaded')) ],
        [ _('Loss of signal'), state(d.los, _('Signal lost'), _('Signal detected')) ],
        [ _('ONU activation state'), Number.isInteger(d.registration) ? 'O' + d.registration : value(null) ],
        [ _('OMCI authenticated session'), flag(o.authenticated, _('Admitted'), _('Closed')) ],
        [ _('OMCI agent'), flag(o.agent_enabled, _('Enabled'), _('Disabled')) ],
        [ _('OMCI exchanges'), flag(o.agent_operational, _('Observed'), _('Not observed')) ],
        [ _('OMCI MIB objects'), value(o.mib_objects) ],
        [ _('Configured service rules (may be dormant)'), value(o.service_rules) ],
        [ _('Service reconciliation error'), value(o.service_error) ],
        [ _('OMCI received packets'), value(o.rx_packets) ],
        [ _('OMCI transmitted packets'), value(o.tx_packets) ],
        [ _('Provisioned service'), state(d.service_ready, _('Ready'), _('Not ready')) ],
        [ _('Optical measurements'), _('Unavailable') ]
    ];
}
function table(d, failed) {
    if (failed || !d || d.schema_version !== 1)
        return E('p', { 'class': 'alert-message warning' },
            _('Status unavailable. The last sample could not be refreshed.'));
    return E('table', { 'class': 'table' }, rows(d).map(function(r) {
        return E('tr', { 'class': 'tr' }, [
            E('td', { 'class': 'td left', 'width': '40%' }, r[0]),
            E('td', { 'class': 'td left' }, r[1])
        ]);
    }));
}
return view.extend({
    load: function() { return callStatus().catch(function() { return null; }); },
    render: function(data) {
        var content = E('div', {}, table(data));
        poll.add(function() {
            return callStatus().then(function(d) {
                dom.content(content, table(d));
            }).catch(function() { dom.content(content, table(null, true)); });
        }, 5);
        return E('div', { 'class': 'cbi-map' }, [
            E('h2', {}, _('Q1000K XGS-PON')),
            E('p', { 'class': 'cbi-map-descr' },
                _('Development diagnostics. An admitted OMCI session and configured rules do not prove Internet service. Missing readings are shown as Unavailable.')),
            content
        ]);
    },
    handleSave: null, handleSaveApply: null, handleReset: null
});
