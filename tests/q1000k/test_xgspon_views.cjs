// Execute the production LuCI views with RPC fixtures and a minimal DOM.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const base = path.resolve(__dirname, '../../package/luci-app-econet-xpon/htdocs/luci-static/resources/view/econet-xpon');
function harness(file) {
    const polls = [], options = {};
    const state = { result: null, fail: false, calls: [], saveFails: false };
    const context = {
        _: x => x,
        E: (tag, attrs, children) => ({ tag, attrs, children }),
        view: { extend: x => x },
        rpc: { declare: spec => (...args) => {
            state.calls.push([spec.object, spec.method, ...args]);
            return state.fail ? Promise.reject(new Error('offline')) : Promise.resolve(state.result);
        } },
        ui: { changes: { init: () => { state.calls.push(['refresh-changes']); return Promise.resolve(); } } },
        poll: { add: fn => polls.push(fn) },
        dom: { content: (node, children) => { node.children = children; } },
        uci: { load: () => Promise.resolve() },
        form: { NamedSection: {}, Value: {}, Flag: {}, ListValue: {}, Map: class {
            section() { return { tab() {}, option(type, name) { return options[name] = { value() {}, depends() {} }; },
                taboption(tab, type, name) { return this.option(type, name); } }; }
            render() { return {}; }
            save() {
                state.calls.push(['save-form']);
                return state.saveFails ? Promise.reject(new Error('invalid')) : Promise.resolve();
            }
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
    const settings = harness('settings.js');
    settings.view.render();
    for (const action of ['handleSave', 'handleSaveApply']) {
        settings.state.calls = [];
        await settings.view[action]();
        assert.deepEqual(settings.state.calls, [
            ['save-form'], ['uci', 'commit', 'xgspon'], ['refresh-changes']
        ]);
    }
    settings.state.saveFails = true;
    settings.state.calls = [];
    await assert.rejects(settings.view.handleSave(), /invalid/);
    assert.deepEqual(settings.state.calls, [['save-form']]);
    settings.state.saveFails = false;
    settings.state.fail = true;
    settings.state.calls = [];
    await assert.rejects(settings.view.handleSave(), /offline/);
    assert.deepEqual(settings.state.calls, [['save-form'], ['uci', 'commit', 'xgspon']]);
    const sample = { schema_version: 1, model: 'Quantum Fiber Q1000K',
        activation_supported: false, los: null, registration: null, omci: null,
        factory: { available: false }, firmware: {}, modules: { phy_loaded: true, mac_loaded: true } };
    const h = harness('status.js');
    h.state.result = sample;
    const tree = h.view.render(await h.view.load());
    assert.match(text(tree), /Unavailable/);
    assert.match(text(tree), /Not installed/);
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
    h.state.result = { ...sample, optical: { readings: {
        temperature: { value: -12500, unit: 'mC' }, supply: { value: 3300100, unit: 'uV' },
        bias: { value: 0, unit: 'uA' }, tx_power: { value: 4700000, unit: 'nW' },
        rx_power: { value: 19900, unit: 'nW' }
    } } };
    await h.polls[0]();
    assert.match(text(tree), /-12.50 °C/);
    assert.match(text(tree), /3.300 V/);
    assert.match(text(tree), /0.000 mA/);
    assert.match(text(tree), /6.72 dBm/);
    assert.doesNotMatch(text(tree), /Low alarm|Low warning|High warning|High alarm|Thresholds/);
    function tags(node) {
        if (!node || typeof node !== 'object') return [];
        return Array.isArray(node) ? node.flatMap(tags) : [node.tag, ...tags(node.children)];
    }
    assert.ok(!tags(tree).includes('details'));
    assert.match(text(tree), /Module information.*PON, OMCI and firmware details/);
    assert.doesNotMatch(text(tree), /Clear|Active/);
    h.state.result = { ...sample, activation_supported: true, ram_bench: true,
        supervisor: { last_stage: 'waiting_calibration' } };
    await h.polls[0]();
    assert.match(text(tree), /Waiting for this unit’s optical calibration/);
    assert.match(text(tree), /This RAM image disables NAND access/);
    assert.match(text(tree), /Installed/);
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
    for (const field of ['vendor_id', 'equipment_id', 'hardware_version', 'software_version_a',
        'software_version_b', 'registration_id', 'logical_onu_id', 'logical_password', 'active_bank',
        'committed_bank', 'sync_circuit_pack', 'omcc_version', 'pon_slot', 'olt_profile',
        'iphost_mac', 'iphost_hostname', 'iphost_domain', 'monitor', 'enabled', 'mode', 'client_mac']) assert.ok(s.options[field], field);
    assert.equal(s.options.mode.default, 'router');
    assert.equal(s.options.client_mac.validate('passthrough', '02:11:22:33:44:55'), true);
    for (const v of ['', '01:11:22:33:44:55', '00:00:00:00:00:00', '02:11:22:33:44:55\n'])
        assert.notEqual(s.options.client_mac.validate('passthrough', v), true);
    assert.equal(s.options._iop_mask, undefined);
    assert.equal(s.options.mib_profile, undefined);
    assert.doesNotMatch(fs.readFileSync(path.join(base, 'settings.js'), 'utf8'), /Q1000K|AT&T|BGW320|prx300|WAS-110|MaxLinear/);
    const menu = JSON.parse(fs.readFileSync(path.resolve(base, '../../../../../root/usr/share/luci/menu.d/luci-app-econet-xpon.json')));
    assert.equal(menu['admin/network/econet-xpon/configuration'].title, 'Settings');
    for (const name of ['settings', 'status']) {
        const route = menu['admin/network/econet-xpon/' + (name === 'settings' ? 'configuration' : name)];
        const version = name === 'settings' ? 'v5' : 'v4';
        assert.equal(route.action.path, 'econet-xpon/' + name + '-' + version);
        assert.equal(fs.readFileSync(path.join(base, name + '-' + version + '.js'), 'utf8'), fs.readFileSync(path.join(base, name + '.js'), 'utf8'));
    }
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
