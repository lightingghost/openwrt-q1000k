'use strict';
'require view';
'require rpc';
'require poll';
'require dom';

var callStatus = rpc.declare({ object: 'econet-xpon', method: 'status', expect: {} });

function value(v) {
    return v == null || v === '' || typeof v === 'object' ? _('Unavailable') : String(v);
}
function state(v, yes, no) {
    return typeof v !== 'boolean' ? _('Unavailable') : (v ? yes : no);
}
function flag(v, yes, no) {
    return state(v === 0 ? false : v === 1 ? true : null, yes, no);
}
function power(nw) {
    return Number.isInteger(nw) && nw > 0 ?
        (10 * Math.log10(nw / 1000000)).toFixed(2) + ' dBm (' + nw + ' nW)' : _('Unavailable');
}
function rows(d) {
    var f = d.factory || {}, i = d.identity || {}, fw = d.firmware || {}, m = d.modules || {}, c = d.controller || {}, o = d.omci || {};
    return [
        [ _('Device'), value(d.model) ],
        [ _('SoC'), value(d.soc) ],
        [ _('Optical controllers'), value(d.optics) ],
        [ _('Mode'), value(d.mode) ],
        [ _('PON startup service'), state(d.activation_supported, _('Installed'), _('Not installed')) ],
        [ _('Controller driver'), state(c.available, _('Available'), _('Not loaded')) ],
        [ _('GPON detection (last check)'), state(c.gpon_detected, _('Detected'), _('Not detected')) ],
        [ _('XGS-PON detection (last check)'), state(c.xgspon_detected, _('Detected'), _('Not detected')) ],
        [ _('Controller power mode'), value(c.mode) ],
        [ _('Controller initialization'), value(c.stage) ],
        [ _('Controller error'), value(c.last_error) ],
        [ _('Optical MCU'), state(c.md32_enabled, _('Enabled'), _('Disabled')) ],
        [ _('Optical transmission'), state(c.tx_disabled, _('Disabled'), _('Enabled')) ],
        [ _('Controller memory verification'), state(c.firmware_verified, _('Verified'), _('Not initialized')) ],
        [ _('Unit optical calibration'), state((d.calibration || {}).available, _('Available'), _('Not staged or unavailable')) ],
        [ _('Calibration source'), value((d.calibration || {}).source) ],
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
        [ _('OMCI transmitted packets'), value(o.tx_packets) ]
    ];
}
function dataTable(data, headings) {
    var body = data.map(function(r) {
        return E('tr', { 'class': 'tr' }, r.map(function(cell) {
            return E('td', { 'class': 'td left' }, cell);
        }));
    });
    if (headings) body.unshift(E('tr', { 'class': 'tr table-titles' }, headings.map(function(h) {
        return E('th', { 'class': 'th left' }, h);
    })));
    return E('div', { 'style': 'overflow-x:auto' }, E('table', { 'class': 'table' }, body));
}
var measurements = [
    ['temperature', _('Temperature'), 'mC', 1000, '°C', 'temperature_mc'],
    ['supply', _('Supply voltage (Vcc)'), 'uV', 1000000, 'V', 'voltage_uv'],
    ['bias', _('TX laser bias'), 'uA', 1000, 'mA', 'bias_ua'],
    ['tx_power', _('TX optical power'), 'nW', 1, 'nW', 'tx_power_nw'],
    ['rx_power', _('RX optical power'), 'nW', 1, 'nW', 'rx_power_nw']
];
function reading(v, metric) {
    if (!Number.isInteger(v) || (metric[0] !== 'temperature' && v < 0)) return _('Unavailable');
    if (metric[2] === 'nW') return v === 0 ? '0 nW (' + _('below reporting range') + ')' : power(v);
    return (v / metric[3]).toFixed(metric[0] === 'temperature' ? 2 : 3) + ' ' + metric[4];
}
function diagnosticTable(d) {
    var readings = (d.optical || {}).readings || {}, omci = d.omci || {};
    return dataTable(measurements.map(function(metric) {
        var sample = readings[metric[0]], current;
        if (sample && sample.unit === metric[2]) current = sample.value;
        else if (!sample) current = omci[metric[5]];
        return [metric[1], reading(current, metric)];
    }), [_('Measurement'), _('Current')]);
}
function moduleDetails(d) {
    var data = [
        [_('Optical hardware'), value(d.optics)],
        [_('TX disable state'), state((d.controller || {}).tx_disabled, _('Disabled'), _('Enabled'))],
        [_('RX LOS state'), state(d.los, _('Signal lost'), _('Signal detected'))]
    ];
    return E('div', {}, [E('h3', {}, _('Module information')),
        E('p', {}, _('Optical hardware and signal state reported by the driver.')),
        dataTable(data)]);
}
function table(d, failed) {
    if (failed || !d || d.schema_version !== 1)
        return E('p', { 'class': 'alert-message warning' },
            _('Status unavailable. The last sample could not be refreshed.'));
    var supervisor = d.supervisor || {}, c = d.controller || {}, fw = d.firmware || {};
    var stages = {
        waiting_firmware: _('Waiting for the verified OEM optical firmware pair.'),
        waiting_calibration: _('Waiting for this unit’s optical calibration.'),
        initializing: _('Initializing the optical controller.'),
        monitoring: _('Optical diagnostics active; transmission is disabled.'),
        running: _('PON stack is running. Check registration and service status below.'),
        waiting_registration: _('PON service is waiting for fiber, registration or provider provisioning. LAN networking remains available.'),
        waiting_provisioning: _('Confirming stable provider provisioning before starting WAN networking.'),
        waiting_network: _('PON is provisioned; retrying WAN network startup.'),
        stopped: _('PON startup service is stopped.'),
        fault: _('PON startup stopped after a hardware error.'),
        cleanup_failed: _('PON shutdown is incomplete. Check the system log.')
    };
    var notice = stages[supervisor.last_stage];
    if (!notice) {
        if (!c.available) notice = _('The optical controller is not loaded. Automatic diagnostics start at boot when enabled in Settings.');
        else if (!fw.program_verified || !fw.data_verified) notice = stages.waiting_firmware;
        else if (!(d.calibration || {}).available) notice = stages.waiting_calibration;
    }
    return E('div', {}, [
        notice ? E('p', { 'class': 'alert-message notice' }, notice) : '',
        d.ram_bench && !(d.factory || {}).available ? E('p', {},
            _('This RAM image disables NAND access. Factory identity is unavailable; configured identity overrides and embedded or staged unit calibration can still be used.')) : '',
        E('h3', {}, _('Fiber status')),
        dataTable([
            [_('Optical WAN link (kernel carrier)'), state((d.link || {}).carrier, _('Up'), _('Down'))],
            [_('Link state (ONU registration)'), Number.isInteger(d.registration) ? 'O' + d.registration : _('Unavailable')],
            [_('Sample uptime'), Number.isInteger(d.sampled_uptime) ? d.sampled_uptime + ' s' : _('Unavailable')],
            [_('Supervisor last stage'), value(supervisor.last_stage)]
        ]),
        E('h3', {}, _('Optical diagnostics')),
        E('p', {}, _('Current readings reported by the optical controller.')),
        diagnosticTable(d),
        moduleDetails(d),
        E('div', {}, [E('h3', {}, _('PON, OMCI and firmware details')), dataTable(rows(d))])
    ]);
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
            E('h2', {}, _('XGS-PON Status')),
            E('p', { 'class': 'cbi-map-descr' },
                _('Optical link, module and OMCI status. Readings refresh every five seconds.')),
            content
        ]);
    },
    handleSave: null, handleSaveApply: null, handleReset: null
});
