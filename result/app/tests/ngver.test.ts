// The one feature rule (ngver.gen.ts ngFeature, REMOTE_PROTOCOL.md section 10.0) and the app's wrapper ngHas:
// the four cases of section 3, plus the table against the app's bit lists.
import { test } from "node:test";
import assert from "node:assert/strict";

import {
  NG_FEATURES, NG_FEATURES1, NG_FEATURES2, NG_FEATURES3, NG_FEAT_FROZEN, NG_FEATURE_TABLE, NG_PROTO_VERSION,
  type NgFeatureName, decodeIdentity, ngFeature, ngHas,
} from "../src/protocol/ngext.ts";

const ALL = NG_FEATURE_TABLE.map((f) => f.name as NgFeatureName);
const BIT_LISTS = [NG_FEATURES, NG_FEATURES1, NG_FEATURES2, NG_FEATURES3];
/** what the app decided before ngver: the name is in featureNames (bits only) */
const byBits = (feats: number[], name: string) =>
  BIT_LISTS.some((list, i) => list.some((f) => f.name === name && ((feats[i] ?? 0) & f.bit) !== 0));
const ident = (ngver: number, feats: number[]) => decodeIdentity(new Uint8Array([ngver, 2, 19, ...feats]))!;

test("ngver table: bits match the app's NG_FEATURES* lists, frozen bytes = ngver 2's bit features", () => {
  for (const f of NG_FEATURE_TABLE) {
    if (f.byte == null) continue;
    assert.deepEqual(BIT_LISTS[f.byte]!.find((x) => x.name === f.name)?.bit, f.bit, f.name);
  }
  assert.equal(BIT_LISTS.flat().length, NG_FEATURE_TABLE.filter((f) => f.byte != null).length);
  assert.ok(NG_PROTO_VERSION >= 2);
  for (const f of NG_FEATURE_TABLE) {
    if (f.byte == null || f.bit == null) continue;
    assert.equal(((NG_FEAT_FROZEN[f.byte] ?? 0) & f.bit) !== 0, f.since != null && f.since <= 2, f.name);
  }
});

test("ngver 1 + bits: exactly the old bit decision (current fixtures, diagnostic images, ota_main_20)", () => {
  const cases = [[0x07], [0x3b, 0x77], [0x3b, 0x77, 0x7f, 0x3f], [0x0f, 0x07], [0x5f, 0x0b, 0x01], [0, 0x10, 0x28], [0, 0, 0, 0]];
  for (const feats of cases) {
    const id = ident(1, feats);
    for (const name of ALL) assert.equal(ngHas(id, name), byBits(feats, name), `${name} ${feats}`);
  }
});

test("ngver 2 + frozen bytes 3B/77/7F/3F: same as the bits say", () => {
  const id = ident(2, [0x3b, 0x77, 0x7f, 0x3f]);
  for (const name of ALL) {
    if (name === "filexfer" || name === "standbyfanout") continue; // no bit: implied by ngver 2 only
    assert.equal(ngHas(id, name), byBits([0x3b, 0x77, 0x7f, 0x3f], name), name);
  }
  assert.ok(ngHas(id, "power") && ngHas(id, "recorder") && ngHas(id, "session") && ngHas(id, "filexfer") && ngHas(id, "standbyfanout"));
  assert.ok(!ngHas(id, "nordump") && !ngHas(id, "canary") && !ngHas(id, "hookdemo"));
});

test("ngver 2 + zero bytes: every feature with since <= 2 still on (by version), diagnostic ones off", () => {
  const id = ident(2, [0, 0, 0, 0]);
  for (const f of NG_FEATURE_TABLE) assert.equal(ngHas(id, f.name as NgFeatureName), f.since != null && f.since <= 2, f.name);
});

test("filexfer (no bit): off on ngver 1 whatever the bytes, on from ngver 2; a since-only feature turns on without a bit", () => {
  assert.equal(ngHas(ident(1, [0x7f, 0x7f, 0x7f, 0x7f]), "filexfer"), false);
  assert.equal(ngFeature(1, [0x7f, 0x7f, 0x7f, 0x7f], "filexfer"), false);
  assert.equal(ngFeature(2, [], "filexfer"), true);
  assert.equal(ngFeature(3, [], "filexfer"), true); // a later version implies everything an earlier one did
  assert.equal(ngHas(null, "power"), false); // stock piano: no Identify reply
});

test("ngver 3 (fold step 3: S3 / S4 / MIDI channel / executor gate without off ops): every ngver 2 feature still on", () => {
  assert.ok(NG_PROTO_VERSION >= 3);
  const id2 = ident(2, [0x3b, 0x77, 0x7f, 0x3f]);
  const id3 = ident(3, [0x3b, 0x77, 0x7f, 0x3f]);
  for (const name of ALL) assert.equal(ngHas(id3, name), ngHas(id2, name), name);
});
