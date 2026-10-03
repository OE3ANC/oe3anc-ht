// SPDX-License-Identifier: GPL-3.0-or-later
// RFC 1321 digest, used only to compare bootloader readback with the RAM helper.
// SHA-256 remains the downloadable backup's integrity hash.
const shifts = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
const constants = Int32Array.from({ length: 64 }, (_, i) =>
    Math.floor(Math.abs(Math.sin(i + 1)) * 0x100000000)
);
export function md5(bytes) {
    const padded = new Uint8Array(Math.ceil((bytes.length + 9) / 64) * 64);
    padded.set(bytes);
    padded[bytes.length] = 0x80;
    const view = new DataView(padded.buffer);
    view.setUint32(padded.length - 8, (bytes.length * 8) >>> 0, true);
    view.setUint32(padded.length - 4, Math.floor(bytes.length / 0x20000000), true);
    let a = 0x67452301;
    let b = 0xefcdab89;
    let c = 0x98badcfe;
    let d = 0x10325476;

    // Bitwise coercions intentionally wrap arithmetic to MD5's 32-bit words.
    for (let offset = 0; offset < padded.length; offset += 64) {
        let aa = a;
        let bb = b;
        let cc = c;
        let dd = d;
        for (let i = 0; i < 64; i++) {
            const round = i >>> 4;
            const f =
                round === 0
                    ? (bb & cc) | (~bb & dd)
                    : round === 1
                      ? (dd & bb) | (~dd & cc)
                      : round === 2
                        ? bb ^ cc ^ dd
                        : cc ^ (bb | ~dd);
            const word =
                round === 0
                    ? i
                    : round === 1
                      ? (5 * i + 1) % 16
                      : round === 2
                        ? (3 * i + 5) % 16
                        : (7 * i) % 16;
            const sum = (aa + f + constants[i] + view.getInt32(offset + word * 4, true)) | 0;
            const shift = shifts[round * 4 + (i % 4)];
            aa = dd;
            dd = cc;
            cc = bb;
            bb = (bb + ((sum << shift) | (sum >>> (32 - shift)))) | 0;
        }
        a = (a + aa) | 0;
        b = (b + bb) | 0;
        c = (c + cc) | 0;
        d = (d + dd) | 0;
    }

    const result = new Uint8Array(16);
    const output = new DataView(result.buffer);
    [a, b, c, d].forEach((value, index) => output.setInt32(index * 4, value, true));
    return result;
}
