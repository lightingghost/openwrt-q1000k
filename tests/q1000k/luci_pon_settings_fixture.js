// Render the production LuCI form against its public form API and exercise validators.
const fs = require('fs');
const options = {};
const section = {
    tab() {},
    taboption(tab, type, key) {
        const o = { value() {} };
        options[key] = o;
        return o;
    }
};
const form = { Value: {}, Flag: {}, ListValue: {}, Map: function() {
    this.section = () => section;
    this.render = () => options;
} };
String.prototype.format = function(value) { return this.replace('%d', value); };
const page = new Function('view', 'form', 'uci', '_', fs.readFileSync(process.argv[2], 'utf8'))(
    { extend: x => x }, form, {}, x => x);
page.render();
const cases = JSON.parse(fs.readFileSync(0, 'utf8'));
process.stdout.write(JSON.stringify(cases.map(([key, value]) => options[key].validate('identity', value) === true)));
