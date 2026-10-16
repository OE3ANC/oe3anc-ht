// SPDX-License-Identifier: GPL-3.0-or-later
import { decodeAx25, decodeAprsPosition } from './kiss.mjs';

const MAX_STATIONS = 100;
const MERCATOR_LIMIT = 85.0511287798;

function mapPoint(station, center = station.longitude) {
    return [Math.max(-MERCATOR_LIMIT, Math.min(MERCATOR_LIMIT, station.latitude)),
        station.longitude + 360 * Math.round((center - station.longitude) / 360)];
}

// Cut the longitude circle at its largest empty gap to fit dateline neighbors together.
export function stationBounds(stations) {
    const longitudes = stations.map(station => (station.longitude + 360) % 360)
        .sort((a, b) => a - b);
    let gap = -1;
    let start = 0;
    for (let i = 0; i < longitudes.length; ++i) {
        const next = longitudes[(i + 1) % longitudes.length] +
            (i === longitudes.length - 1 ? 360 : 0);
        if (next - longitudes[i] > gap) {
            gap = next - longitudes[i];
            start = next % 360;
        }
    }
    return stations.map(station => mapPoint(station, start + (360 - gap) / 2));
}

function details(station) {
    const text = document.createElement('div');
    text.className = 'station-details';
    text.textContent = `${station.source} · ${station.latitude.toFixed(5)}°, ` +
        `${station.longitude.toFixed(5)}° · symbol ${station.symbol}\n` +
        `Received ${new Date(station.receivedAt).toLocaleString()}` +
        (station.comment ? `\n${station.comment}` : '') +
        (Math.abs(station.latitude) > MERCATOR_LIMIT
            ? '\nPolar position shown at the map edge; coordinates above are unchanged.' : '');
    return text;
}

export class ReceivedMap {
    constructor(root) {
        this.root = root;
        this.stations = new Map();
        this.markers = new Map();
        this.canvas = root.querySelector('#modem-map');
        this.list = root.querySelector('#modem-stations');
        this.status = root.querySelector('#modem-map-status');
        this.online = root.querySelector('#modem-map-online');
        this.fitButton = root.querySelector('#modem-map-fit');
        this.online.addEventListener('click', () => this.toggleOnline());
        this.fitButton.addEventListener('click', () => this.fit());
        // ResizeObserver also fires when the Modem tab becomes visible again.
        this.resize = new ResizeObserver(() => this.show());
        this.resize.observe(this.canvas);
        this.renderList();
    }

    receive(frame) {
        if (frame.command !== 0) {
            return null;
        }
        let item;
        try {
            item = decodeAx25(frame.payload);
        } catch {
            return null;
        }
        const point = decodeAprsPosition(item);
        if (!point) {
            return null;
        }
        const station = { ...point, source: item.source, receivedAt: Date.now() };
        this.stations.delete(station.source);
        this.stations.set(station.source, station);
        if (this.stations.size > MAX_STATIONS) {
            this.stations.delete(this.stations.keys().next().value);
        }
        this.renderList();
        this.renderMarkers();
        if (!this.fitted && this.map) {
            this.fit();
        }
        return station;
    }

    show() {
        if (!this.canvas.clientWidth || !this.canvas.clientHeight) {
            return;
        }
        if (!this.map) {
            this.map = L.map(this.canvas, { worldCopyJump: true }).setView([0, 0], 2);
            this.map.on('moveend', () => this.renderMarkers());
        }
        this.map.invalidateSize({ pan: false });
        this.renderMarkers();
        if (!this.fitted) {
            this.fit();
        }
    }

    renderList() {
        this.list.replaceChildren();
        this.fitButton.disabled = !this.stations.size;
        if (!this.stations.size) {
            this.list.textContent = 'No received positions yet.';
        }
        for (const station of [...this.stations.values()].reverse()) {
            const row = document.createElement('li');
            const button = document.createElement('button');
            button.type = 'button';
            button.className = 'secondary';
            button.textContent = `Map ${station.source}`;
            button.addEventListener('click', () => this.focus(station));
            row.append(details(station), button);
            this.list.append(row);
        }
    }

    renderMarkers() {
        if (!this.map) {
            return;
        }
        for (const [source, marker] of this.markers) {
            if (!this.stations.has(source)) {
                marker.remove();
                this.markers.delete(source);
            }
        }
        for (const station of this.stations.values()) {
            const point = mapPoint(station, this.map.getCenter().lng);
            let marker = this.markers.get(station.source);
            if (!marker) {
                marker = L.marker(point, { title: station.source, alt: station.source })
                    .addTo(this.map).bindPopup(details(station), { minWidth: 220, maxWidth: 300 });
                this.markers.set(station.source, marker);
            } else {
                marker.setLatLng(point).setPopupContent(details(station));
            }
        }
    }

    fit() {
        if (this.map && this.stations.size) {
            this.fitted = true;
            this.map.fitBounds(stationBounds([...this.stations.values()]),
                { padding: [30, 30], maxZoom: 12, animate: false });
        }
    }

    focus(station) {
        this.show();
        if (!this.map) {
            return;
        }
        this.fitted = true;
        const point = mapPoint(station, this.map.getCenter().lng);
        this.map.setView(point, 12, { animate: false });
        L.popup({ minWidth: 220, maxWidth: 300 }).setLatLng(point).setContent(details(station)).openOn(this.map);
        this.canvas.scrollIntoView({ block: 'nearest' });
        this.canvas.focus({ preventScroll: true });
    }

    toggleOnline() {
        this.show();
        if (!this.map) {
            return;
        }
        if (this.tiles) {
            this.tiles.remove();
            this.tiles = null;
            this.status.textContent = 'Online map disabled. Positions remain available offline.';
        } else {
            this.status.textContent = 'Online map enabled.';
            this.tiles = L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
                maxZoom: 19,
                attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'
            });
            this.tiles.on('tileerror', () => {
                this.status.textContent = 'Map tiles could not load. Check your internet connection, ' +
                    'then disable and enable the online map to retry. Received positions still work.';
            });
            this.tiles.addTo(this.map);
        }
        this.online.textContent = this.tiles ? 'Disable online map' : 'Enable online map';
        this.online.setAttribute('aria-pressed', String(!!this.tiles));
    }

    clear() {
        this.stations.clear();
        this.fitted = false;
        this.map?.closePopup();
        this.renderList();
        this.renderMarkers();
    }
}
