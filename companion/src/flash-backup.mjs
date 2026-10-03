// SPDX-License-Identifier: GPL-3.0-or-later
import { parseJson } from './json.mjs';
import { FLASH_SIZE, sha256 } from './firmware-bundle.mjs';
import { HELPER_SHA256 } from './bootloader.mjs';
export const MAX_BACKUP_BYTES = 6 * 1024 * 1024;

function fail(message) {
    throw new Error('Flash backup: ' + message);
}

function fields(object, names) {
    const expected = names.split(' ');
    if (
        !object ||
        typeof object !== 'object' ||
        Array.isArray(object) ||
        Object.keys(object).length !== expected.length ||
        expected.some(key => !Object.hasOwn(object, key))
    ) {
        fail('missing or unknown fields');
    }
}

function device(value) {
    fields(value, 'chip_id flash_id');
    if (
        value.chip_id !== null &&
        (typeof value.chip_id !== 'string' ||
            value.chip_id.length !== 16 ||
            !/^[0-9a-f]+$/.test(value.chip_id) ||
            ['0000000000000000', 'ffffffffffffffff'].includes(value.chip_id))
    ) {
        fail('invalid chip ID');
    }
    if (
        !Number.isSafeInteger(value.flash_id) ||
        value.flash_id <= 0 ||
        value.flash_id >= 0xffffff ||
        ((value.flash_id >>> 16) & 0xff) !== 22
    ) {
        fail('invalid 4 MiB flash ID');
    }
}

function base64(bytes) {
    let binary = '';
    for (let offset = 0; offset < bytes.length; offset += 8192) {
        binary += String.fromCharCode(...bytes.subarray(offset, offset + 8192));
    }
    return btoa(binary);
}
export async function createFlashBackup(info, bytes) {
    if (
        info?.target !== 'c62' ||
        info.flashSize !== FLASH_SIZE ||
        !(bytes instanceof Uint8Array) ||
        bytes.length !== FLASH_SIZE
    ) {
        fail('only complete detected C62 flash is a backup');
    }
    const identity = { chip_id: info.chipId, flash_id: info.flashId };
    device(identity);
    return (
        JSON.stringify({
            format: 'oe3anc-ht-flash-backup',
            schema_version: 1,
            target: 'c62',
            offset: 0,
            size: FLASH_SIZE,
            helper_sha256: HELPER_SHA256,
            device: identity,
            created_utc: new Date().toISOString(),
            sha256: await sha256(bytes),
            data_base64: base64(bytes)
        }) + '\n'
    );
}
export async function parseFlashBackup(text) {
    const value = parseJson(text, MAX_BACKUP_BYTES);
    fields(
        value,
        'format schema_version target offset size helper_sha256 device created_utc sha256 data_base64'
    );
    if (
        value.format !== 'oe3anc-ht-flash-backup' ||
        value.schema_version !== 1 ||
        value.target !== 'c62' ||
        value.offset !== 0 ||
        value.size !== FLASH_SIZE ||
        value.helper_sha256 !== HELPER_SHA256
    ) {
        fail('unsupported or incomplete flash layout');
    }
    device(value.device);
    if (
        typeof value.created_utc !== 'string' ||
        !Number.isFinite(Date.parse(value.created_utc)) ||
        new Date(value.created_utc).toISOString() !== value.created_utc
    ) {
        fail('invalid timestamp');
    }
    if (
        typeof value.sha256 !== 'string' ||
        value.sha256.length !== 64 ||
        !/^[0-9a-f]+$/.test(value.sha256)
    ) {
        fail('invalid integrity hash');
    }
    const encoded = value.data_base64;
    if (
        typeof encoded !== 'string' ||
        encoded.length !== Math.ceil(FLASH_SIZE / 3) * 4 ||
        !/^[A-Za-z0-9+/]*={0,2}$/.test(encoded)
    ) {
        fail('invalid base64 bytes');
    }
    let binary;
    try {
        binary = atob(encoded);
    } catch {
        fail('invalid base64 bytes');
    }
    if (binary.length !== FLASH_SIZE || btoa(binary) !== encoded) {
        fail('incomplete or noncanonical bytes');
    }
    const bytes = Uint8Array.from(binary, character => character.charCodeAt(0));
    if ((await sha256(bytes)) !== value.sha256) {
        fail('checksum mismatch');
    }
    return Object.freeze({ device: Object.freeze(value.device), sha256: value.sha256, bytes });
}
