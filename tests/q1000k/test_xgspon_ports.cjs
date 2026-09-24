// Exercise the installed LuCI port renderer with the PON preload extension.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../..');

class Element {
    constructor(tag, attrs, children) {
        this.tag = tag;
        this.attrs = attrs || {};
        this.style = {};
        this.nodes = [children].flat(Infinity).filter(v => v != null);
    }
    get children() { return this.nodes.filter(v => v instanceof Element); }
    get textContent() { return this.nodes.map(v => v instanceof Element ? v.textContent : String(v)).join(''); }
    set innerText(v) { this.nodes = [v]; }
    appendChild(v) { this.nodes.push(v); return v; }
    querySelectorAll(selector) {
        return this.children.flatMap(node => [
            ...((node.attrs.class || '').split(' ').includes(selector.slice(1)) ? [node] : []),
            ...node.querySelectorAll(selector)
        ]);
    }
    querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
}
function E(tag, attrs, children) {
    if (Array.isArray(attrs) || typeof attrs === 'string') [children, attrs] = [attrs, {}];
    return new Element(tag, attrs, children);
}
function device(name, tx, rx) {
    const state = { idx: 10, flags: { up: true }, link: { carrier: true, speed: 10000, duplex: 'full' },
        stats: { tx_bytes: tx, rx_bytes: rx, tx_packets: 8, rx_packets: 9, multicast: 0,
            tx_errors: 0, rx_errors: 0, tx_dropped: 0, rx_dropped: 0, collisions: 0 } };
    return { state, _devstate: key => state[key], getName: () => name,
        isUp: () => state.flags.up, getSpeed: () => state.link.speed,
        getDuplex: () => state.link.duplex, getCarrier: () => state.link.carrier,
        getTXBytes: () => state.stats.tx_bytes, getRXBytes: () => state.stats.rx_bytes,
        getI18n: () => name, getType: () => 'ethernet' };
}

async function main() {
    const devices = { lan1: device('lan1', 101, 102), lan2: device('lan2', 201, 202),
        pon: device('pon', 12345, 23456), ponraw: device('ponraw', 999999, 888888) };
    const state = { builtin: ['lan1', 'lan2'].map(device => ({ device, role: 'lan' })),
        board: { network: { lan: { ports: ['lan1', 'lan2'] } } },
        status: { schema_version: 1, mode: 'XGS-PON', controller: { available: true, last_error: 0 },
            los: false, registration: 5 }, fail: false, swconfig: false };
    const wan = { getName: () => 'wan', getDevice: () => devices.pon };
    const zone = { getNetworks: () => ['wan'], getName: () => 'wan' };
    const rpcCalls = [];
    const context = vm.createContext({ E, _: x => x, N_: (n, one, many) => n === 1 ? one : many,
        baseclass: { extend: value => value },
        dom: { content: (node, values) => { node.nodes = values; } },
        fs: { read: () => Promise.resolve(JSON.stringify(state.board)) },
        rpc: { declare: spec => (...args) => {
            rpcCalls.push([spec.object, spec.method, ...args]);
            if (spec.method === 'getBuiltinEthernetPorts') return Promise.resolve(state.builtin);
            if (spec.object === 'network.device') return Promise.resolve({});
            assert.equal(spec.object, 'econet-xpon');
            assert.equal(spec.method, 'status');
            return state.fail ? Promise.reject(new Error('offline')) : Promise.resolve(state.status);
        } },
        network: { getNetworks: () => Promise.resolve([wan]),
            getDevice: name => Promise.resolve(devices[name] || null),
            instantiateDevice: name => { assert.ok(devices[name], name); return devices[name]; } },
        firewall: { getZones: () => Promise.resolve([zone]), getZoneColorStyle: () => 'background:red' },
        uci: { load: () => Promise.resolve(), sections: () => [] },
        ui: { itemlist: (node, items) => { node.nodes = items; return node; } },
        L: { resolveDefault: (promise, fallback) => promise.catch(() => fallback),
            hasSystemFeature: () => state.swconfig, isObject: v => v && typeof v === 'object',
            naturalCompare: (a, b) => a.localeCompare(b), toArray: v => v == null ? [] : [].concat(v),
            resource: value => '/luci-static/resources/' + value }
    });
    vm.runInContext(`String.prototype.format = function(...args) {
        let i = 0;
        return this.replace(/%(?:[0-9.]+)?(?:m[A-Za-z.]+|[dfsqu])/g, () => String(args[i++]));
    };`, context);
    function load(file) {
        return vm.runInContext('(function(){' + fs.readFileSync(path.join(root, file), 'utf8') + '\n})()', context);
    }
    const ports = context.ports = load('feeds/luci/modules/luci-mod-status/htdocs/luci-static/resources/view/status/include/29_ports.js');
    const plugin = load('package/luci-app-econet-xpon/htdocs/luci-static/resources/preload/xgspon-ports.js');
    plugin.__init__();
    const update = async () => ports.render(await ports.load());
    const card = (tree, name) => tree.children.find(c => c.querySelector('.ifacebox-head').textContent === name);
    const link = (tree, name = 'pon') => card(tree, name).querySelector('.ifacebox-body');
    let tree = await update();
    assert.deepEqual(tree.children.map(c => c.querySelector('.ifacebox-head').textContent), ['lan1', 'lan2', 'pon']);
    assert.equal(link(tree).textContent, '10 Gbit/s');
    assert.match(link(tree).children[0].attrs.src, /port_up\.svg$/);
    assert.match(card(tree, 'pon').textContent, /12345.*23456/);
    assert.doesNotMatch(card(tree, 'pon').textContent, /999999|888888/);
    assert.match(card(tree, 'pon').textContent, /Part of network:.*wan/);
    assert.equal(devices.pon.getCarrier(), true);
    assert.match(link(tree, 'lan1').textContent, /10.*GbE/);

    for (const [status, label] of [
        [{ ...state.status, registration: 1, los: true }, 'No signal'],
        [{ ...state.status, registration: 2 }, 'O2'],
        [{ ...state.status, registration: 7 }, 'O7'],
        [{ ...state.status, los: null }, 'O5'],
        [{ ...state.status, registration: null }, 'Signal detected'],
        [{ ...state.status, controller: { available: true, last_error: -110 } }, 'Fault'],
        [{ ...state.status, supervisor: { last_stage: 'fault' } }, 'Fault'],
        [{ ...state.status, controller: { available: false } }, 'Unknown'],
        [{ ...state.status, schema_version: 99 }, 'Unknown']
    ]) {
        const saved = state.status;
        state.status = status;
        tree = await update();
        assert.equal(link(tree).textContent, label);
        assert.match(link(tree).children[0].attrs.src, /port_down\.svg$/);
        assert.match(card(tree, 'pon').textContent, /12345.*23456/);
        assert.match(link(tree, 'lan1').textContent, /10.*GbE/);
        state.status = saved;
    }
    state.fail = true;
    assert.equal(link(await update()).textContent, 'Unknown');
    state.fail = false;
    assert.equal(link(await update()).textContent, '10 Gbit/s');
    state.status.mode = 'unknown';
    assert.equal(link(await update()).textContent, 'Registered');
    state.status.mode = 'XGS-PON';
    devices.pon.state.flags.up = false;
    assert.equal(link(await update()).textContent, 'Disabled');
    devices.pon.state.flags.up = true;
    devices.pon.state.stats.tx_bytes += 10;
    assert.match(card(await update(), 'pon').textContent, /12355/);

    state.builtin = [];
    state.board.network.wan = { device: 'pon' };
    tree = await update();
    assert.deepEqual(tree.children.map(c => c.querySelector('.ifacebox-head').textContent), ['lan1', 'lan2', 'pon']);
    state.builtin = ['lan1', 'lan2', 'pon'].map(device => ({ device, role: 'wan' }));
    assert.equal((await update()).children.length, 3);
    state.builtin = state.builtin.slice(0, 2);
    devices.pon.state.idx = undefined;
    assert.equal((await update()).children.length, 2);
    delete devices.pon;
    assert.equal((await update()).children.length, 2);
    devices.pon = device('pon', 12345, 23456);
    state.swconfig = true;
    assert.equal(await update(), null);
    assert.ok(rpcCalls.every(([object, method]) =>
        (object === 'luci' && method === 'getBuiltinEthernetPorts') ||
        (object === 'network.device' && method === 'status') ||
        (object === 'econet-xpon' && method === 'status')));
    console.log('PON overview ports: real upstream renderer, runtime discovery, optical states, counters, fallback, deduplication and failed refreshes passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
