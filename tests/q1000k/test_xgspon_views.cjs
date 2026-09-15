// Execute the production LuCI views with RPC fixtures and a minimal DOM.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const base = path.resolve(__dirname, '../../package/luci-app-econet-xpon/htdocs/luci-static/resources/view/econet-xpon');
function harness(file) {
    const polls = [], options = {};
    const state = { result: null, fail: false };
    const context = {
        _: x => x,
        E: (tag, attrs, children) => ({ tag, attrs, children }),
        view: { extend: x => x },
        rpc: { declare: () => () => state.fail ? Promise.reject(new Error('offline')) : Promise.resolve(state.result) },
        poll: { add: fn => polls.push(fn) },
        dom: { content: (node, children) => { node.children = children; } },
        uci: { load: () => Promise.resolve() },
        form: { NamedSection: {}, Value: {}, Flag: {}, ListValue: {}, Map: class {
            section() { return { tab() {}, taboption(tab, type, name) { return options[name] = { value() {} }; } }; }
            render() { return {}; }
        } }
    };
    const view = vm.runInNewContext('(function(){' + fs.readFileSync(path.join(base, file), 'utf8') + '\n})()', context);
    return { view, polls, state, options };
}
function text(node) {
    if (node == null) return '';
    if (Array.isArray(node)) return node.map(text).join(' ');
    if (typeof node === 'object') return text(node.children);
    return String(node);
}
async function main() {
    const sample = { schema_version: 1, model: 'Quantum Fiber Q1000K',
        activation_supported: false, los: null, registration: null, omci: null,
        factory: { available: false }, firmware: {}, modules: { phy_loaded: true, mac_loaded: true } };
    const h = harness('status.js');
    h.state.result = sample;
    const tree = h.view.render(await h.view.load());
    assert.match(text(tree), /Unavailable/);
    assert.match(text(tree), /Not available in this build/);
    assert.doesNotMatch(text(tree), /Signal detected|Signal lost/);
    h.state.result = { ...sample, controller: { available: true, mode: 'xgspon',
        gpon_detected: true, xgspon_detected: true, md32_enabled: true,
        tx_disabled: true, firmware_verified: true, stage: 'initialized', last_error: 0 } };
    await h.polls[0]();
    assert.match(text(tree), /Detected/);
    assert.match(text(tree), /Enabled/);
    assert.match(text(tree), /Disabled/);
    assert.doesNotMatch(text(tree), /Signal detected|Signal lost/);
    h.state.result = { ...sample, los: true };
    await h.polls[0]();
    assert.match(text(tree), /Signal lost/);
    h.state.result = { ...sample, los: false };
    await h.polls[0]();
    assert.match(text(tree), /Signal detected/);
    h.state.result = { ...sample, registration: 5, omci: { authenticated: 1,
        agent_enabled: 1, agent_operational: 0, mib_objects: 281, service_rules: 3,
        service_error: -22, rx_packets: '18446744073709551615' } };
    await h.polls[0]();
    assert.match(text(tree), /O5/);
    assert.match(text(tree), /Admitted/);
    assert.match(text(tree), /18446744073709551615/);
    assert.match(text(tree), /may be dormant/);
    assert.doesNotMatch(text(tree), /\[object Object\]/);
    h.state.result = { ...sample, omci: { rx_power_nw: 19900 } };
    await h.polls[0]();
    assert.match(text(tree), /-17.01 dBm \(19900 nW\)/);
    for (const invalid of [null, 0, -1, '19900', {}, false]) {
        h.state.result = { ...sample, omci: { rx_power_nw: invalid } };
        await h.polls[0]();
        assert.doesNotMatch(text(tree), /dBm/);
    }
    h.state.fail = true;
    await h.polls[0]();
    assert.match(text(tree), /last sample could not be refreshed/);
    assert.doesNotMatch(text(tree), /Signal detected/);
    h.state.fail = false;
    h.state.result = { ...sample, schema_version: 99 };
    await h.polls[0]();
    assert.match(text(tree), /Status unavailable/);

    const d = harness('mibdata.js');
    d.state.result = sample;
    const raw = d.view.render(await d.view.load());
    assert.match(text(raw), /Quantum Fiber Q1000K/);
    d.state.fail = true;
    await d.polls[0]();
    assert.doesNotMatch(text(raw), /Quantum Fiber Q1000K/);
    assert.match(text(raw), /Diagnostics unavailable/);

    const button = raw.children.find(n => n.tag === 'button');
    d.state.fail = false;
    d.state.result = { schema_version: 1, available: true, entities: [{ class_id: 277, name: '<script>test</script>' }] };
    await button.attrs.click();
    assert.equal(button.disabled, false);
    assert.match(text(raw), /class_id/);
    assert.match(text(raw), /<script>test<\/script>/); // Text content, never innerHTML.
    d.state.fail = true;
    await button.attrs.click();
    assert.match(text(raw), /MIB unavailable/);
    assert.doesNotMatch(text(raw), /class_id/);
    assert.equal(button.disabled, false);

    const s = harness('settings.js');
    s.view.render();
    for (const v of ['', 'TEST00112233', 'ABCDaabbccdd'])
        assert.equal(s.options.serial.validate('identity', v), true);
    for (const v of ['TEST0011223', 'AB C00112233', 'TEST00112233\n', 'TESTxyz12233'])
        assert.notEqual(s.options.serial.validate('identity', v), true);
    for (const v of ['', '00:11:22:33:44:55', '02:11:22:33:44:55'])
        assert.equal(s.options.wan_mac.validate('identity', v), true);
    for (const v of ['01:11:22:33:44:55', '00:00:00:00:00:00', '02:11:22:33:44:55\n'])
        assert.notEqual(s.options.wan_mac.validate('identity', v), true);
    console.log('XGS-PON views: unavailable/LOS states, failed refreshes, schema checks and identity validation passed');
}
main().catch(e => { console.error(e); process.exitCode = 1; });
