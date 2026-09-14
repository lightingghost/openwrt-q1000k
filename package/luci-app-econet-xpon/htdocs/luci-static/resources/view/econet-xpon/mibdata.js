'use strict';
'require view';
'require rpc';
'require poll';
'require dom';

var callRaw = rpc.declare({ object: 'econet-xpon', method: 'raw', expect: {} });
var callMib = rpc.declare({ object: 'econet-xpon', method: 'mib', expect: {} });
function dump(d) {
    return d && d.schema_version === 1 ? JSON.stringify(d, null, 2) :
        _('Diagnostics unavailable. The last sample could not be refreshed.');
}
return view.extend({
    load: function() { return callRaw().catch(function() { return null; }); },
    render: function(data) {
        var pre = E('pre', { 'style': 'white-space:pre-wrap' }, dump(data));
        var mib = E('pre', { 'style': 'white-space:pre-wrap' }, _('MIB has not been read.'));
        var button = E('button', {
            'class': 'cbi-button cbi-button-action',
            'click': function() {
                button.disabled = true;
                dom.content(mib, _('Reading MIB…'));
                return callMib().then(function(d) {
                    dom.content(mib, d && d.schema_version === 1 && d.available === true && Array.isArray(d.entities)
                        ? JSON.stringify(d.entities, null, 2) : _('MIB unavailable.'));
                }).catch(function() { dom.content(mib, _('MIB unavailable.')); })
                    .finally(function() { button.disabled = false; });
            }
        }, _('Read OMCI MIB'));
        poll.add(function() {
            return callRaw().then(function(d) { dom.content(pre, dump(d)); })
                .catch(function() { dom.content(pre, dump(null)); });
        }, 5);
        return E('div', { 'class': 'cbi-map' }, [
            E('h2', {}, _('XGS-PON diagnostics')),
            E('p', {}, _('Read-only diagnostics. The MIB is read on demand from the kernel core. It is a live view, not an atomic backup.')),
            pre, button, mib
        ]);
    },
    handleSave: null, handleSaveApply: null, handleReset: null
});
