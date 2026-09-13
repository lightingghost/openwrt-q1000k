'use strict';
'require view';
'require rpc';
'require poll';
'require dom';

var callRaw = rpc.declare({ object: 'econet-xpon', method: 'raw', expect: {} });
function dump(d) {
    return d && d.schema_version === 1 ? JSON.stringify(d, null, 2) :
        _('Diagnostics unavailable. The last sample could not be refreshed.');
}
return view.extend({
    load: function() { return callRaw().catch(function() { return null; }); },
    render: function(data) {
        var pre = E('pre', { 'style': 'white-space:pre-wrap' }, dump(data));
        poll.add(function() {
            return callRaw().then(function(d) { dom.content(pre, dump(d)); })
                .catch(function() { dom.content(pre, dump(null)); });
        }, 5);
        return E('div', { 'class': 'cbi-map' }, [
            E('h2', {}, _('XGS-PON diagnostics')),
            E('p', {}, _('Read-only diagnostic snapshot. OMCI managed-entity data is not available yet.')),
            pre
        ]);
    },
    handleSave: null, handleSaveApply: null, handleReset: null
});
