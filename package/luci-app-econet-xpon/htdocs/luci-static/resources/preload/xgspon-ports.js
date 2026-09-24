'use strict';
'require baseclass';
'require dom';
'require network';
'require rpc';
'require view.status.include.29_ports as ports';

var callStatus = rpc.declare({ object: 'econet-xpon', method: 'status', expect: {} });

function opticalLink(device, status) {
    if (!device.isUp())
        return { up: false, label: _('Disabled'), detail: _('The PON network device is administratively down.') };
    if (!status || status.schema_version !== 1 || (status.controller || {}).available !== true)
        return { up: false, label: _('Unknown'), detail: _('Optical status unavailable.') };

    var stage = (status.supervisor || {}).last_stage;
    if (stage === 'fault' || stage === 'cleanup_failed' || status.controller.last_error)
        return { up: false, label: _('Fault'), detail: _('Check XGS-PON Status for the controller or service error.') };
    if (status.los === true)
        return { up: false, label: _('No signal'), detail: _('Optical loss of signal.') };
    if (status.los === false && status.registration === 5)
        return { up: true, label: status.mode === 'XGS-PON' ? _('10 Gbit/s') : _('Registered'),
            detail: status.mode === 'XGS-PON'
                ? _('Registered (O5). XGS-PON nominal line rate; Internet provisioning is separate.')
                : _('Registered (O5). Internet provisioning is separate.') };
    if (Number.isInteger(status.registration) && status.registration >= 1 && status.registration <= 9)
        return { up: false, label: 'O' + status.registration,
            detail: _('ONU registration state. An operational optical link requires O5 and no loss of signal.') };
    return { up: false, label: status.los === false ? _('Signal detected') : _('Unknown'),
        detail: _('Optical registration is not confirmed.') };
}

return baseclass.extend({
    __init__: function() {
        var load = ports.load, render = ports.render;

        // Extend the existing overview card without changing board/network defaults
        // or replacing the luci-mod-status package's files.
        ports.load = function() {
            return Promise.all([
                load.apply(this, arguments),
                network.getDevice('pon'),
                L.resolveDefault(callStatus(), null)
            ]).then(function(result) {
                var data = result[0], device = result[1];
                if (!device || !(device._devstate('idx') > 0))
                    return data;

                var known = Array.isArray(data[0]) ? data[0].slice() : [];
                if (!known.length) {
                    var board = JSON.parse(data[1] || '{}');
                    ['lan', 'wan'].forEach(function(role) {
                        var entry = (board.network || {})[role] || {};
                        var names = Array.isArray(entry.ports) ? entry.ports : [entry.device];
                        names.forEach(function(name) {
                            if (typeof name === 'string' && name)
                                known.push({ role: role, device: name });
                        });
                    });
                }
                if (!known.some(function(port) { return port.device === 'pon'; }))
                    known.push({ role: 'wan', device: 'pon' });
                data[0] = known;
                data.xgspon = { device: device, status: result[2] };
                return data;
            });
        };

        ports.render = function(data) {
            var content = render.call(this, data);
            if (!content || !data.xgspon)
                return content;

            var link = opticalLink(data.xgspon.device, data.xgspon.status);
            Array.from(content.children).forEach(function(card) {
                var heading = card.querySelector('.ifacebox-head');
                if (!heading || heading.textContent !== 'pon')
                    return;

                // Kernel carrier can reflect only an open CPU data interface.
                // Keep the original pon counters and network/zone tooltips.
                dom.content(card.querySelector('.ifacebox-body'), [
                    E('img', { 'src': L.resource('icons/port_' + (link.up ? 'up' : 'down') + '.svg'),
                        'title': link.detail }),
                    E('br'), E('span', { 'title': link.detail }, link.label)
                ]);
                card.querySelectorAll('.zonebadge').forEach(function(badge) {
                    badge.style.opacity = link.up ? 1 : 0.25;
                });
            });
            return content;
        };
    }
});
