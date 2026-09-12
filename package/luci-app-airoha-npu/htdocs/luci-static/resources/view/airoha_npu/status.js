'use strict';
'require view';
'require poll';
'require rpc';
'require ui';

var callNpuStatus = rpc.declare({ object: 'luci.airoha_npu', method: 'getStatus' });
var callPpeEntries = rpc.declare({ object: 'luci.airoha_npu', method: 'getPpeEntries' });
var callTokenInfo = rpc.declare({ object: 'luci.airoha_npu', method: 'getTokenInfo' });
var callTemperatures = rpc.declare({ object: 'luci.airoha_npu', method: 'getTemperatures' });
var callGetVlanOffload = rpc.declare({ object: 'luci.airoha_npu', method: 'getVlanOffload' });
var callSetVlanOffload = rpc.declare({ object: 'luci.airoha_npu', method: 'setVlanOffload', params: ['enabled'] });
var callGetPppoeOffload = rpc.declare({ object: 'luci.airoha_npu', method: 'getPPPoEOffload' });
var callSetPppoeOffload = rpc.declare({ object: 'luci.airoha_npu', method: 'setPPPoEOffload', params: ['enabled'] });
var callFrameEngine = rpc.declare({ object: 'luci.airoha_npu', method: 'getFrameEngine' });
var callSetGovernor = rpc.declare({ object: 'luci.airoha_npu', method: 'setGovernor', params: ['governor'] });
var callSetMaxFreq = rpc.declare({ object: 'luci.airoha_npu', method: 'setMaxFreq', params: ['freq'] });

/* ── Theme-adaptive CSS ── */
var themeCSS = '\
.soc-card{background:var(--soc-card-bg);border:1px solid var(--soc-border);border-radius:8px;padding:14px;transition:border-color .3s}\
.soc-card-accent{border-left-width:3px;border-left-style:solid}\
.soc-muted{color:var(--soc-muted)}\
.soc-text{color:var(--soc-text)}\
.soc-label{font-size:11px;color:var(--soc-muted)}\
.soc-bar-track{background:var(--soc-bar-track);border-radius:4px;overflow:hidden}\
.soc-pse-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:6px}\
.soc-pse-cell{background:var(--soc-card-bg);border:1px solid var(--soc-border);border-radius:5px;padding:6px 8px;font-size:12px}\
.soc-band-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:10px}\
.soc-gdm-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:10px}\
.soc-cdm-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px;margin-bottom:10px}\
';

function isDarkMode() {
	// Sample multiple elements to get a reliable reading
	var els = [document.body, document.querySelector('.main-content'), document.querySelector('#maincontent'), document.querySelector('.cbi-map')];
	for (var i = 0; i < els.length; i++) {
		if (!els[i]) continue;
		var bg = window.getComputedStyle(els[i]).backgroundColor;
		var m = bg.match(/\d+/g);
		if (m && m.length >= 3) {
			var a = m.length >= 4 ? parseFloat(m[3]) : 1;
			if (a < 0.1) continue; // transparent, skip
			var lum = (parseInt(m[0]) * 299 + parseInt(m[1]) * 587 + parseInt(m[2]) * 114) / 1000;
			return lum < 128;
		}
	}
	// Fallback: check if any known dark theme stylesheet is loaded
	var sheets = document.querySelectorAll('link[href*="dark"], link[href*="glass"]');
	return sheets.length > 0;
}

var _lastDarkMode = null;

function injectCSS() {
	var el = document.getElementById('soc-theme-css');
	if (!el) { el = document.createElement('style'); el.id = 'soc-theme-css'; document.head.appendChild(el); }

	var dark = isDarkMode();
	if (dark === _lastDarkMode) return;
	_lastDarkMode = dark;

	var vars = dark
		? ':root{--soc-card-bg:#1e1e1e;--soc-border:#333;--soc-muted:#999;--soc-text:#e0e0e0;--soc-bar-track:#333}'
		: ':root{--soc-card-bg:#fff;--soc-border:#d0d0d0;--soc-muted:#666;--soc-text:#222;--soc-bar-track:#e0e0e0}';
	el.textContent = themeCSS + vars;
}

function fmtFreq(khz) { return khz ? (khz / 1000).toFixed(0) + ' MHz' : 'N/A'; }
function calcTotalMem(regions) {
	var total = 0;
	(regions || []).forEach(function(r) {
		var m = (r.size || '').match(/(\d+)\s*(KiB|MiB|GiB)/i);
		if (m) total += Number(m[1]) * ({ k:1, m:1024, g:1048576 }[m[2][0].toLowerCase()]);
	});
	return total >= 1024 ? (total / 1024).toFixed(0) + ' MiB' : total + ' KiB';
}
function renderTemperatures(data) {
	var sensors = data && Array.isArray(data.sensors) ? data.sensors : [];
	if (!sensors.length) return E('p', { 'class':'soc-muted' }, _('No temperature sensors are available.'));
	return E('table', { 'class':'table' }, sensors.map(function(sensor) {
		return E('tr', { 'class':'tr' }, [
			E('td', { 'class':'td' }, sensor.name),
			E('td', { 'class':'td' }, (sensor.millidegrees / 1000).toFixed(1) + ' °C')
		]);
	}));
}
function renderBridgeFilter(id, state, setter) {
	return E('select', { 'id':id, 'class':'cbi-input-select', 'disabled':state.available === false ? '' : null,
		'change':function(ev) {
			var previous = state.enabled ? '1' : '0';
			ev.target.disabled = true;
			setter(Number(ev.target.value)).then(function(result) {
				if (result.error) throw new Error(result.error);
				state.enabled = result.enabled;
			}).catch(function(error) {
				ev.target.value = previous;
				ui.addNotification(null, E('p', {}, error.message), 'error');
			}).finally(function() { ev.target.disabled = false; });
		}
	}, [0,1].map(function(value) {
		return E('option', { 'value':String(value), 'selected':Number(state.enabled) === value ? '' : null }, value ? _('Enabled') : _('Disabled'));
	}));
}

/* ── CPU Frequency ── */
function freqBarState(hw, min, max, pll, gov) {
	var oc = gov==='performance' && pll>0 && (pll*1000)>max;
	return { freq: oc ? pll*1000 : Math.min(hw,max), max: oc ? pll*1000 : max, oc: oc };
}

function renderFreqBar(hw, min, max, pll, gov) {
	if (!max) return E('span',{},'N/A');
	var s = freqBarState(hw,min,max,pll,gov);
	var pct = Math.round(((s.freq-min)/Math.max(1, s.max-min))*100);
	pct = Math.max(0,Math.min(100,pct));
	var bg = s.oc ? 'linear-gradient(90deg,#e65100,#ff9800)' : 'linear-gradient(90deg,#2e7d32,#66bb6a)';
	var label = s.oc ? (pll+' MHz (OC)') : fmtFreq(s.freq);

	return E('div', { 'id':'cpu-freq-bar-wrap', 'style':'display:flex;align-items:center;gap:10px' }, [
		E('span', { 'class':'soc-muted', 'style':'font-size:90%' }, fmtFreq(min)),
		E('div', { 'style':'flex:1;border-radius:4px;height:22px;position:relative;min-width:180px;max-width:350px;overflow:hidden', 'class':'soc-bar-track' }, [
			E('div', { 'id':'cpu-freq-fill', 'style':'background:'+bg+';height:100%;border-radius:4px;width:'+pct+'%;transition:width .5s' }),
			E('span', { 'id':'cpu-freq-text', 'style':'position:absolute;inset:0;display:flex;align-items:center;justify-content:center;font-weight:bold;font-size:13px;color:#fff;text-shadow:0 1px 2px rgba(0,0,0,.6)' }, label)
		]),
		E('span', { 'id':'cpu-freq-max-label', 'class':'soc-muted', 'style':'font-size:90%' }, fmtFreq(s.max))
	]);
}

function updateFreqBar(hw, min, max, pll, gov) {
	var s = freqBarState(hw,min,max,pll,gov);
	var el = document.getElementById('cpu-freq-text'), fl = document.getElementById('cpu-freq-fill'), ml = document.getElementById('cpu-freq-max-label');
	if (el) el.textContent = s.oc ? (pll+' MHz (OC)') : fmtFreq(s.freq);
	if (fl && s.max>0) { var pct=Math.max(0,Math.min(100,Math.round(((s.freq-min)/Math.max(1, s.max-min))*100))); fl.style.width=pct+'%'; fl.style.background=s.oc?'linear-gradient(90deg,#e65100,#ff9800)':'linear-gradient(90deg,#2e7d32,#66bb6a)'; }
	if (ml) ml.textContent = fmtFreq(s.max);
}

function renderGovSelect(avail, active) {
	var gs = (avail||'').trim().split(/\s+/).filter(Boolean);
	if (!gs.length) return E('span',{},'N/A');
	return E('select', { 'id':'cpu-governor-select','class':'cbi-input-select','style':'min-width:140px','change':function(ev){
		var g=ev.target.value; ev.target.disabled=true;
		callSetGovernor(g).then(function(r){ev.target.disabled=false;if(r&&r.error) ui.addNotification(null,E('p',{},_('Error: ')+r.error),'error');}).catch(function(){ev.target.disabled=false;});
	}}, gs.map(function(g){return E('option',{'value':g,'selected':g===active?'':null},g);}));
}

function renderMaxFreqSelect(avail, cur) {
	var fs = (avail||'').trim().split(/\s+/).filter(Boolean);
	if (!fs.length) return E('span',{},'N/A');
	return E('select', { 'id':'cpu-maxfreq-select','class':'cbi-input-select','style':'min-width:140px','change':function(ev){
		var f=ev.target.value; ev.target.disabled=true;
		callSetMaxFreq(parseInt(f)).then(function(r){ev.target.disabled=false;if(r&&r.error) ui.addNotification(null,E('p',{},_('Error: ')+r.error),'error');}).catch(function(){ev.target.disabled=false;});
	}}, fs.map(function(f){return E('option',{'value':f,'selected':parseInt(f)===parseInt(cur)?'':null},(parseInt(f)/1000).toFixed(0)+' MHz');}));
}


/* ── PPE Table ── */
function renderPpeRows(entries) {
	return entries.slice(0,100).map(function(e) {
		var eth = e.eth||''; if(eth==='00:00:00:00:00:00->00:00:00:00:00:00') eth='-';
		return E('tr',{'class':'tr'},[
			E('td',{'class':'td'},e.index), E('td',{'class':'td'},E('span',{'class':e.state==='BND'?'label-success':''},e.state)),
			E('td',{'class':'td'},e.type), E('td',{'class':'td'},e.orig||'-'), E('td',{'class':'td'},e.new_flow||'-'), E('td',{'class':'td'},eth)
		]);
	});
}

/* ── Main View ── */
return view.extend({
	load: function() {
		return Promise.all([ callNpuStatus(), callPpeEntries(), callTokenInfo(), callFrameEngine(), callGetVlanOffload(), callGetPppoeOffload(), callTemperatures() ]);
	},

	render: function(data) {
		injectCSS();
		var st = data[0]||{}, ppe = data[1]||{}, ti = data[2]||{}, fe = data[3]||{}, vo = data[4]||{}, po = data[5]||{};
		var entries = Array.isArray(ppe.entries) ? ppe.entries : [];
		var memR = Array.isArray(st.memory_regions) ? st.memory_regions : [];

		var view = E('div',{'class':'cbi-map'},[
			E('h2',{},_('Airoha SoC Status')),

			E('div',{'class':'cbi-section'},[
				E('h3',{},_('Temperature Sensors')),
				E('div',{'id':'temperature-sensors'},renderTemperatures(data[6]))
			]),

			// CPU Frequency
			E('div',{'class':'cbi-section'},[
				E('h3',{},_('CPU Frequency')),
				E('table',{'class':'table'},[
					E('tr',{'class':'tr'},[ E('td',{'class':'td','width':'33%'},E('strong',{},_('Current Frequency'))), E('td',{'class':'td'}, renderFreqBar(st.cpu_hw_freq,st.cpu_min_freq,st.cpu_max_freq,st.pll_freq_mhz,st.cpu_governor)) ]),
					E('tr',{'class':'tr'},[ E('td',{'class':'td'},E('strong',{},_('Governor'))), E('td',{'class':'td'}, renderGovSelect(st.cpu_avail_governors,st.cpu_governor)) ]),
					E('tr',{'class':'tr'},[ E('td',{'class':'td'},E('strong',{},_('Max Frequency'))), E('td',{'class':'td'}, renderMaxFreqSelect(st.cpu_avail_freqs,st.cpu_max_freq)) ]),
					E('tr',{'class':'tr'},[ E('td',{'class':'td'},E('strong',{},_('CPU Cores'))), E('td',{'class':'td'},(st.cpu_count||0).toString()) ])
				])
			]),

			// NPU & Frame Engine (unified)
			E('div',{'class':'cbi-section'},[
				E('h3',{},_('NPU & Offload Engine')),
				E('table',{'class':'table'},[
					E('tr',{'class':'tr'},[ E('td',{'class':'td','width':'33%'},E('strong',{},_('NPU Status'))),
						E('td',{'class':'td','id':'npu-status'}, st.npu_loaded ?
							E('span',{'class':'label-success'},_('Active')+(st.npu_device?' ('+st.npu_device+')':'')) :
							E('span',{'class':'label-danger'},_('Not Active'))) ]),
					E('tr',{'class':'tr'},[ E('td',{'class':'td'},E('strong',{},_('Firmware / Clock / Cores'))),
						E('td',{'class':'td','id':'npu-info'}, (st.npu_version||'N/A')+' | '+(st.npu_clock?(st.npu_clock/1e6).toFixed(0)+' MHz':'N/A')+' | '+(st.npu_cores||0)+' cores') ]),
					E('tr',{'class':'tr'},[ E('td',{'class':'td'},E('strong',{},_('Reserved Memory'))),
						E('td',{'class':'td','id':'npu-memory'}, calcTotalMem(memR)+' ('+memR.length+' regions)') ])
				]),

			]),

			E('div',{'class':'cbi-section'},[
				E('h3',{},_('VLAN / PPPoE Bridge Filtering')),
				E('p',{},_('Allow bridge netfilter to inspect VLAN or PPPoE encapsulated traffic. These settings do not enable hardware acceleration by themselves; routed flow offloading is configured in Firewall or FlowSense. Bridge IP filtering must also be enabled for these settings to take effect.')),
				E('table',{'class':'table'},[
					E('tr',{'class':'tr'},[E('td',{'class':'td'},_('VLAN traffic')),E('td',{'class':'td'},renderBridgeFilter('vlan-offload-select',vo,callSetVlanOffload))]),
					E('tr',{'class':'tr'},[E('td',{'class':'td'},_('PPPoE traffic')),E('td',{'class':'td'},renderBridgeFilter('pppoe-offload-select',po,callSetPppoeOffload))])
				])
			]),

			// PPE Flow Table
			E('div',{'class':'cbi-section'},[
				E('h3',{},_('PPE Flow Offload Entries')),
				E('table',{'class':'table','id':'ppe-entries-table'},[
					E('tr',{'class':'tr cbi-section-table-titles'},[
						E('th',{'class':'th'},_('Index')), E('th',{'class':'th'},_('State')), E('th',{'class':'th'},_('Type')),
						E('th',{'class':'th'},_('Original Flow')), E('th',{'class':'th'},_('New Flow')), E('th',{'class':'th'},_('Ethernet'))
					])
				].concat(renderPpeRows(entries)))
			])
		]);

		poll.add(L.bind(function() {
			return Promise.all([ callNpuStatus(), callPpeEntries(), callTokenInfo(), callFrameEngine(), callGetVlanOffload(), callGetPppoeOffload(), callTemperatures() ]).then(L.bind(function(d) {
				injectCSS();
				var st=d[0]||{}, ppe=d[1]||{}, ti=d[2]||{}, fe=d[3]||{}, vo=d[4]||{}, po=d[5]||{};
				var entries = Array.isArray(ppe.entries)?ppe.entries:[];

				updateFreqBar(st.cpu_hw_freq,st.cpu_min_freq,st.cpu_max_freq,st.pll_freq_mhz,st.cpu_governor);
				var gs=document.getElementById('cpu-governor-select'); if(gs&&!gs.matches(':focus')) gs.value=st.cpu_governor||'';
				var fs=document.getElementById('cpu-maxfreq-select'); if(fs&&!fs.matches(':focus')) fs.value=(st.cpu_max_freq||0).toString();
				var vs=document.getElementById('vlan-offload-select'); if(vs&&!vs.matches(':focus')) vs.value=(vo.enabled?'1':'0');
				var ps=document.getElementById('pppoe-offload-select'); if(ps&&!ps.matches(':focus')) ps.value=(po.enabled?'1':'0');

				var se=document.getElementById('npu-status');
				if(se){se.innerHTML='';var sp=document.createElement('span');sp.className=st.npu_loaded?'label-success':'label-danger';sp.textContent=st.npu_loaded?(_('Active')+(st.npu_device?' ('+st.npu_device+')':'')):_('Not Active');se.appendChild(sp);}

				var temp=document.getElementById('temperature-sensors'); if(temp){temp.innerHTML='';temp.appendChild(renderTemperatures(d[6]));}

				var tb=document.getElementById('ppe-entries-table');
				if(tb){while(tb.rows.length>1)tb.deleteRow(1);renderPpeRows(entries).forEach(function(r){tb.appendChild(r);});}
			},this));
		},this), 5);

		return view;
	},

	handleSaveApply: null, handleSave: null, handleReset: null
});
