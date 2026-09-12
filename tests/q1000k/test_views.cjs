// Render and poll both real LuCI views with the Q1000K hardware contract.
const fs = require('fs');
const path = require('path');
const assert = require('assert/strict');
const repo = path.resolve(__dirname, '../..');
async function test(app) {
  const ids = new Map(), callbacks = [], calls = [];
  class Element {
    constructor(tag, attrs = {}, children = []) {
      this.tag = tag; this.attrs = attrs; this.style = {}; this.children = [];
      Object.assign(this, attrs); this.style = {};
      if (this.id) ids.set(this.id, this);
      for (const child of (Array.isArray(children) ? children : [children])) if (child != null) this.appendChild(child);
    }
    appendChild(child) { this.children.push(child); return child; }
    set innerHTML(v) { this.html = v; this.children = []; }
    get innerHTML() { return this.html || ''; }
    get rows() { return this.children.filter(x => x.tag === 'tr'); }
    deleteRow(i) { this.children.splice(i, 1); }
    matches() { return false; }
    querySelectorAll() { return []; }
    querySelector() { return null; }
    replaceWith(fresh) { if (fresh.id) ids.set(fresh.id, fresh); }
    setAttribute(key, value) { this[key] = value; }
  }
  const E = (tag, attrs, children) => new Element(tag, attrs, children);
  const document = { head: E('head'), body: E('body'), documentElement: E('html'),
    getElementById: id => ids.get(id) || null, querySelector: () => null, querySelectorAll: () => [],
    createElement: tag => E(tag) };
  const window = { getComputedStyle: () => ({backgroundColor:'rgb(255, 255, 255)', getPropertyValue: () => ''}), matchMedia: () => ({matches:false}) };
  const responses = {
    getStatus: { npu_loaded:true, npu_version:'TLB2.3', offload_bound:1, offload_total:2, cpu_count:4,
      cpu_hw_freq:1200000, cpu_min_freq:500000, cpu_max_freq:1200000,
      cpu_avail_freqs:'500000 1200000', cpu_governor:'schedutil', cpu_avail_governors:'performance schedutil' },
    getTemperatures: {sensors:[{name:'CPU', millidegrees:53250}]},
    getPpeEntries: {entries:[], bnd:{total:1,entries:[]},unb:{total:1,entries:[]}},
    getFrameEngine: {available:false,error:'unavailable'}, getWifiStats:{available:false,bands:[]},
    getDeviceMode: {mode:'ap',reason:'no_wan'}, getWanHealth:{available:false},
    getJitterResult:{available:false}, getVlanOffload:{enabled:0,available:true},
    getPPPoEOffload:{enabled:0,available:true},getPppoeOffload:{enabled:0,available:true},
    getEthStats:{ports:[{iface:'lan1',up:true,speed:1000,tx_bytes:100,rx_bytes:200},{iface:'lan2',up:false,speed:0,tx_bytes:0,rx_bytes:0}]}
  };
  const rpc = {declare: spec => (...args) => { calls.push([spec.method, args]); return Promise.resolve(responses[spec.method] || {}); }};
  const source = fs.readFileSync(path.join(repo, `package/luci-app-airoha-${app}/htdocs/luci-static/resources/view/airoha_${app}/status.js`), 'utf8');
  const view = new Function('view','rpc','poll','ui','E','L','_','document','window','getComputedStyle', source)(
    {extend: v => v}, rpc, {add: cb => callbacks.push(cb)}, {addNotification:()=>{}}, E,
    {bind:(fn,obj)=>fn.bind(obj)}, s=>s, document, window, window.getComputedStyle);
  const tree = view.render(await view.load());
  const text = node => typeof node === 'object' ? (node.textContent || '') + node.innerHTML + node.children.map(text).join(' ') : String(node);
  if (app === 'npu') {
    assert(text(tree).includes('53.3 °C'));
    assert(ids.has('vlan-offload-select') && ids.has('pppoe-offload-select'));
    assert(!text(tree).includes('Overclock'));
    responses.getTemperatures = {sensors:[{name:'CPU',millidegrees:60000}]};
  } else {
    assert(!ids.has('wifi-svg-wrap-0'));
    assert(ids.has('eth-port-svg-lan1') && ids.has('eth-port-svg-lan2'));
    assert(!ids.has('eth-port-svg-wan') && !ids.has('eth-port-svg-lan3'));
    assert(text(tree).includes('Raw hardware counters are unavailable'));
  }
  for (const cb of callbacks) await cb();
  if (app === 'npu') assert(text(ids.get('temperature-sensors')).includes('60.0 °C'));
  for (const cb of callbacks) await cb();
  assert(!calls.some(([method])=>method==='setOverclock'));
  console.log(`${app}: render and two polls passed`);
}
(async()=>{await test('npu');await test('flowsense');})().catch(e=>{console.error(e);process.exit(1);});
