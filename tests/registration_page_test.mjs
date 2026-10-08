// Execute the actual embedded registration-page JS against a mocked DOM/fetch.
// No browser, vehicle, socket or real fetch is used.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(new URL('../src/main.cpp', import.meta.url), 'utf8');
const page = source.split('void showRegistrationPage() {')[1].split('String statusJson()')[0];
const script = page.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = Object.fromEntries(['result', 'detail', 'start', 'adopt'].map(id => [id, {}]));
let result = {result: 'idle', busy: false, detail: ''};
let fail = false;
const context = vm.createContext({
    document: {getElementById: id => elements[id]},
    fetch: async (url, options) => {
        assert.equal(url, '/registration/status');
        assert.equal(options.cache, 'no-store');
        return {ok: !fail, json: async () => result};
    },
    setInterval: (callback, ms) => assert.equal(ms, 2000),
});
new vm.Script(script).runInContext(context);
for (const status of ['idle', 'queued', 'waiting_vehicle', 'waiting_ack', 'acknowledged', 'full', 'failed', 'uncertain', 'cancelled']) {
    for (const busy of [true, false]) {
        result = {result: status, busy, detail: '<img src=x onerror=alert(1)>'};
        await context.poll();
        assert.equal(elements.adopt.disabled, busy || status !== 'acknowledged');
        assert.equal(elements.start.disabled, busy);
        assert.equal(elements.detail.textContent, result.detail);
        assert.ok(elements.result.textContent);
    }
}
fail = true;
await context.poll();
assert.match(elements.result.textContent, /Reconnecter au portail/);
assert.equal(elements.start.disabled, true);
assert.equal(elements.adopt.disabled, true);
assert.doesNotMatch(script, /innerHTML|outerHTML/);
assert.match(page, /name='confirm' value='yes' required/);
assert.equal((page.match(/<form method='post'/g) || []).length, 3);
console.log('PASS registration page: actual JS, all states, busy/ACK adoption gates, safe text and reconnect guidance');
