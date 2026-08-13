// A screen that proves the round trip rather than demonstrating a UI.
//
// It writes records, updates ONE field on all of them, reads a batch back and
// checks the decoded values against what was written - including a serial above
// 2^53, which a decoder using float64 instead of bigint would silently collapse.
// The counts and timings are shown so a failure is visible rather than implied.

import { useEffect, useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import { StatusBar } from 'expo-status-bar';

import { FieldStore, Device } from './modules/fieldstore/src';

type Line = { label: string; value: string; ok?: boolean };

const RECORDS = 2000;

export default function App() {
  const [lines, setLines] = useState<Line[]>([]);
  const [rows, setRows] = useState<Device[]>([]);

  useEffect(() => {
    const out: Line[] = [];
    const say = (label: string, value: string, ok?: boolean) => out.push({ label, value, ok });
    const timed = <T,>(label: string, fn: () => T): T => {
      const t0 = Date.now();
      const r = fn();
      say(label, `${Date.now() - t0} ms`);
      return r;
    };

    try {
      const opened = FieldStore.open();
      say('open', opened ? 'ok' : 'FAILED', opened);
      if (!opened) { setLines(out); return; }

      const before = FieldStore.stats();
      say('records already on disk', String(before.records));
      say('page size', String(before.pageSize), before.pageSize > 0);

      // Serials deliberately above 2^53 - the value a float64 decoder loses.
      const seed = BigInt(Date.now());
      const fresh: Device[] = Array.from({ length: RECORDS }, (_, i) => ({
        serial: 0x0020000000000000n + BigInt(i),
        last_seen_ms: seed,
        credit: BigInt(-1000 + i),
        firmware: 0x01040000 + i,
        status: 1,
        name: `edge-${i}`,
      }));

      const ids = timed(`create ${RECORDS} records`, () => FieldStore.createMany(fresh));
      say('ids returned', String(ids.length), ids.length === RECORDS);

      // The premise: eight bytes per record reach the log, not seventy-two.
      const stamp = BigInt(Date.now());
      timed(`update one field on ${RECORDS}`, () => FieldStore.touch(ids, stamp));
      timed('sync', () => FieldStore.sync());

      const read = timed(`read ${RECORDS} back`, () => FieldStore.readMany(ids));
      say('records decoded', String(read.length), read.length === RECORDS);

      const serialsOk = read.every((d, i) => d.serial === 0x0020000000000000n + BigInt(i));
      say('serials survive above 2^53', serialsOk ? 'ok' : 'MISMATCH', serialsOk);

      const updateOk = read.every((d) => d.last_seen_ms === stamp);
      say('the field update landed on every record', updateOk ? 'ok' : 'MISMATCH', updateOk);

      const untouchedOk = read.every((d, i) => d.credit === BigInt(-1000 + i) && d.name === `edge-${i}`);
      say('the other fields were not disturbed', untouchedOk ? 'ok' : 'MISMATCH', untouchedOk);

      FieldStore.checkpoint();
      const after = FieldStore.stats();
      say('log bytes', String(after.logBytes));
      say('live bytes', String(after.liveBytes));
      say('override nodes in use', String(after.overrides));

      // Every restart of the app re-reads what the last one wrote, which is the
      // durability claim rather than a demo of it.
      say('records after this run', String(after.records), after.records >= RECORDS);
      say('grew by', String(after.records - before.records));

      setRows(read.slice(0, 4));
    } catch (e: any) {
      out.push({ label: 'threw', value: String(e?.message ?? e), ok: false });
    }
    setLines(out);
  }, []);

  const failed = lines.some((l) => l.ok === false);

  return (
    <ScrollView style={styles.page} contentContainerStyle={styles.content}>
      <StatusBar style="light" />
      <Text style={styles.title}>field_storage</Text>
      <Text style={[styles.verdict, failed ? styles.bad : styles.good]}>
        {lines.length === 0 ? 'running…' : failed ? 'SOMETHING FAILED' : 'round trip verified'}
      </Text>

      {lines.map((l, i) => (
        <View key={i} style={styles.row}>
          <Text style={styles.label}>{l.label}</Text>
          <Text style={[styles.value, l.ok === false && styles.bad, l.ok === true && styles.good]}>
            {l.value}
          </Text>
        </View>
      ))}

      {rows.length > 0 && <Text style={styles.section}>first records, decoded</Text>}
      {rows.map((d, i) => (
        <Text key={i} style={styles.record}>
          {d.name}  serial={String(d.serial)}  seen={String(d.last_seen_ms)}  credit={String(d.credit)}
        </Text>
      ))}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  page: { flex: 1, backgroundColor: '#11131a' },
  content: { padding: 20, paddingTop: 60 },
  title: { color: '#e6e8ee', fontSize: 24, fontWeight: '600' },
  verdict: { fontSize: 15, marginTop: 4, marginBottom: 18 },
  good: { color: '#5ec27a' },
  bad: { color: '#e0605e' },
  row: { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 5 },
  label: { color: '#8b93a7', fontSize: 13, flex: 1 },
  value: { color: '#e6e8ee', fontSize: 13, fontVariant: ['tabular-nums'] },
  section: { color: '#8b93a7', fontSize: 12, marginTop: 22, marginBottom: 6, textTransform: 'uppercase' },
  record: { color: '#c8cddb', fontSize: 11, fontFamily: 'monospace', paddingVertical: 2 },
});
