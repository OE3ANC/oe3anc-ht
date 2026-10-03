// SPDX-License-Identifier: GPL-3.0-or-later
import { parseJson } from './json.mjs';

export const MAX_BUNDLE_BYTES = 3 * 1024 * 1024;
export const FLASH_SIZE = 0x400000;
export const ERASE_SIZE = 4096;
export const PARTITION_SIZE = 0x100000;
export const DSP_SHA256 = 'fc0e2d7a8a2a991c831daf798a965743a178a582c6cbab9f9ef6a05c34628bdb';

function fail(message) {
    throw new Error('Firmware bundle: ' + message);
}

function fields(value, names) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
        fail('expected object');
    }
    const expected = names.split(' ');
    const keys = Object.keys(value);
    if (keys.length !== expected.length || expected.some(key => !Object.hasOwn(value, key))) {
        fail('missing or unknown fields');
    }
}

function hex(value, size) {
    return typeof value === 'string' && value.length === size && /^[0-9a-f]+$/.test(value);
}
export async function sha256(bytes) {
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
    return Array.from(digest, byte => byte.toString(16).padStart(2, '0')).join('');
}
export async function parseFirmwareBundle(text) {
    const document = parseJson(text, MAX_BUNDLE_BYTES);
    fields(document, 'format schema_version target flash_size erase_size release images');
    if (
        document.format !== 'oe3anc-ht-firmware' ||
        document.schema_version !== 1 ||
        document.target !== 'c62'
    ) {
        fail('unsupported format, schema or target');
    }
    if (document.flash_size !== FLASH_SIZE || document.erase_size !== ERASE_SIZE) {
        fail('unsupported flash layout');
    }
    const release = document.release;
    fields(release, 'identity label commit source_sha256');
    if (!hex(release.commit, 40) || !hex(release.source_sha256, 64)) {
        fail('invalid release metadata');
    }
    const tag =
        typeof release.label === 'string' &&
        /^v[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?$/.exec(release.label);
    const identity =
        release.label === 'development'
            ? 'dev-' + release.source_sha256.slice(0, 32)
            : tag && tag[0] === release.label
              ? release.label + '@' + release.commit
              : null;
    if (!identity || identity.length > 96 || release.identity !== identity) {
        fail('inconsistent release identity');
    }
    if (!Array.isArray(document.images) || document.images.length !== 2) {
        fail('expected application and DSP pair');
    }
    const images = [];
    for (const [index, image] of document.images.entries()) {
        fields(image, 'role offset size sha256 data_base64');
        const role = index === 0 ? 'application' : 'dsp';
        const offset = index * PARTITION_SIZE;
        if (image.role !== role || image.offset !== offset) {
            fail('incorrect image role, order or destination');
        }
        if (!Number.isSafeInteger(image.size) || image.size < 1 || image.size > PARTITION_SIZE) {
            fail('image exceeds partition');
        }
        if (!hex(image.sha256, 64)) {
            fail('invalid image hash');
        }
        const encoded = image.data_base64;
        if (
            typeof encoded !== 'string' ||
            encoded.length !== Math.ceil(image.size / 3) * 4 ||
            !/^[A-Za-z0-9+/]*={0,2}$/.test(encoded)
        ) {
            fail('invalid base64 image');
        }
        let binary;
        try {
            binary = atob(encoded);
        } catch {
            fail('invalid base64 image');
        }
        // Reject noncanonical padding bits as well as an incorrect decoded size.
        if (binary.length !== image.size || btoa(binary) !== encoded) {
            fail('noncanonical base64 or incorrect size');
        }
        const bytes = Uint8Array.from(binary, character => character.charCodeAt(0));
        if ((await sha256(bytes)) !== image.sha256) {
            fail(role + ' checksum mismatch');
        }
        if (role === 'dsp' && image.sha256 !== DSP_SHA256) {
            fail('DSP is not the pinned UART-silent pair');
        }
        if (role === 'application') {
            const needle = new TextEncoder().encode(release.identity + '\0');
            let found = false;
            for (let start = 0; start <= bytes.length - needle.length && !found; start++) {
                if (bytes[start] === needle[0]) {
                    found = needle.every((byte, i) => bytes[start + i] === byte);
                }
            }
            if (!found) {
                fail('application release identity mismatch');
            }
        }
        images.push(Object.freeze({ role, offset, size: image.size, sha256: image.sha256, bytes }));
    }
    return Object.freeze({ release: Object.freeze(release), images: Object.freeze(images) });
}
