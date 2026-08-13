// The typed face of the engine.
//
// Everything below the encode/decode pair is pure JS over an ArrayBuffer: one
// crossing carries a whole batch, and nothing here calls native per record. The
// offsets come from schema.ts, which is generated from the same schema.json
// that generates the C++ structs - so a layout drift is a compile error on the
// native side rather than wrong numbers on this one.

import { NativeModule, requireNativeModule } from 'expo';
import { Device, DeviceLayout, decodeDevice } from './schema';

declare class FieldStoreNative extends NativeModule<{}> {
  open(): boolean;
  close(): void;
  createMany(packed: Uint8Array): number[];
  updateU64(ids: number[], offset: number, values: number[]): void;
  readMany(ids: number[]): Uint8Array;
  readAll(limit: number): Uint8Array;
  sync(): void;
  checkpoint(): boolean;
  stats(): number[];
}

const native = requireNativeModule<FieldStoreNative>('FieldStoreModule');

// Field offsets, by name, straight out of the generated layout. This is what
// makes updateField(id, 'last_seen_ms', v) mean the same thing on both sides.
export const DeviceOffsets = {
  serial: 0,
  last_seen_ms: 8,
  credit: 16,
  firmware: 24,
  status: 28,
  name: 29,
} as const;

const utf8 = new TextEncoder();

function encodeDevice(view: DataView, at: number, d: Device): void {
  view.setBigUint64(at + DeviceOffsets.serial, d.serial, true);
  view.setBigUint64(at + DeviceOffsets.last_seen_ms, d.last_seen_ms, true);
  view.setBigInt64(at + DeviceOffsets.credit, d.credit, true);
  view.setUint32(at + DeviceOffsets.firmware, d.firmware, true);
  view.setUint8(at + DeviceOffsets.status, d.status);
  const bytes = utf8.encode(d.name).subarray(0, 39);
  new Uint8Array(view.buffer, view.byteOffset + at + DeviceOffsets.name, 40).set(bytes);
}

export type { Device };

export const FieldStore = {
  open: () => native.open(),
  close: () => native.close(),
  sync: () => native.sync(),
  checkpoint: () => native.checkpoint(),

  // N records, one crossing.
  createMany(devices: Device[]): number[] {
    const buf = new ArrayBuffer(devices.length * DeviceLayout.size);
    const view = new DataView(buf);
    devices.forEach((d, i) => encodeDevice(view, i * DeviceLayout.size, d));
    return native.createMany(new Uint8Array(buf));
  },

  // A field update: eight bytes reach the log, not seventy-two.
  touch(ids: number[], whenMs: bigint): void {
    native.updateU64(ids, DeviceOffsets.last_seen_ms, ids.map(() => Number(whenMs)));
  },

  readMany(ids: number[]): Device[] {
    return decodeBatch(native.readMany(ids), ids.length);
  },

  readAll(limit = 1000): Device[] {
    const bytes = native.readAll(limit);
    return decodeBatch(bytes, bytes.byteLength / DeviceLayout.size);
  },

  stats() {
    const [records, logBytes, liveBytes, overrides, pageSize] = native.stats();
    return { records, logBytes, liveBytes, overrides, pageSize };
  },
};

function decodeBatch(bytes: Uint8Array, count: number): Device[] {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const out: Device[] = new Array(count);
  for (let i = 0; i < count; i++) out[i] = decodeDevice(view, i * DeviceLayout.size);
  return out;
}
