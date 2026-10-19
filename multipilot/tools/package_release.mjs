// After building both targets: node multipilot/tools/package_release.mjs 1.2.3
// Validates all images before replacing release/<board> and staging GH assets.
import { readFile, mkdir, copyFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import assert from 'node:assert/strict';

const version = process.argv[2];
assert.match(version || '', /^\d+\.\d+\.\d+(?:-[\w.]+)?$/, 'Supply the built firmware version');
const root = new URL('../../', import.meta.url);
const assets = new URL(`.dev/release-v${version}/`, root);
const names = ['bootloader.bin', 'firmware.bin', 'ota_data_initial.bin', 'partitions.bin'];
const pending = [];
const hashes = [];
for (const board of ['c5zero', 'xiaoc5']) {
  const build = new URL(`multipilot/.pio/build/${board}/`, root);
  const files = Object.fromEntries(await Promise.all(names.map(async name => [name, await readFile(new URL(name, build))])));
  for (const name of ['bootloader.bin', 'firmware.bin']) {
    assert.equal(files[name][0], 0xe9, `${board}/${name}: image magic`);
    assert.equal(files[name].readUInt16LE(12), 23, `${board}/${name}: ESP32-C5 chip ID`);
  }
  assert.equal(files['firmware.bin'].subarray(48, 80).toString().split('\0')[0], `${version}+${board}`);
  const table = files['partitions.bin'];
  const parts = [];
  for (let i = 0; i + 32 <= table.length && table.readUInt16LE(i) === 0x50aa; i += 32)
    parts.push({type: table[i + 2], sub: table[i + 3], offset: table.readUInt32LE(i + 4), size: table.readUInt32LE(i + 8)});
  const apps = parts.filter(p => p.type === 0 && p.sub >= 0x10 && p.sub < 0x20);
  assert.equal(apps.length, 2, 'Two OTA slots required');
  assert.equal(apps[0].offset, 0x20000);
  assert.ok(apps.every(p => files['firmware.bin'].length <= p.size), 'App must fit both slots');
  const ota = parts.find(p => p.type === 1 && p.sub === 0);
  assert.equal(ota.offset, 0xf000);
  assert.equal(files['ota_data_initial.bin'].length, ota.size);
  assert.ok(0x2000 + files['bootloader.bin'].length <= 0x8000, 'Bootloader overlap');
  assert.ok(0x8000 + table.length <= ota.offset, 'Partition table overlap');
  assert.ok(ota.offset + ota.size <= apps[0].offset, 'OTA data overlap');
  for (const name of names) {
    hashes.push(`${createHash('sha256').update(files[name]).digest('hex')}  ${board}-${name}`);
    pending.push({source: new URL(name, build), dest: new URL(`release/${board}/${name}`, root), asset: new URL(`${board}-${name}`, assets)});
  }
  console.log(`Validated ${board}: ${version}, ESP32-C5, OTA slots and image sizes`);
}
await mkdir(assets, {recursive: true});
for (const p of pending) {
  await copyFile(p.source, p.dest);
  await copyFile(p.source, p.asset);
}
const sums = hashes.join('\n') + '\n';
await writeFile(new URL('release/SHA256SUMS.txt', root), sums);
await writeFile(new URL('SHA256SUMS.txt', assets), sums);
console.log(`Packaged ${pending.length} images and SHA256SUMS.txt in ${assets.pathname}`);
