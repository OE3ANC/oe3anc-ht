// SPDX-License-Identifier: GPL-3.0-or-later
import { release } from './release.mjs';
import { site } from './site.mjs';
import { CompanionConnection } from './serial.mjs';
import { CpsEditor } from './cps.mjs';
import { ConnectedCps } from './connected-cps.mjs';
import { LiveUi } from './live-ui.mjs';
import { FirmwareTools } from './firmware-tools.mjs';
import { releaseTag, githubReleaseUrl } from './release-links.mjs';
import { setupToolTabs } from './tabs.mjs';

const editor = new CpsEditor(document.querySelector('#cps'));
const connect = document.querySelector('#connect');
const disconnect = document.querySelector('#disconnect');
const status = document.querySelector('#status');
const matching = document.querySelector('#matching');
const matchingRelease = document.querySelector('#matching-release');
document.querySelector('#release').textContent = `Companion ${release.label}`;

for (const link of document.querySelectorAll('[data-release-link]')) {
    const url = githubReleaseUrl(release.identity);
    link.href = url ?? 'https://github.com/OE3ANC/oe3anc-ht/releases';
    link.textContent = url ? `Firmware ${release.label} ↗` : 'Firmware releases ↗';
}

let connecting = false;

function updatePortButtons() {
    connect.disabled =
        connecting || !!connection.port || firmwareTools.ownsPort || !('serial' in navigator);
    firmwareTools.update();
}

function state(value) {
    disconnect.disabled = !value.connected;
    updatePortButtons();
    status.classList.toggle('error', !!value.error);
    status.textContent = value.error
        ? value.error.message
        : value.connected
          ? `Connected to ${value.info.target}`
          : 'Disconnected';
    connectedCps.update();
    liveUi.update();
}
const connection = new CompanionConnection(release, state);
const connectedCps = new ConnectedCps(document.querySelector('#cps'), editor, connection);
const liveUi = new LiveUi(document.querySelector('#live-ui'), connection);
const firmwareTools = new FirmwareTools(document.querySelector('#firmware-tools'), {
    available: () => !connecting && !connection.port,
    changed: updatePortButtons
});
setupToolTabs(document, () => liveUi.update());
if (!('serial' in navigator)) {
    connect.disabled = true;
    status.textContent = 'Serial connections require a desktop browser with Web Serial support.';
}
connect.addEventListener('click', async () => {
    if (connecting || connection.port || firmwareTools.ownsPort) {
        return;
    }
    connecting = true;
    updatePortButtons();
    matching.hidden = true;
    matchingRelease.hidden = true;
    status.textContent = 'Connecting…';
    try {
        const port = await navigator.serial.requestPort();
        await connection.connect(port);
    } catch (error) {
        state({ connected: false, error });
        const tag = releaseTag(error.requiredRelease);
        if (tag) {
            matching.href = `${site.basePath}${tag}/`;
            matching.hidden = false;
            matchingRelease.href = githubReleaseUrl(error.requiredRelease);
            matchingRelease.textContent = `Open ${tag} firmware release ↗`;
            matchingRelease.hidden = false;
        }
    } finally {
        connecting = false;
        updatePortButtons();
    }
});
disconnect.addEventListener('click', async () => {
    disconnect.disabled = true;
    // Use the same cancellation/cleanup slot as CPS before closing an idle link.
    // Lost/busy links still rely on the radio's independent lease.
    try {
        if (connection.session) {
            await connection.transfer(async () => {});
        }
    } catch {}
    await connection.close();
    state({ connected: false });
});
