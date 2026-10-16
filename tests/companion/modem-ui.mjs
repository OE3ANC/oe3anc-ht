// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { ax25 } from '../../companion/src/kiss.mjs';
import { stationBounds } from '../../companion/src/received-map.mjs';
import { ModemUi } from '../../companion/src/modem-ui.mjs';
function element() {
    return { disabled: false, textContent: '', value: '', children: [],
        addEventListener(event, callback) { this[event] = callback; },
        setAttribute(name, value) { this[name] = value; },
        append(...children) { this.children.push(...children); },
        replaceChildren() { this.children = []; this.textContent = ''; },
        scrollIntoView() {}, focus() {},
        prepend(child) {
            this.children.unshift(child);
            child.remove = () => this.children.splice(this.children.indexOf(child), 1);
        },
        get lastChild() { return this.children.at(-1); }
    };
}
globalThis.ResizeObserver = class { observe() {} };
globalThis.document = { createElement: element };
const controls = new Map();
const root = {
    querySelector(selector) {
        if (!controls.has(selector)) {
            const node = element();
            controls.set(selector, node);
        }
        return controls.get(selector);
    }
};
let rejectPicker;
let requests = 0;
Object.defineProperty(globalThis, 'navigator', { configurable: true, value: {
    serial: { requestPort() {
        requests++;
        return new Promise((resolve, reject) => { rejectPicker = reject; });
    } }
} });
const companion = { session: 0n, port: null };
let flasherOwnsPort = false;
const ownership = [];
const panel = new ModemUi(root, companion, {
    available: () => !companion.port && !flasherOwnsPort,
    changed: () => ownership.push(panel.ownsPort)
});
assert.equal(controls.get('#modem-position').disabled, true);
assert.equal(controls.get('#modem-enter').disabled, true);
flasherOwnsPort = true;
await panel.run('connect');
assert.equal(requests, 0);
flasherOwnsPort = false;
companion.port = {};
await panel.run('connect');
assert.equal(requests, 0);
companion.port = null;
const picking = panel.run('connect');
assert.equal(panel.ownsPort, true, 'Reserve ownership before awaiting browser chooser');
await panel.run('connect');
assert.equal(requests, 1, 'No concurrent chooser/open');
rejectPicker(new Error('Picker cancelled'));
await picking;
assert.equal(panel.ownsPort, false);
assert.deepEqual(ownership, [true, false]);
assert.match(panel.status.textContent, /Picker cancelled/);
companion.session = 1n;
companion.bulk = true;
companion.enterKiss = () => { throw new Error('Must not switch during CPS'); };
await panel.run('enter');
assert.equal(panel.ownsPort, false);
console.log('Modem UI exclusive reservation, chooser cancellation and CPS exclusion passed');

for (const [name, value] of Object.entries({ source: 'N0CALL-1', destination: 'APRS',
    path: '', latitude: '49.05833333', longitude: '-72.02916667', table: '/', symbol: '-',
    comment: 'Test', addressee: 'N0CALL', text: 'Hello' })) {
    root.querySelector(`#modem-${name}`).value = value;
}
const writes = [];
panel.kiss.send = async bytes => writes.push(bytes);
await panel.run('preview-position');
await panel.run('preview-message');
assert.equal(writes.length, 0, 'Offline previews must never write serial');
assert.match(root.querySelector('#modem-position-preview').textContent, /49.05833/);
root.querySelector('#modem-text').value = 'Changed after preview';
root.querySelector('#modem-text').input();
assert.match(root.querySelector('#modem-message-preview').textContent, /Draft changed/);
panel.kiss.ready = true;
panel.kiss.port = {};
await panel.run('message');
assert.equal(writes.length, 1);
assert.match(new TextDecoder().decode(writes[0]), /Changed after preview$/);
assert.equal(root.querySelector('#modem-sent-count').textContent, '1');
const frame = { command: 0, payload: writes[0] };
for (let i = 0; i < 103; ++i) {
    panel.packet('Received', frame);
}
assert.equal(root.querySelector('#modem-received-count').textContent, '103');
assert.equal(root.querySelector('#modem-received-log').children.length, 100);
assert.equal(root.querySelector('#modem-sent-log').children.length, 1);
panel.kiss.close = async () => { panel.kiss.port = null; panel.kiss.ready = false; };
await panel.run('close');
assert.equal(root.querySelector('#modem-text').value, 'Changed after preview');
navigator.serial.requestPort = async () => ({});
panel.kiss.connect = async port => { panel.kiss.port = port; panel.kiss.ready = true; };
await panel.run('connect');
assert.equal(root.querySelector('#modem-text').value, 'Changed after preview');
assert.equal(root.querySelector('#modem-latitude').value, '49.05833333');
root.querySelector('#modem-latitude').value = 'invalid';
await panel.run('preview-position');
assert.match(panel.status.textContent, /decimal degrees/);
assert.equal(writes.length, 1);
root.querySelector('#modem-clear').click();
assert.equal(root.querySelector('#modem-received-count').textContent, '0');
assert.equal(root.querySelector('#modem-sent-count').textContent, '0');
console.log('Offline preview, fresh send, counts, bounded history and reconnect drafts passed');

// No Leaflet or network access is needed while the tab has no visible map canvas.
const map = panel.receivedMap;
assert.equal(map.stations.size, 0, 'Previews, sent packets and messages are not stations');
const positionFrame = (source, payload = '!4903.50S/07201.75W-Test') => ({ command: 0,
    payload: ax25({ source, destination: 'APRS', payload }) });
const first = positionFrame('N0CALL-1');
panel.packet('Sent', first);
assert.equal(map.stations.size, 0, 'Sent positions excluded');
panel.packet('Received', { ...first, command: 1 });
panel.packet('Received', { command: 0, payload: Uint8Array.of(1, 2) });
panel.packet('Received', positionFrame('N0CALL', '!4960.00N/07201.75W-'));
assert.equal(map.stations.size, 0, 'Commands, malformed and invalid positions excluded');
panel.packet('Received', first);
assert.equal(map.stations.size, 1);
assert.ok(map.stations.get('N0CALL-1').latitude < 0);
assert.ok(map.stations.get('N0CALL-1').longitude < 0);
assert.ok(Number.isFinite(map.stations.get('N0CALL-1').receivedAt));
panel.packet('Received', positionFrame('N0CALL-1', '=0000.00N/00000.00E>updated'));
assert.equal(map.stations.size, 1);
assert.equal(map.stations.get('N0CALL-1').latitude, 0);
assert.equal(map.stations.get('N0CALL-1').comment, 'updated');
panel.packet('Received', positionFrame('N0CALL-2'));
assert.equal(map.stations.size, 2, 'SSIDs are separate stations');
for (let i = 0; i < 101; ++i) {
    panel.packet('Received', positionFrame(`T${i}`));
}
assert.equal(map.stations.size, 100);
assert.equal(map.stations.has('T0'), false);
assert.equal(map.stations.has('T100'), true);
assert.equal(root.querySelector('#modem-stations').children.length, 100);

let tileRequests = 0;
let tileLayer;
let popup;
let fitCount = 0;
const leafletMap = {
    center: { lng: 0 },
    setView(point, zoom) { this.center = { lng: point[1] }; this.point = point;
        this.zoom = zoom; return this; },
    on() {}, invalidateSize() { this.resized = true; },
    getCenter() { return this.center; },
    fitBounds(points) { this.bounds = points; ++fitCount; },
    closePopup() { popup = null; }
};
const markers = [];
globalThis.L = {
    map() { return leafletMap; },
    marker(point) {
        const marker = { point, addTo() { markers.push(this); return this; },
            bindPopup(content) { this.content = content; return this; },
            setLatLng(point) { this.point = point; return this; },
            setPopupContent(content) { this.content = content; return this; },
            remove() { this.removed = true; } };
        return marker;
    },
    popup() { return { setLatLng(point) { this.point = point; return this; },
        setContent(content) { this.content = content; return this; },
        openOn() { popup = this; } }; },
    tileLayer(url, options) {
        assert.equal(url, 'https://tile.openstreetmap.org/{z}/{x}/{y}.png');
        assert.match(options.attribution, /openstreetmap.org\/copyright/);
        tileLayer = { on(event, callback) { this[event] = callback; },
            addTo() { ++tileRequests; }, remove() { this.removed = true; } };
        return tileLayer;
    }
};
map.canvas.clientWidth = 600;
map.canvas.clientHeight = 300;
map.show();
assert.equal(tileRequests, 0, 'Showing map does not load online tiles');
assert.equal(fitCount, 1);
panel.packet('Received', positionFrame('T100', '!9000.00N/18000.00W-<script>'));
assert.equal(fitCount, 1, 'Incoming positions must not interrupt the user view');
assert.equal(map.stations.get('T100').latitude, 90, 'Keep actual polar coordinate');
const button = root.querySelector('#modem-received-log').children[0].children[0];
button.click();
assert.ok(leafletMap.point[0] < 85.052, 'Clamp map only at Web Mercator edge');
assert.equal(leafletMap.zoom, 12);
assert.match(popup.content.textContent, /90.00000/);
assert.match(popup.content.textContent, /<script>/, 'Comment inserted as text');
assert.equal(popup.content.children.length, 0);
assert.equal(tileRequests, 0, 'Packet focus remains offline');
root.querySelector('#modem-map-online').click();
assert.equal(tileRequests, 1);
tileLayer.tileerror();
assert.match(map.status.textContent, /could not load/);
assert.match(map.status.textContent, /Received positions still work/);
root.querySelector('#modem-map-online').click();
assert.equal(tileLayer.removed, true);
assert.equal(map.tiles, null);
root.querySelector('#modem-clear').click();
assert.equal(map.stations.size, 0);
assert.equal(map.markers.size, 0);
assert.equal(popup, null);
assert.equal(map.fitButton.disabled, true);
const bounds = stationBounds([{ latitude: 0, longitude: 179 },
    { latitude: 0, longitude: -179 }]);
assert.equal(Math.abs(bounds[0][1] - bounds[1][1]), 2, 'Dateline fit uses short span');
console.log('Received map filtering, bounds, updates, focus, clear and opt-in tiles passed');
