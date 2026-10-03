// SPDX-License-Identifier: GPL-3.0-or-later
function fail(path, reason) {
    throw new Error(`${path}: ${reason}`);
}
// Native JSON.parse hides duplicate keys and numeric token forms (1.0, 1e0).
// The small bounded tokenizer retains those two contract checks, delegating
// JSON string unescaping to the platform rather than owning an escape codec.
export function parseJson(text, maxBytes) {
    if (typeof text !== 'string' || new TextEncoder().encode(text).length > maxBytes) {
        fail('JSON', `input exceeds ${maxBytes} bytes`);
    }
    let cursor = 0;
    const whitespace = () => {
        while (/[ \t\r\n]/.test(text[cursor] ?? '\0')) {
            cursor++;
        }
    };
    function value(depth) {
        whitespace();
        if (depth > 32) {
            fail('JSON', 'nesting limit exceeded');
        }
        const start = cursor;
        const token = text[cursor];
        if (token === '"') {
            cursor++;
            while (cursor < text.length) {
                const byte = text[cursor++];
                if (byte === '\\') {
                    cursor++;
                } else if (byte === '"') {
                    return JSON.parse(text.slice(start, cursor));
                }
            }
            fail('JSON', 'unterminated string');
        }
        if (token === '{' || token === '[') {
            const result = token === '{' ? Object.create(null) : [];
            const end = token === '{' ? '}' : ']';
            cursor++;
            whitespace();
            if (text[cursor] === end) {
                cursor++;
                return result;
            }
            while (true) {
                if (token === '{') {
                    whitespace();
                    if (text[cursor] !== '"') {
                        fail('JSON', `expected object key at ${cursor}`);
                    }
                    const key = value(depth + 1);
                    whitespace();
                    if (Object.hasOwn(result, key)) {
                        fail('JSON', `duplicate JSON key ${JSON.stringify(key)}`);
                    }
                    if (text[cursor++] !== ':') {
                        fail('JSON', 'expected colon');
                    }
                    result[key] = value(depth + 1);
                } else {
                    result.push(value(depth + 1));
                }
                whitespace();
                const separator = text[cursor++];
                if (separator === end) {
                    return result;
                }
                if (separator !== ',') {
                    fail('JSON', `expected comma or ${end}`);
                }
            }
        }
        for (const [word, parsed] of [
            ['true', true],
            ['false', false],
            ['null', null]
        ]) {
            if (text.startsWith(word, cursor)) {
                cursor += word.length;
                return parsed;
            }
        }
        const number = /^-?(?:0|[1-9][0-9]*)/.exec(text.slice(cursor));
        if (!number || /[.eE0-9]/.test(text[cursor + number[0].length] ?? '\0')) {
            fail('JSON', `expected integer token or value at ${cursor}`);
        }
        cursor += number[0].length;
        const parsed = Number(number[0]);
        if (!Number.isSafeInteger(parsed)) {
            fail('JSON', 'integer exceeds exact numeric range');
        }
        return parsed;
    }
    const result = value(0);
    whitespace();
    if (cursor !== text.length) {
        fail('JSON', 'unexpected trailing data');
    }
    return result;
}
