// SPDX-License-Identifier: GPL-3.0-or-later
export function releaseTag(identity) {
    return identity?.match(/^(v[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?)@[a-f0-9]{40}$/)?.[1] ?? null;
}

export function githubReleaseUrl(identity) {
    const tag = releaseTag(identity);
    return tag ? `https://github.com/OE3ANC/oe3anc-ht/releases/tag/${tag}` : null;
}

export function companionLabel(release, site) {
    return site.nightlyBuiltAt ? `Nightly ${site.nightlyBuiltAt}` : `Companion ${release.label}`;
}

export function firmwareLink(release, site) {
    if (site.nightlyBuiltAt) {
        return { url: './firmware-nightly.zip', label: 'Download nightly firmware' };
    }
    const url = githubReleaseUrl(release.identity);
    return {
        url: url ?? 'https://github.com/OE3ANC/oe3anc-ht/releases',
        label: url ? `Firmware ${release.label} ↗` : 'Firmware releases ↗'
    };
}
