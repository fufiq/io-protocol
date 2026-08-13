// GENERATED from schema.json by gen.py. Do not edit.
//
// Decodes the packed buffers the C++ side hands up. One crossing per batch;
// everything below this line is pure JS over an ArrayBuffer.

const utf8 = new TextDecoder();

function str(view: DataView, at: number, len: number): string {
    const bytes = new Uint8Array(view.buffer, view.byteOffset + at, len);
    const end = bytes.indexOf(0);
    return utf8.decode(end === -1 ? bytes : bytes.subarray(0, end));
}

export interface Device {
    serial: bigint;
    last_seen_ms: bigint;
    credit: bigint;
    firmware: number;
    status: number;
    name: string;
}

export const DeviceLayout = { tag: 1, size: 72 } as const;

export function decodeDevice(view: DataView, at: number): Device {
    return {
        serial: view.getBigUint64(at + 0, true),
        last_seen_ms: view.getBigUint64(at + 8, true),
        credit: view.getBigInt64(at + 16, true),
        firmware: view.getUint32(at + 24, true),
        status: view.getUint8(at + 28),
        name: str(view, at + 29, 40),
    };
}

export function decodeDeviceBatch(buffer: ArrayBuffer, count: number): Device[] {
    const view = new DataView(buffer);
    const out: Device[] = new Array(count);
    for (let i = 0; i < count; i++) out[i] = decodeDevice(view, i * 72);
    return out;
}
