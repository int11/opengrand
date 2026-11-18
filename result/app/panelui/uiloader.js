/* OpenGrand panel UI loader (ES5, self-contained). Inlined by scripts/bundlecore.ts into the
 * chooser (appUI/index.html) and the OpenGrand host page (appUI/ng/index.html) of the USB UI
 * bundle for the ORIGINAL Kawai panel shell.
 *
 * What it does
 *  - receives a signed UI package (.c79u) as chunked SysEx over the shell's MIDI bridge
 *    (REMOTE_PROTOCOL.md §10.7, ops 0x30-0x34; the piano relays them from the BLE port),
 *  - verifies it (Ed25519 over the header, SHA-512 of the payload), unpacks it (raw DEFLATE,
 *    JSON {js, css}) and keeps it in localStorage (two slots A/B, the newest confirmed one is
 *    never overwritten),
 *  - boots the newest usable version: stored slots by seq, then the bundled app; a failure
 *    (script error, no boot in 15 s, no piano in 30 s before the first confirm, 3 boots
 *    without a confirm) marks that slot bad and falls back to the next; the bundled app's
 *    failure goes back to the chooser, which counts down to the Kawai UI.
 * Every stored package is verified again at each boot. Nothing here can install code
 * without the panel-UI signing key (separate from the device OTA key).
 */
(function (root) {
  "use strict";
  var CFG = /*UI_CONFIG*/{"pub":"","keyId":"","bundled":{"version":"dev","seq":0}}/*END*/;

  var MAGIC = [0x43, 0x37, 0x39, 0x55]; // "C79U"
  var HDR = 80, SIG = 64, CHUNK = 200, MAX_PKG = 512 * 1024, MAX_UNPACKED = 4 * 1024 * 1024;
  var ABI = 1, LAYOUT = 1, MAX_TRIES = 3;
  var CAPS = 1; // Begin reply byte 5: bit 0 Data frames may carry a CRC-32 (2026-11-15)
  var OP = { status: 0x30, begin: 0x31, data: 0x32, commit: 0x33, control: 0x34 };
  var RC = { ok: 0, args: 1, busy: 3, length: 5, seq: 6, offset: 7, notRx: 8, incomplete: 9, hash: 10, header: 11, sig: 12, storage: 13, already: 14, unpack: 15, noSlot: 16, noKey: 17 };
  var K_META = "ca79ng.ui.meta", K_RX = "ca79ng.ui.rx", K_SLOT = "ca79ng.ui.slot.";
  var TAG = [0x7d, 0x43, 0x41, 0x37, 0x39];

  // ---------------------------------------------------------------- bytes
  function u8(n) { return new Uint8Array(n); }
  function rd32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0; }
  function le32(v) { return [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255]; }
  function hex(b) { var s = "", i; for (i = 0; i < b.length; i++) s += (b[i] < 16 ? "0" : "") + b[i].toString(16); return s; }
  function unhex(s) { var b = u8(s.length >> 1), i; for (i = 0; i < b.length; i++) b[i] = parseInt(s.substr(2 * i, 2), 16); return b; }
  function concat(a, b) { var r = u8(a.length + b.length); r.set(a, 0); r.set(b, a.length); return r; }
  function same(a, b) { if (a.length !== b.length) return false; var d = 0, i; for (i = 0; i < a.length; i++) d |= a[i] ^ b[i]; return d === 0; }

  var B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  var B64I = {};
  (function () { for (var i = 0; i < 64; i++) B64I[B64.charAt(i)] = i; })();
  function b64enc(b) {
    var out = [], i, n;
    for (i = 0; i < b.length; i += 3) {
      n = (b[i] << 16) | ((i + 1 < b.length ? b[i + 1] : 0) << 8) | (i + 2 < b.length ? b[i + 2] : 0);
      out.push(B64.charAt((n >> 18) & 63) + B64.charAt((n >> 12) & 63) + (i + 1 < b.length ? B64.charAt((n >> 6) & 63) : "=") + (i + 2 < b.length ? B64.charAt(n & 63) : "="));
    }
    return out.join("");
  }
  function b64dec(s) {
    s = String(s || "").replace(/[^A-Za-z0-9+\/]/g, "");
    var n = Math.floor(s.length * 3 / 4), b = u8(n), i, j = 0, v;
    for (i = 0; i + 1 < s.length; i += 4) {
      v = (B64I[s.charAt(i)] << 18) | (B64I[s.charAt(i + 1)] << 12) | ((B64I[s.charAt(i + 2)] || 0) << 6) | (B64I[s.charAt(i + 3)] || 0);
      if (j < n) b[j++] = (v >> 16) & 255;
      if (j < n && i + 2 < s.length) b[j++] = (v >> 8) & 255;
      if (j < n && i + 3 < s.length) b[j++] = v & 255;
    }
    return j === n ? b : b.subarray(0, j);
  }
  function utf8dec(b) {
    var parts = [], cps = [], i = 0, c, c2, c3, c4, cp;
    while (i < b.length) {
      c = b[i++];
      if (c < 0x80) cp = c;
      else if (c >= 0xc0 && c < 0xe0) { c2 = b[i++]; cp = ((c & 31) << 6) | (c2 & 63); }
      else if (c >= 0xe0 && c < 0xf0) { c2 = b[i++]; c3 = b[i++]; cp = ((c & 15) << 12) | ((c2 & 63) << 6) | (c3 & 63); }
      else if (c >= 0xf0) { c2 = b[i++]; c3 = b[i++]; c4 = b[i++]; cp = ((c & 7) << 18) | ((c2 & 63) << 12) | ((c3 & 63) << 6) | (c4 & 63); }
      else cp = 0xfffd;
      if (cp > 0xffff) { cp -= 0x10000; cps.push(0xd800 + (cp >> 10), 0xdc00 + (cp & 1023)); }
      else cps.push(cp);
      if (cps.length >= 8192) { parts.push(String.fromCharCode.apply(null, cps)); cps = []; }
    }
    parts.push(String.fromCharCode.apply(null, cps));
    return parts.join("");
  }
  function ascii(b) { var s = "", i; for (i = 0; i < b.length && b[i]; i++) s += String.fromCharCode(b[i] & 0x7f); return s; }

  var CRC_T = (function () {
    var t = [], n, c, k;
    for (n = 0; n < 256; n++) { c = n; for (k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
    return t;
  })();
  function crc32(b) { var c = 0xffffffff, i; for (i = 0; i < b.length; i++) c = CRC_T[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }

  // 7-in-8 packing, the same as the cave's (REMOTE_PROTOCOL.md §10.1)
  function pack7(raw) {
    var out = [], i, k, hi, n;
    for (i = 0; i < raw.length; i += 7) {
      n = Math.min(7, raw.length - i); hi = 0;
      for (k = 0; k < n; k++) if (raw[i + k] & 0x80) hi |= 1 << k;
      out.push(hi);
      for (k = 0; k < n; k++) out.push(raw[i + k] & 0x7f);
    }
    return out;
  }
  function unpack7(d) {
    var out = [], i, k, n, hi;
    for (i = 0; i < d.length; ) {
      hi = d[i]; n = Math.min(7, d.length - i - 1);
      for (k = 0; k < n; k++) out.push(d[i + 1 + k] | ((hi >> k) & 1 ? 0x80 : 0));
      i += 1 + n;
    }
    return new Uint8Array(out);
  }

  // ---------------------------------------------------------------- SHA-512 (FIPS 180-4)
  var K512 = [
    0x428a2f98, 0xd728ae22, 0x71374491, 0x23ef65cd, 0xb5c0fbcf, 0xec4d3b2f, 0xe9b5dba5, 0x8189dbbc,
    0x3956c25b, 0xf348b538, 0x59f111f1, 0xb605d019, 0x923f82a4, 0xaf194f9b, 0xab1c5ed5, 0xda6d8118,
    0xd807aa98, 0xa3030242, 0x12835b01, 0x45706fbe, 0x243185be, 0x4ee4b28c, 0x550c7dc3, 0xd5ffb4e2,
    0x72be5d74, 0xf27b896f, 0x80deb1fe, 0x3b1696b1, 0x9bdc06a7, 0x25c71235, 0xc19bf174, 0xcf692694,
    0xe49b69c1, 0x9ef14ad2, 0xefbe4786, 0x384f25e3, 0x0fc19dc6, 0x8b8cd5b5, 0x240ca1cc, 0x77ac9c65,
    0x2de92c6f, 0x592b0275, 0x4a7484aa, 0x6ea6e483, 0x5cb0a9dc, 0xbd41fbd4, 0x76f988da, 0x831153b5,
    0x983e5152, 0xee66dfab, 0xa831c66d, 0x2db43210, 0xb00327c8, 0x98fb213f, 0xbf597fc7, 0xbeef0ee4,
    0xc6e00bf3, 0x3da88fc2, 0xd5a79147, 0x930aa725, 0x06ca6351, 0xe003826f, 0x14292967, 0x0a0e6e70,
    0x27b70a85, 0x46d22ffc, 0x2e1b2138, 0x5c26c926, 0x4d2c6dfc, 0x5ac42aed, 0x53380d13, 0x9d95b3df,
    0x650a7354, 0x8baf63de, 0x766a0abb, 0x3c77b2a8, 0x81c2c92e, 0x47edaee6, 0x92722c85, 0x1482353b,
    0xa2bfe8a1, 0x4cf10364, 0xa81a664b, 0xbc423001, 0xc24b8b70, 0xd0f89791, 0xc76c51a3, 0x0654be30,
    0xd192e819, 0xd6ef5218, 0xd6990624, 0x5565a910, 0xf40e3585, 0x5771202a, 0x106aa070, 0x32bbd1b8,
    0x19a4c116, 0xb8d2d0c8, 0x1e376c08, 0x5141ab53, 0x2748774c, 0xdf8eeb99, 0x34b0bcb5, 0xe19b48a8,
    0x391c0cb3, 0xc5c95a63, 0x4ed8aa4a, 0xe3418acb, 0x5b9cca4f, 0x7763e373, 0x682e6ff3, 0xd6b2b8a3,
    0x748f82ee, 0x5defb2fc, 0x78a5636f, 0x43172f60, 0x84c87814, 0xa1f0ab72, 0x8cc70208, 0x1a6439ec,
    0x90befffa, 0x23631e28, 0xa4506ceb, 0xde82bde9, 0xbef9a3f7, 0xb2c67915, 0xc67178f2, 0xe372532b,
    0xca273ece, 0xea26619c, 0xd186b8c7, 0x21c0c207, 0xeada7dd6, 0xcde0eb1e, 0xf57d4f7f, 0xee6ed178,
    0x06f067aa, 0x72176fba, 0x0a637dc5, 0xa2c898a6, 0x113f9804, 0xbef90dae, 0x1b710b35, 0x131c471b,
    0x28db77f5, 0x23047d84, 0x32caab7b, 0x40c72493, 0x3c9ebe0a, 0x15c9bebc, 0x431d67c4, 0x9c100d4c,
    0x4cc5d4be, 0xcb3e42b6, 0x597f299c, 0xfc657e2a, 0x5fcb6fab, 0x3ad6faec, 0x6c44198c, 0x4a475817
  ];
  // 64-bit words as (hi, lo) int32 pairs; R* = rotate right, sums carried through doubles
  function sha512(msg) {
    var H = [0x6a09e667, 0xf3bcc908, 0xbb67ae85, 0x84caa73b, 0x3c6ef372, 0xfe94f82b, 0xa54ff53a, 0x5f1d36f1,
      0x510e527f, 0xade682d1, 0x9b05688c, 0x2b3e6c1f, 0x1f83d9ab, 0xfb41bd6b, 0x5be0cd19, 0x137e2179];
    var len = msg.length, total = ((len + 17 + 127) >> 7) << 7, buf = u8(total), W = new Int32Array(160);
    var i, t, o, ah, al, bh, bl, ch, cl, dh, dl, eh, el, fh, fl, gh, gl, hh, hl, xh, xl, yh, yl, sh, sl, s0h, s0l, s1h, s1l, lo, hi;
    buf.set(msg, 0); buf[len] = 0x80;
    lo = (len * 8) >>> 0; hi = Math.floor(len / 0x20000000);
    buf[total - 8] = hi >>> 24; buf[total - 7] = (hi >>> 16) & 255; buf[total - 6] = (hi >>> 8) & 255; buf[total - 5] = hi & 255;
    buf[total - 4] = lo >>> 24; buf[total - 3] = (lo >>> 16) & 255; buf[total - 2] = (lo >>> 8) & 255; buf[total - 1] = lo & 255;
    for (o = 0; o < total; o += 128) {
      for (t = 0; t < 32; t++) W[t] = (buf[o + 4 * t] << 24) | (buf[o + 4 * t + 1] << 16) | (buf[o + 4 * t + 2] << 8) | buf[o + 4 * t + 3];
      for (t = 16; t < 80; t++) {
        xh = W[2 * (t - 15)]; xl = W[2 * (t - 15) + 1];
        s0h = ((xh >>> 1) | (xl << 31)) ^ ((xh >>> 8) | (xl << 24)) ^ (xh >>> 7);
        s0l = ((xl >>> 1) | (xh << 31)) ^ ((xl >>> 8) | (xh << 24)) ^ ((xl >>> 7) | (xh << 25));
        yh = W[2 * (t - 2)]; yl = W[2 * (t - 2) + 1];
        s1h = ((yh >>> 19) | (yl << 13)) ^ ((yl >>> 29) | (yh << 3)) ^ (yh >>> 6);
        s1l = ((yl >>> 19) | (yh << 13)) ^ ((yh >>> 29) | (yl << 3)) ^ ((yl >>> 6) | (yh << 26));
        sl = (W[2 * (t - 16) + 1] >>> 0) + (s0l >>> 0) + (W[2 * (t - 7) + 1] >>> 0) + (s1l >>> 0);
        sh = W[2 * (t - 16)] + s0h + W[2 * (t - 7)] + s1h + Math.floor(sl / 4294967296);
        W[2 * t] = sh | 0; W[2 * t + 1] = sl | 0;
      }
      ah = H[0]; al = H[1]; bh = H[2]; bl = H[3]; ch = H[4]; cl = H[5]; dh = H[6]; dl = H[7];
      eh = H[8]; el = H[9]; fh = H[10]; fl = H[11]; gh = H[12]; gl = H[13]; hh = H[14]; hl = H[15];
      for (t = 0; t < 80; t++) {
        // T1 = h + Σ1(e) + Ch(e,f,g) + K[t] + W[t]
        s1h = ((eh >>> 14) | (el << 18)) ^ ((eh >>> 18) | (el << 14)) ^ ((el >>> 9) | (eh << 23));
        s1l = ((el >>> 14) | (eh << 18)) ^ ((el >>> 18) | (eh << 14)) ^ ((eh >>> 9) | (el << 23));
        xh = (eh & fh) ^ (~eh & gh); xl = (el & fl) ^ (~el & gl);
        sl = (hl >>> 0) + (s1l >>> 0) + (xl >>> 0) + (K512[2 * t + 1] >>> 0) + (W[2 * t + 1] >>> 0);
        sh = hh + s1h + xh + K512[2 * t] + W[2 * t] + Math.floor(sl / 4294967296);
        // T2 = Σ0(a) + Maj(a,b,c)
        s0h = ((ah >>> 28) | (al << 4)) ^ ((al >>> 2) | (ah << 30)) ^ ((al >>> 7) | (ah << 25));
        s0l = ((al >>> 28) | (ah << 4)) ^ ((ah >>> 2) | (al << 30)) ^ ((ah >>> 7) | (al << 25));
        yh = (ah & bh) ^ (ah & ch) ^ (bh & ch); yl = (al & bl) ^ (al & cl) ^ (bl & cl);
        hh = gh; hl = gl; gh = fh; gl = fl; fh = eh; fl = el;
        lo = (dl >>> 0) + (sl >>> 0); eh = (dh + sh + Math.floor(lo / 4294967296)) | 0; el = lo | 0;
        dh = ch; dl = cl; ch = bh; cl = bl; bh = ah; bl = al;
        lo = (sl >>> 0) + (s0l >>> 0) + (yl >>> 0);
        ah = (sh + s0h + yh + Math.floor(lo / 4294967296)) | 0; al = lo | 0;
      }
      var v = [ah, al, bh, bl, ch, cl, dh, dl, eh, el, fh, fl, gh, gl, hh, hl];
      for (i = 0; i < 16; i += 2) {
        lo = (H[i + 1] >>> 0) + (v[i + 1] >>> 0);
        H[i] = (H[i] + v[i] + Math.floor(lo / 4294967296)) | 0; H[i + 1] = lo | 0;
      }
    }
    var out = u8(64);
    for (i = 0; i < 16; i++) { out[4 * i] = H[i] >>> 24; out[4 * i + 1] = (H[i] >>> 16) & 255; out[4 * i + 2] = (H[i] >>> 8) & 255; out[4 * i + 3] = H[i] & 255; }
    return out;
  }

  // ---------------------------------------------------------------- Ed25519 verify (RFC 8032)
  // Field GF(2^255-19) in 16 limbs of 16 bits (the TweetNaCl representation). The curve
  // constants are derived at start-up from their definitions instead of being typed in.
  function gf(init) { var r = new Float64Array(16), i; if (init) for (i = 0; i < init.length; i++) r[i] = init[i]; return r; }
  function car(o) { var i, c; for (i = 0; i < 16; i++) { o[i] += 65536; c = Math.floor(o[i] / 65536); if (i < 15) o[i + 1] += c - 1; else o[0] += 38 * (c - 1); o[i] -= c * 65536; } }
  function sel(p, q, b) { var t, i, c = ~(b - 1); for (i = 0; i < 16; i++) { t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; } }
  function packF(o, n) {
    var i, j, b, m = gf(), t = gf();
    for (i = 0; i < 16; i++) t[i] = n[i];
    car(t); car(t); car(t);
    for (j = 0; j < 2; j++) {
      m[0] = t[0] - 0xffed;
      for (i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
      m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
      b = (m[15] >> 16) & 1;
      m[14] &= 0xffff;
      sel(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) { o[2 * i] = t[i] & 0xff; o[2 * i + 1] = t[i] >> 8; }
  }
  function neqF(a, b) { var c = u8(32), d = u8(32); packF(c, a); packF(d, b); return !same(c, d); }
  function parF(a) { var d = u8(32); packF(d, a); return d[0] & 1; }
  function unpackF(o, n) { var i; for (i = 0; i < 16; i++) o[i] = n[2 * i] + (n[2 * i + 1] << 8); o[15] &= 0x7fff; }
  function A(o, a, b) { for (var i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
  function Z(o, a, b) { for (var i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
  var Mt = new Float64Array(31);
  function M(o, a, b) {
    var i, j, t = Mt, ai;
    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++) { ai = a[i]; for (j = 0; j < 16; j++) t[i + j] += ai * b[j]; }
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    car(o); car(o);
  }
  function S(o, a) { M(o, a, a); }
  function inv(o, x) { var c = gf(), a; for (a = 0; a < 16; a++) c[a] = x[a]; for (a = 253; a >= 0; a--) { S(c, c); if (a !== 2 && a !== 4) M(c, c, x); } for (a = 0; a < 16; a++) o[a] = c[a]; }
  function pow2523(o, x) { var c = gf(), a; for (a = 0; a < 16; a++) c[a] = x[a]; for (a = 250; a >= 0; a--) { S(c, c); if (a !== 1) M(c, c, x); } for (a = 0; a < 16; a++) o[a] = c[a]; }
  function small(n) { return gf([n & 0xffff, n >>> 16]); }

  var gf0 = gf(), gf1 = gf([1]), D = gf(), D2 = gf(), SQRTM1 = gf(), BX = gf(), BY = gf();
  (function () {
    var t = gf();
    inv(t, small(121666)); Z(D, gf0, small(121665)); M(D, D, t); // d = -121665/121666
    A(D2, D, D);
    // sqrt(-1) = 2^((p-1)/4), (p-1)/4 = 2^253 - 5: square-and-multiply over its bits
    var two = small(2), r = gf([1]), i, bit;
    for (i = 252; i >= 0; i--) {
      S(r, r);
      bit = i >= 3 ? 1 : (i === 2 ? 0 : 1); // 2^253 - 5 = 1…1011 (bits 252..3 set, 2 clear, 1..0 set)
      if (bit) M(r, r, two);
    }
    for (i = 0; i < 16; i++) SQRTM1[i] = r[i];
    inv(t, small(5)); M(BY, small(4), t); // base point y = 4/5, x even
  })();

  function add(p, q) {
    var a = gf(), b = gf(), c = gf(), d = gf(), e = gf(), f = gf(), g = gf(), h = gf(), t = gf();
    Z(a, p[1], p[0]); Z(t, q[1], q[0]); M(a, a, t);
    A(b, p[0], p[1]); A(t, q[0], q[1]); M(b, b, t);
    M(c, p[3], q[3]); M(c, c, D2);
    M(d, p[2], q[2]); A(d, d, d);
    Z(e, b, a); Z(f, d, c); A(g, d, c); A(h, b, a);
    M(p[0], e, f); M(p[1], h, g); M(p[2], g, f); M(p[3], e, h);
  }
  function cswap(p, q, b) { for (var i = 0; i < 4; i++) sel(p[i], q[i], b); }
  function packP(r, p) {
    var tx = gf(), ty = gf(), zi = gf();
    inv(zi, p[2]); M(tx, p[0], zi); M(ty, p[1], zi);
    packF(r, ty); r[31] ^= parF(tx) << 7;
  }
  function scalarmult(p, q, s) {
    var i, b;
    for (i = 0; i < 16; i++) { p[0][i] = 0; p[1][i] = i ? 0 : 1; p[2][i] = i ? 0 : 1; p[3][i] = 0; }
    for (i = 255; i >= 0; --i) {
      b = (s[(i / 8) | 0] >> (i & 7)) & 1;
      cswap(p, q, b); add(q, p); add(p, p); cswap(p, q, b);
    }
  }
  // decode a point and negate it (-P); 0 on success
  function unpackneg(r, p) {
    var t = gf(), chk = gf(), num = gf(), den = gf(), den2 = gf(), den4 = gf(), den6 = gf(), i;
    for (i = 0; i < 16; i++) r[2][i] = gf1[i];
    unpackF(r[1], p);
    S(num, r[1]); M(den, num, D); Z(num, num, r[2]); A(den, r[2], den);
    S(den2, den); S(den4, den2); M(den6, den4, den2);
    M(t, den6, num); M(t, t, den);
    pow2523(t, t);
    M(t, t, num); M(t, t, den); M(t, t, den); M(r[0], t, den);
    S(chk, r[0]); M(chk, chk, den);
    if (neqF(chk, num)) M(r[0], r[0], SQRTM1);
    S(chk, r[0]); M(chk, chk, den);
    if (neqF(chk, num)) return -1;
    if (parF(r[0]) === (p[31] >> 7)) Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return 0;
  }
  (function () { // base point x: decode y = 4/5 with sign 0, then undo the negation
    var enc = u8(32), q = [gf(), gf(), gf(), gf()];
    packF(enc, BY);
    unpackneg(q, enc);
    Z(BX, gf0, q[0]);
  })();
  var L = [0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10];
  function modL(r, x) {
    var carry, i, j, k;
    for (i = 63; i >= 32; --i) {
      carry = 0;
      for (j = i - 32, k = i - 12; j < k; ++j) {
        x[j] += carry - 16 * x[i] * L[j - (i - 32)];
        carry = Math.floor((x[j] + 128) / 256);
        x[j] -= carry * 256;
      }
      x[j] += carry;
      x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) { x[j] += carry - (x[31] >> 4) * L[j]; carry = x[j] >> 8; x[j] &= 255; }
    for (j = 0; j < 32; j++) x[j] -= carry * L[j];
    for (i = 0; i < 32; i++) { x[i + 1] += x[i] >> 8; r[i] = x[i] & 255; }
  }
  function ltL(s) { for (var i = 31; i >= 0; i--) { if (s[i] < L[i]) return true; if (s[i] > L[i]) return false; } return false; }

  /** Ed25519 signature check (RFC 8032 §5.1.7, cofactorless; S must be canonical). */
  function edVerify(sig, msg, pub) {
    if (!sig || !pub || sig.length !== 64 || pub.length !== 32) return false;
    if (!ltL(sig.subarray(32))) return false;
    var q = [gf(), gf(), gf(), gf()], p = [gf(), gf(), gf(), gf()], t = u8(32), i;
    if (unpackneg(q, pub)) return false;
    var h = sha512(concat(concat(sig.subarray(0, 32), pub), msg)), x = new Float64Array(64), k = u8(32);
    for (i = 0; i < 64; i++) x[i] = h[i];
    modL(k, x);
    scalarmult(p, q, k); // [k](-A)
    var b = [gf(), gf(), gf1, gf()];
    for (i = 0; i < 16; i++) { b[0][i] = BX[i]; b[1][i] = BY[i]; }
    b[2] = gf([1]); M(b[3], BX, BY);
    var s = [gf(), gf(), gf(), gf()];
    scalarmult(s, b, sig.subarray(32)); // [S]B
    add(p, s);
    packP(t, p);
    return same(t, sig.subarray(0, 32));
  }

  // ---------------------------------------------------------------- inflate (RFC 1951, raw)
  var LBASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
  var LEXT = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
  var DBASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
  var DEXT = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
  var CLORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
  function huff(lengths, n) {
    var count = new Int16Array(16), offs = new Int16Array(16), symbol = new Int16Array(n), i;
    for (i = 0; i < n; i++) count[lengths[i]]++;
    count[0] = 0;
    for (i = 1; i < 16; i++) offs[i] = offs[i - 1] + count[i - 1];
    for (i = 0; i < n; i++) if (lengths[i]) symbol[offs[lengths[i]]++] = i;
    return { count: count, symbol: symbol };
  }
  function inflateRaw(src, sizeHint) {
    var out = u8(sizeHint > 0 ? sizeHint : src.length * 4 + 1024), op = 0, pos = 0, bb = 0, bc = 0;
    function need(n) {
      if (op + n <= out.length) return;
      var g = u8(Math.max(out.length * 2, op + n)); g.set(out.subarray(0, op)); out = g;
    }
    function bits(n) {
      while (bc < n) { if (pos >= src.length) throw new Error("inflate: input ends"); bb |= src[pos++] << bc; bc += 8; }
      var v = bb & ((1 << n) - 1); bb >>>= n; bc -= n; return v;
    }
    function decode(h) {
      var code = 0, first = 0, index = 0, len, count;
      for (len = 1; len < 16; len++) {
        code |= bits(1);
        count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
      }
      throw new Error("inflate: bad code");
    }
    var fixL = null, fixD = null, last, type, i, n, lens, hl, hd, hc, cl, ch, sym, len, dist, lt, dt, prev, rep;
    do {
      last = bits(1); type = bits(2);
      if (type === 0) {
        bb = 0; bc = 0;
        if (pos + 4 > src.length) throw new Error("inflate: stored header");
        n = src[pos] | (src[pos + 1] << 8);
        if ((n ^ 0xffff) !== (src[pos + 2] | (src[pos + 3] << 8))) throw new Error("inflate: stored length");
        pos += 4;
        if (pos + n > src.length) throw new Error("inflate: stored data");
        need(n); out.set(src.subarray(pos, pos + n), op); op += n; pos += n;
        continue;
      }
      if (type === 1) {
        if (!fixL) {
          lens = []; for (i = 0; i < 288; i++) lens[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
          fixL = huff(lens, 288);
          lens = []; for (i = 0; i < 30; i++) lens[i] = 5;
          fixD = huff(lens, 30);
        }
        lt = fixL; dt = fixD;
      } else if (type === 2) {
        hl = bits(5) + 257; hd = bits(5) + 1; hc = bits(4) + 4;
        cl = []; for (i = 0; i < 19; i++) cl[i] = 0;
        for (i = 0; i < hc; i++) cl[CLORDER[i]] = bits(3);
        ch = huff(cl, 19);
        lens = []; i = 0;
        while (i < hl + hd) {
          sym = decode(ch);
          if (sym < 16) { lens[i++] = sym; continue; }
          if (sym === 16) { if (!i) throw new Error("inflate: repeat at start"); prev = lens[i - 1]; rep = 3 + bits(2); }
          else if (sym === 17) { prev = 0; rep = 3 + bits(3); }
          else { prev = 0; rep = 11 + bits(7); }
          if (i + rep > hl + hd) throw new Error("inflate: too many lengths");
          while (rep--) lens[i++] = prev;
        }
        lt = huff(lens.slice(0, hl), hl); dt = huff(lens.slice(hl), hd);
      } else throw new Error("inflate: bad block type");
      for (;;) {
        sym = decode(lt);
        if (sym < 256) { need(1); out[op++] = sym; continue; }
        if (sym === 256) break;
        sym -= 257;
        if (sym >= 29) throw new Error("inflate: bad length");
        len = LBASE[sym] + bits(LEXT[sym]);
        sym = decode(dt);
        if (sym >= 30) throw new Error("inflate: bad distance");
        dist = DBASE[sym] + bits(DEXT[sym]);
        if (dist > op) throw new Error("inflate: distance too far");
        need(len);
        for (i = 0; i < len; i++) { out[op] = out[op - dist]; op++; }
      }
    } while (!last);
    return out.subarray(0, op);
  }

  // ---------------------------------------------------------------- package (.c79u)
  // header 80 B (LE): 0 "C79U" · 4 format 1 · 5 flags (bit0 raw DEFLATE) · 6 loader ABI · 7 0 ·
  // 8 seq u32 · 12 payload length · 16 unpacked length · 20 payload CRC-32 · 24 key id u32 ·
  // 28 version ASCII 16 B (NUL padded) · 44 0 u32 · 48 SHA-512(payload)[0..32]
  // then the Ed25519 signature (64 B) over the 80 header bytes, then the payload.
  function parseHeader(h) {
    if (!h || h.length < HDR) return null;
    for (var i = 0; i < 4; i++) if (h[i] !== MAGIC[i]) return null;
    return { format: h[4], flags: h[5], abi: h[6], seq: rd32(h, 8), length: rd32(h, 12), unpacked: rd32(h, 16), crc: rd32(h, 20), keyId: rd32(h, 24), version: ascii(h.subarray(28, 44)), hash: h.subarray(48, 80) };
  }
  function pubKey() { return CFG.pub && CFG.pub.length === 64 ? unhex(CFG.pub) : null; }
  function keyIdOf(pub) { return pub ? rd32(sha512(pub), 0) : 0; } // first 4 B of SHA-512(pub), LE
  /** Header + signature check (no payload). Returns rc and the parsed header. */
  function checkHead(hs) {
    var pub = pubKey();
    if (!pub) return { rc: RC.noKey };
    var h = parseHeader(hs);
    if (!h || h.format !== 1 || h.abi > ABI || hs.length < HDR + SIG) return { rc: RC.header };
    if (h.keyId !== keyIdOf(pub)) return { rc: RC.header, h: h };
    if (h.length > MAX_PKG || h.length === 0 || h.unpacked > MAX_UNPACKED) return { rc: RC.length, h: h };
    if (!edVerify(hs.subarray(HDR, HDR + SIG), hs.subarray(0, HDR), pub)) return { rc: RC.sig, h: h };
    return { rc: RC.ok, h: h };
  }
  /** Full check of a stored or received package; files = {js, css, version}. */
  function openPackage(pkg) {
    var c = checkHead(pkg);
    if (c.rc) return c;
    var h = c.h, body = pkg.subarray(HDR + SIG);
    if (body.length !== h.length) return { rc: RC.incomplete, h: h };
    if (crc32(body) !== h.crc || !same(sha512(body).subarray(0, 32), h.hash)) return { rc: RC.hash, h: h };
    var files;
    try {
      var raw = h.flags & 1 ? inflateRaw(body, h.unpacked) : body;
      if (raw.length !== h.unpacked) return { rc: RC.unpack, h: h };
      files = JSON.parse(utf8dec(raw));
    } catch (e) { return { rc: RC.unpack, h: h, error: String(e && e.message || e) }; }
    if (!files || typeof files.js !== "string" || typeof files.css !== "string") return { rc: RC.unpack, h: h };
    return { rc: RC.ok, h: h, files: files };
  }

  // ---------------------------------------------------------------- storage
  function ls() { try { return root.localStorage || null; } catch (e) { return null; } }
  function get(k) { var s = ls(); try { return s ? s.getItem(k) : null; } catch (e) { return null; } }
  function put(k, v) { var s = ls(); if (!s) return false; try { s.setItem(k, v); return true; } catch (e) { return false; } }
  function del(k) { var s = ls(); try { if (s) s.removeItem(k); } catch (e) { /* ignore */ } }
  function loadMeta() {
    var m = null;
    try { m = JSON.parse(get(K_META) || "null"); } catch (e) { m = null; }
    if (!m || m.v !== 1) m = { v: 1, maxSeq: 0, slots: {} };
    if (!m.slots) m.slots = {};
    return m;
  }
  function saveMeta(m) { return put(K_META, JSON.stringify(m)); }
  function slotBytes(s) { var t = get(K_SLOT + s); return t ? b64dec(t) : null; }
  function markBad(slot, why) {
    var m = loadMeta(), e = m.slots[slot];
    if (!e) return;
    e.state = "bad"; e.why = why;
    saveMeta(m);
  }
  /** Which slot a new package goes to: never the newest confirmed one (the fallback). */
  function pickSlot(m) {
    var a = m.slots.A, b = m.slots.B, keep;
    if (!a) return "A";
    if (!b) return "B";
    if (a.state === "ok" && (b.state !== "ok" || a.seq >= b.seq)) keep = "A";
    else if (b.state === "ok") keep = "B";
    else keep = a.seq >= b.seq ? "A" : "B";
    return keep === "A" ? "B" : "A";
  }

  // ---------------------------------------------------------------- receiver (ops 0x30-0x34)
  var rx = null; // { head: Uint8Array(144), h, buf: Uint8Array(len), got, saved }
  var refused = 0; // Data frames refused by length / CRC since this page loaded (Status 93..96)
  var rxState = 0, lastRc = 0; // 0 idle · 1 receiving · 2 complete · 4 stored · 5 error
  var page = 0x7f, running = "none";
  var listeners = [];
  function emit(kind, info) { for (var i = 0; i < listeners.length; i++) { try { listeners[i](kind, info || {}); } catch (e) { /* listener bug */ } } }

  function saveRx() {
    if (!rx) { del(K_RX); return; }
    rx.saved = rx.got;
    put(K_RX, JSON.stringify({ head: b64enc(rx.head), got: b64enc(rx.buf.subarray(0, rx.got)) }));
  }
  function restoreRx() {
    var o = null;
    try { o = JSON.parse(get(K_RX) || "null"); } catch (e) { o = null; }
    if (!o || !o.head) return;
    var head = b64dec(o.head), h = parseHeader(head), got = b64dec(o.got || "");
    if (!h || head.length !== HDR + SIG || got.length > h.length) { del(K_RX); return; }
    rx = { head: head, h: h, buf: u8(h.length), got: got.length, saved: got.length };
    rx.buf.set(got, 0);
    rxState = rx.got === h.length ? 2 : 1;
  }

  function reply(op, data) { return [0xf0].concat(TAG, [op | 0x40], data, [0xf7]); }
  function statusRaw() {
    var m = loadMeta(), r = [], i, s, e, v;
    function ver(str) { var b = [], k; for (k = 0; k < 16; k++) b.push(k < str.length ? str.charCodeAt(k) & 0x7f : 0); return b; }
    var st = { none: 0, "new": 1, ok: 2, bad: 3 };
    r.push(LAYOUT, ABI, page & 0xff, rxState, lastRc, (ls() ? 1 : 0) | (pubKey() ? 2 : 0));
    r = r.concat(le32(rx ? rx.h.seq : 0), le32(rx ? rx.got : 0), le32(rx ? rx.h.length : 0), le32(m.maxSeq >>> 0));
    for (i = 0; i < 2; i++) {
      s = i ? "B" : "A"; e = m.slots[s];
      r = r.concat(le32(e ? e.seq : 0), [e ? st[e.state] || 0 : 0, e ? Math.min(255, e.tries || 0) : 0]);
    }
    r = r.concat(le32(CFG.bundled.seq >>> 0), [CHUNK & 0xff, CHUNK >> 8, { none: 0, bundled: 1, A: 2, B: 3 }[running] || 0]);
    r = r.concat(le32(keyIdOf(pubKey())));
    for (i = 0; i < 2; i++) { e = m.slots[i ? "B" : "A"]; v = e ? e.version || "" : ""; r = r.concat(ver(v)); }
    r = r.concat(ver(String(CFG.bundled.version || "")));
    return r.concat(le32(refused)); // 97 B (93..96 since 2026-11-15; older apps read 93)
  }

  function onBegin(raw) {
    if (raw.length < HDR + SIG) return [RC.args];
    var head = raw.subarray(0, HDR + SIG), c = checkHead(head), m = loadMeta();
    if (c.rc) { lastRc = c.rc; return [c.rc]; }
    var h = c.h;
    if (m.slots.A && m.slots.A.seq === h.seq && same(unhex(m.slots.A.hash || ""), h.hash) || m.slots.B && m.slots.B.seq === h.seq && same(unhex(m.slots.B.hash || ""), h.hash)) { lastRc = RC.already; return [RC.already].concat(le32(h.length)); }
    if (h.seq <= m.maxSeq || h.seq <= (CFG.bundled.seq >>> 0)) { lastRc = RC.seq; return [RC.seq]; }
    if (!(rx && same(rx.head, head))) { // a different package: start over (else resume)
      rx = { head: u8(HDR + SIG), h: h, buf: u8(h.length), got: 0, saved: 0 };
      rx.head.set(head, 0);
      saveRx();
    }
    rxState = rx.got === h.length ? 2 : 1; lastRc = 0;
    emit("begin", { version: h.version, seq: h.seq, total: h.length, got: rx.got });
    return [RC.ok].concat(le32(rx.got), [CAPS]); // CAPS after `received`: older senders read 5 bytes only
  }
  // Data carries exactly min(CHUNK, total - off) bytes, optionally followed by CRC-32(off LE u32 · data) (CAPS bit 0).
  // A frame that lost or gained bytes on the way still ends in F7 and used to be stored as decoded — the next frame then
  // went on from the short `received` and only the Commit hash saw it (field 2026-11-15, rc 10 after 121 s). Refused frames answer rc 7 + received: every sender resends.
  function onData(d) {
    if (d.length < 4) return [RC.args];
    if (!rx) { lastRc = RC.notRx; return [RC.notRx].concat(le32(0)); }
    var off = (d[0] << 14) | (d[1] << 7) | d[2], data = unpack7(d.subarray(3)), n, c;
    if (off > rx.got) return [RC.offset].concat(le32(rx.got)); // gap: the sender goes back
    if (off < rx.got) return [RC.ok].concat(le32(rx.got)); // duplicate: already have it
    n = Math.min(CHUNK, rx.h.length - off);
    if (d.length - 3 === data.length + Math.ceil(data.length / 7) && data.length === n + 4) { // with CRC
      c = data.subarray(n);
      data = data.subarray(0, n);
      if (crc32(concat(new Uint8Array(le32(off)), data)) !== ((c[0] | (c[1] << 8) | (c[2] << 16) | (c[3] << 24)) >>> 0)) data = null;
    } else if (d.length - 3 !== data.length + Math.ceil(data.length / 7) || data.length !== n) data = null;
    if (!data || !n) {
      refused++; emit("refused", { off: off, got: rx.got });
      return [RC.offset].concat(le32(rx.got));
    }
    rx.buf.set(data, off); rx.got += data.length;
    if (rx.got === rx.h.length) rxState = 2;
    if (rx.got - rx.saved >= 16 * CHUNK || rx.got === rx.h.length) saveRx(); // resume across a page reload
    emit("progress", { got: rx.got, total: rx.h.length });
    return [RC.ok].concat(le32(rx.got));
  }
  function onCommit() {
    if (!rx) { lastRc = RC.notRx; return [RC.notRx, 0]; }
    if (rx.got !== rx.h.length) { lastRc = RC.incomplete; return [RC.incomplete, 0]; }
    var pkg = concat(rx.head, rx.buf), o = openPackage(pkg), m, slot, ok;
    if (o.rc) { lastRc = o.rc; rxState = 5; rx = null; saveRx(); emit("error", { rc: o.rc }); return [o.rc, 0]; }
    m = loadMeta();
    if (o.h.seq <= m.maxSeq) { lastRc = RC.seq; rx = null; saveRx(); return [RC.seq, 0]; }
    slot = pickSlot(m);
    rx = null; saveRx(); // free the staging copy before the slot write (quota)
    del(K_SLOT + slot); delete m.slots[slot]; saveMeta(m);
    ok = put(K_SLOT + slot, b64enc(pkg));
    if (!ok) { lastRc = RC.storage; rxState = 5; emit("error", { rc: RC.storage }); return [RC.storage, 0]; }
    m.slots[slot] = { seq: o.h.seq, version: o.h.version, len: pkg.length, hash: hex(o.h.hash), state: "new", tries: 0 };
    m.maxSeq = o.h.seq;
    if (!saveMeta(m)) { del(K_SLOT + slot); lastRc = RC.storage; rxState = 5; return [RC.storage, 0]; }
    rxState = 4; lastRc = 0;
    emit("stored", { slot: slot, version: o.h.version, seq: o.h.seq });
    return [RC.ok, slot === "A" ? 0 : 1];
  }
  /** Control 1. Field 2026-11-15 (record 04_panel_ui_remote, last lines): a package stored with "start right away"
   *  while the shell's Android Settings activity was on top — the page never reloaded and kept the full-screen notice
   *  for 43 min; op 0x37 clicks answered rc 0 but changed nothing [inference: the WebView of a background activity
   *  does not run timers, so neither the delayed navigate nor the deferred clicks ran]. Now: a hidden page does not
   *  navigate or cover itself — it reloads when it is shown again (visibilitychange); a shown page that is still here
   *  activateTimeoutMs after the navigate takes the notice down (the old UI stays usable) and tries again the next
   *  time it is shown (at most activateRetries stalls, then it is left for the next boot, which starts the newest
   *  version anyway). */
  var activatePending = false, activateTimer = null, visArmed = false, activateStalls = 0;
  function hidden() { var d = root.document; return !!(d && d.visibilityState === "hidden"); }
  function unnotice() {
    var d = root.document, el = d && d.getElementById ? d.getElementById("ca79ui-notice") : null;
    while (el) { try { el.parentNode.removeChild(el); } catch (e) { break; } el = d.getElementById("ca79ui-notice"); }
  }
  function armVisible() {
    var d = root.document;
    if (visArmed || !d || !d.addEventListener) return;
    visArmed = true;
    d.addEventListener("visibilitychange", function () {
      if (hidden() || !activatePending) return;
      activatePending = false;
      activate();
    });
  }
  function activate() {
    emit("activate", {});
    if (activateTimer) { clearTimeout(activateTimer); activateTimer = null; }
    if (hidden()) { activatePending = true; armVisible(); emit("activate-deferred", {}); return; }
    activatePending = false;
    light(); notice("새 UI 시작 중 · starting the new UI…");
    setTimeout(function () { api.navigate(api.ngUrl()); }, 300);
    activateTimer = setTimeout(function () {
      activateTimer = null;
      unnotice();
      activateStalls++;
      if (activateStalls <= api.activateRetries) { activatePending = true; armVisible(); }
      emit("activate-stalled", { stalls: activateStalls });
    }, api.activateTimeoutMs);
  }
  // control: 1 activate (reload into the newest usable version) · 2 roll back (newest stored
  // slot → bad) · 3 forget both slots · 4 abort the reception · 5 clear the bad marks
  function onControl(sub) {
    var m = loadMeta(), s, best = null;
    if (sub === 1) { activate(); return 0; }
    if (sub === 2) {
      for (s in m.slots) if (m.slots.hasOwnProperty(s) && m.slots[s].state !== "bad" && (!best || m.slots[s].seq > m.slots[best].seq)) best = s;
      if (!best) return RC.noSlot;
      m.slots[best].state = "bad"; m.slots[best].why = "remote";
      return saveMeta(m) ? 0 : RC.storage;
    }
    if (sub === 3) { del(K_SLOT + "A"); del(K_SLOT + "B"); m.slots = {}; return saveMeta(m) ? 0 : RC.storage; }
    if (sub === 4) { rx = null; saveRx(); rxState = 0; return 0; }
    if (sub === 5) { for (s in m.slots) if (m.slots.hasOwnProperty(s) && m.slots[s].state === "bad") { m.slots[s].state = "new"; m.slots[s].tries = 0; delete m.slots[s].why; } return saveMeta(m) ? 0 : RC.storage; }
    return RC.args;
  }

  /** One SysEx from the shell. Returns the reply bytes, or null when the frame is not ours. */
  function handle(msg) {
    if (!msg || msg.length < 8 || msg[0] !== 0xf0 || msg[msg.length - 1] !== 0xf7) return null;
    for (var i = 0; i < 5; i++) if (msg[1 + i] !== TAG[i]) return null;
    var op = msg[6], d = new Uint8Array(msg.length - 8), k;
    if (op < 0x30 || op > 0x3f) return null;
    for (k = 0; k < d.length; k++) d[k] = msg[7 + k] & 0x7f;
    if (op === OP.status) return reply(op, pack7(statusRaw()));
    if (op === OP.begin) return reply(op, pack7(onBegin(unpack7(d))));
    if (op === OP.data) return reply(op, pack7(onData(d)));
    if (op === OP.commit) return reply(op, pack7(onCommit()));
    if (op === OP.control) return reply(op, [d[0] || 0, onControl(d[0] || 0)]);
    // the rest of the family: the screen reporter's ops (0x35 / 0x36, panelui/panelscreen.js) when it is on the
    // page, else swallowed without a reply
    var S = root.CA79Screen, r = null;
    try { r = S && S.handle ? S.handle(msg) : null; } catch (e) { r = null; }
    return r || [];
  }

  // ---------------------------------------------------------------- shell plumbing
  function host() {
    if (typeof root.kwmcore === "object" && root.kwmcore && root.kwmcore.postMessage) return root.kwmcore;
    var w = root.webkit; return w && w.messageHandlers && w.messageHandlers.kwmcore ? w.messageHandlers.kwmcore : null;
  }
  function sendMidi(bytes, id) {
    var h = host(), o = { msg: bytes };
    if (!h) return false;
    if (id) o.id = id;
    h.postMessage("kwmcore://sendMidiMessage/?" + JSON.stringify(o));
    return true;
  }
  /** Backlight on at the app's brightness (ca79ng.settings 1..10, else 8) through the shell's setLcdBacklight
   *  (Settings.System screen_brightness 0..100; 0 = off). The page being left may have turned it off (the app's
   *  auto display off) and that outlives the page: a version that does not set it itself (the remote probe) was
   *  drawn on a dark panel — field 2026-11-03 */
  function light() {
    var h = host(), b = 8, s;
    if (!h) return false;
    try { s = JSON.parse(get("ca79ng.settings") || "{}"); if (s && s.brightness >= 1 && s.brightness <= 10) b = Math.round(s.brightness); } catch (e) { /* default */ }
    try { h.postMessage("kwmcore://setLcdBacklight/?" + JSON.stringify({ brightness: b * 10 })); } catch (e) { return false; }
    return true;
  }
  /** Put our filter in front of KWM.onMidiMessage, whoever assigns it later (the app's
   *  installKwm uses Object.assign, which goes through the setter). */
  function hookKwm(K) {
    if (!K || K.__ca79ui) return K;
    var down = K.onMidiMessage;
    function filter(msg, id) {
      var r = null;
      try { r = handle(msg); } catch (e) { r = null; lastRc = 0x7f; }
      if (r) { if (r.length) sendMidi(r, id); return; }
      if (typeof down === "function") return down.apply(K, arguments);
    }
    try {
      Object.defineProperty(K, "onMidiMessage", { configurable: true, enumerable: true, get: function () { return filter; }, set: function (f) { if (f !== filter) down = f; } });
    } catch (e) { K.onMidiMessage = filter; }
    K.__ca79ui = true;
    return K;
  }

  // ---------------------------------------------------------------- boot
  /** A full-screen line on the page being left: the panel WebView keeps showing this page's last
   *  frame while the next page loads, verifies and starts (seconds on the panel, field 2026-11-02),
   *  so say what is going on instead of showing a UI that looks alive but takes no touch. */
  function notice(text) {
    var doc = root.document, el;
    try {
      if (!doc || !doc.body || !doc.createElement) return;
      el = doc.createElement("div");
      el.id = "ca79ui-notice";
      el.textContent = text;
      el.setAttribute("style", "position:fixed;left:0;top:0;right:0;bottom:0;z-index:2147483647;background:#000;color:#fff;" +
        "font:24px sans-serif;display:flex;align-items:center;justify-content:center;text-align:center;padding:24px");
      doc.body.appendChild(el);
    } catch (e) { /* cosmetic */ }
  }
  /** Taps made while the page was still loading (the panel shows the previous page's frame, the
   *  main thread is busy verifying and starting the UI) are queued by the WebView and land on the
   *  NEW page before its first frame — field 2026-11-02: the probe page was never seen [inference:
   *  a queued tap hit its "Roll back"]. Swallow pointer input until the first frame + inputGuardMs. */
  var GUARD_EVENTS = ["touchstart", "touchend", "pointerdown", "pointerup", "mousedown", "mouseup", "click"];
  function guardInput() {
    var on = true, i, opt = { capture: true, passive: false };
    if (!root.addEventListener) return;
    function swallow(e) {
      if (!on) return;
      try { if (e.cancelable && e.preventDefault) e.preventDefault(); if (e.stopImmediatePropagation) e.stopImmediatePropagation(); else if (e.stopPropagation) e.stopPropagation(); } catch (x) { /* ignore */ }
    }
    function release() {
      on = false;
      if (root.removeEventListener) for (i = 0; i < GUARD_EVENTS.length; i++) root.removeEventListener(GUARD_EVENTS[i], swallow, true);
    }
    for (i = 0; i < GUARD_EVENTS.length; i++) root.addEventListener(GUARD_EVENTS[i], swallow, opt);
    // queued input is dispatched before the next frame; the gesture → click of a queued tap may
    // follow a little later (the touch ack), hence the margin after the frame
    if (typeof root.requestAnimationFrame === "function") root.requestAnimationFrame(function () { setTimeout(release, api.inputGuardMs); });
    else setTimeout(release, api.inputGuardMs);
    api._guard = { active: function () { return on; }, release: release };
  }
  function query() {
    var q = {}, s = String(root.location && root.location.search || "").replace(/^\?/, "");
    s.split("&").forEach(function (p) { if (p) { var kv = p.split("="); q[decodeURIComponent(kv[0])] = decodeURIComponent(kv[1] || ""); } });
    return q;
  }
  /** Boot order: stored slots newer than the bundled app (seq, not bad), then the bundled app. */
  function candidates(m) {
    var list = [], s;
    for (s in m.slots) if (m.slots.hasOwnProperty(s) && m.slots[s].state !== "bad" && m.slots[s].seq > (CFG.bundled.seq >>> 0)) list.push(s);
    list.sort(function (a, b) { return m.slots[b].seq - m.slots[a].seq; });
    list.push("bundled");
    return list;
  }
  var guardTimer = null, failing = false;
  function fail(reason, detail) {
    if (failing) return;
    var m = loadMeta(), e = m.slots[running];
    if (reason !== "noconnect" && root.__ca79ngBooted) return; // errors after boot are the app's business
    failing = true;
    if (running === "A" || running === "B") {
      if (!(reason === "noconnect" && e && e.state === "ok")) { markBad(running, reason); api.navigate(api.ngUrl()); return; }
    }
    // the chooser shows what failed and on which WebView (no console on the panel)
    api.navigate(api.chooserUrl(reason) + "&detail=" + encodeURIComponent(String(detail || "").slice(0, 300)) +
      "&ua=" + encodeURIComponent(String((root.navigator && root.navigator.userAgent) || "")));
  }
  function inject(doc, files, bundled) {
    var head = doc.head || doc.getElementsByTagName("head")[0], el;
    // the bundled app sits inline as text (bundlecore): a script loaded from another file:// URL is a
    // different origin to Chrome and its errors arrive as a bare "Script error." (field 2026-11-01)
    var inl = bundled && doc.getElementById && doc.getElementById("ca79ng-app-js");
    if (inl) {
      el = doc.createElement("link"); el.rel = "stylesheet"; el.href = files.css; head.appendChild(el);
      el = doc.createElement("script"); el.text = inl.textContent;
      (doc.body || head).appendChild(el);
      return;
    }
    if (bundled) {
      el = doc.createElement("link"); el.rel = "stylesheet"; el.href = files.css; head.appendChild(el);
      el = doc.createElement("script"); el.src = files.js;
      el.onerror = function () { fail("load"); };
      (doc.body || head).appendChild(el);
      return;
    }
    el = doc.createElement("style"); el.textContent = files.css; head.appendChild(el);
    el = doc.createElement("script"); el.text = files.js;
    (doc.body || head).appendChild(el); // runs now; a throw reaches window.onerror → fail("error")
  }
  /** Called by ng/index.html. bundled = {js, css} file names of the app in this bundle. */
  function boot(bundled) {
    var doc = root.document, q = query(), m = loadMeta(), list = candidates(m), i, c, e, o;
    page = 1;
    if (q.src) list = [q.src];
    root.addEventListener("error", function (e) {
      var d = e && (e.message || "") + (e && e.filename ? " @" + String(e.filename).split("/").pop() + ":" + e.lineno + ":" + e.colno : "");
      setTimeout(function () { fail("error", d); }, 0);
    });
    guardTimer = setTimeout(function () { fail("timeout"); }, api.bootTimeoutMs);
    guardInput();
    light(); // every version starts on a lit panel, whatever the page before did to the backlight
    for (i = 0; i < list.length; i++) {
      c = list[i];
      if (c === "bundled") { running = "bundled"; inject(doc, bundled, true); emit("boot", { running: running }); return running; }
      e = m.slots[c];
      if (!e) continue;
      // a new (never confirmed) version gets MAX_TRIES boots to reach the piano once
      if (e.state !== "ok" && (e.tries || 0) >= MAX_TRIES) { markBad(c, "tries"); m = loadMeta(); continue; }
      e.tries = (e.tries || 0) + 1; saveMeta(m);
      o = openPackage(slotBytes(c) || u8(0));
      if (o.rc || o.h.seq !== e.seq) { markBad(c, "verify " + o.rc); m = loadMeta(); continue; }
      running = c; page = c === "A" ? 2 : 3;
      inject(doc, o.files, false);
      emit("boot", { running: running, version: o.h.version });
      return running;
    }
    api.navigate(api.chooserUrl("load"));
    return "none";
  }
  /** The app reached the piano: this version works (resets its boot tries). */
  function confirm() {
    if (running !== "A" && running !== "B") return;
    var m = loadMeta(), e = m.slots[running];
    if (!e) return;
    e.state = "ok"; e.tries = 0; delete e.why;
    saveMeta(m);
  }
  /** Bytes of localStorage this origin can still take (probe: grows a key until it throws). */
  function probeQuota(maxChars) {
    var s = ls(), chunk = "0123456789abcdef", lo = 0, hi = maxChars || 16 * 1024 * 1024, mid, ok;
    if (!s) return -1;
    while (chunk.length < 65536) chunk += chunk;
    function fill(n) { var parts = [], k = n; while (k > 0) { parts.push(chunk.substring(0, Math.min(k, chunk.length))); k -= chunk.length; } return parts.join(""); }
    while (hi - lo > 4096) {
      mid = Math.floor((lo + hi) / 2);
      try { s.setItem("ca79ng.ui.probe", fill(mid)); ok = true; } catch (e) { ok = false; }
      try { s.removeItem("ca79ng.ui.probe"); } catch (e) { /* ignore */ }
      if (ok) lo = mid; else hi = mid;
    }
    return lo; // characters (UTF-16 code units)
  }

  var api = {
    config: CFG, hookKwm: hookKwm, handle: handle, boot: boot, confirm: confirm, fail: fail,
    on: function (f) { listeners.push(f); },
    setPage: function (p) { page = p; },
    state: function () { var m = loadMeta(); return { refused: refused, running: running, rx: rxState, got: rx ? rx.got : 0, total: rx ? rx.h.length : 0, lastRc: lastRc, maxSeq: m.maxSeq, slots: m.slots, bundled: CFG.bundled, keyId: CFG.keyId }; },
    control: onControl, probeQuota: probeQuota, light: light,
    bootTimeoutMs: 15000, inputGuardMs: 500, activateTimeoutMs: 10000, activateRetries: 3,
    // this loader takes the activate notice down itself when the reload does not happen (the app's own watch,
    // src/ui/activatewatch.ts, is for older loaders only)
    activateWatch: 1,
    navigate: function (url) { root.location.replace(url); },
    // URLs relative to the page that loaded us (page 0 = chooser at appUI/, else appUI/ng/)
    ngUrl: function () { var e = query().embedded; return (page === 0 ? "ng/index.html" : "index.html") + (e ? "?embedded=" + e : ""); },
    chooserUrl: function (reason) { return (page === 0 ? "index.html" : "../index.html") + "?reason=" + reason; },
    // exposed for tests and the packer's cross-check
    _: { sha512: sha512, edVerify: edVerify, inflateRaw: inflateRaw, b64enc: b64enc, b64dec: b64dec, utf8dec: utf8dec, crc32: crc32, openPackage: openPackage, checkHead: checkHead, parseHeader: parseHeader, keyIdOf: keyIdOf, pack7: pack7, unpack7: unpack7, pickSlot: pickSlot }
  };
  restoreRx();
  root.CA79UI = api;
})(this);
