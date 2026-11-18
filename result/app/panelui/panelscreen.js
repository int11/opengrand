/* OpenGrand panel screen reporter — lets a phone / PC see what the built-in panel shows, with nobody at the piano.
 *
 *   REMOTE_PROTOCOL.md §10.7 ops 0x35 Capture / 0x36 Read (replies 0x75 / 0x76), relayed by the piano like the
 *   panel-UI update ops (cave relay_cave.c, ota_main feat2 0x04 — no firmware change).
 *
 *   Capture kind 1 = text report (UTF-8 JSON, bounded: page · tab · visible headings / labels / values / error
 *   banners · backlight · auto display off · UA / viewport · loader slots · last JS errors), kind 2 = snapshot
 *   (this page's own DOM painted on a canvas — no system screenshot — downscaled and JPEG-encoded to a byte
 *   target). The phone pulls the bytes with Read, window 2, go-back-N, like a UI package in reverse.
 *
 * ES5 (Chrome 71 panel WebView), no dependencies, no closing script tag. One copy runs per page: inlined in the
 * USB bundle (chooser, ng/index.html) and prepended to the app js and the probe page of remote packages, so a
 * remote package brings it to a panel whose USB bundle predates it. The newest VERSION on a page wins.
 * Install AFTER the UI loader hooked KWM (CA79UI.hookKwm): the filter here then sees the frames first. */
(function (root) {
  "use strict";
  var VERSION = 2; // 2: op 0x37 Act (owner 2026-11-15)
  if (!root || (root.CA79Screen && root.CA79Screen.version >= VERSION)) return;

  var TAG = [0x7d, 0x43, 0x41, 0x37, 0x39];
  var OP = { capture: 0x35, read: 0x36, act: 0x37 };
  var KIND = { report: 1, snapshot: 2 };
  var FMT = { json: 1, jpeg: 2, png: 3 };
  var RC = { ok: 0, args: 1, busy: 3, offset: 5, none: 8, unsupported: 13, internal: 0x7f };
  var CHUNK = 200; // raw bytes per Read reply: 8 B framing + pack7(6 + 200) = 244 B (relay limit 256 B)
  var REPORT_MAX = 1024, SHOT_W = 240, SHOT_KB = 24;

  function now() { return new Date().getTime(); }
  var t0 = now();

  // ---------------------------------------------------------------- bytes
  function le32(v) { return [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255]; }
  function le16(v) { v = Math.max(0, Math.min(65535, Math.round(v))); return [v & 255, v >> 8]; }
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
  var CRC_T = (function () {
    var t = [], n, c, k;
    for (n = 0; n < 256; n++) { c = n; for (k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
    return t;
  })();
  function crc32(b) { var c = 0xffffffff, i; for (i = 0; i < b.length; i++) c = CRC_T[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }
  function utf8(s) {
    var e = unescape(encodeURIComponent(s)), b = new Uint8Array(e.length), i;
    for (i = 0; i < e.length; i++) b[i] = e.charCodeAt(i);
    return b;
  }
  function utf8len(s) { return unescape(encodeURIComponent(s)).length; }
  function b64bytes(dataUrl) {
    var s = root.atob(dataUrl.substring(dataUrl.indexOf(",") + 1)), b = new Uint8Array(s.length), i;
    for (i = 0; i < s.length; i++) b[i] = s.charCodeAt(i);
    return b;
  }
  function squash(s) { return String(s == null ? "" : s).replace(/\s+/g, " ").replace(/^ | $/g, ""); }
  function cut(s, n) { s = String(s); return s.length > n ? s.substring(0, n - 1) + "…" : s; }

  // ---------------------------------------------------------------- shell
  function host() {
    var k = root.kwmcore, w = root.webkit;
    if (k && typeof k === "object" && k.postMessage) return k;
    return w && w.messageHandlers && w.messageHandlers.kwmcore ? w.messageHandlers.kwmcore : null;
  }
  function sendMidi(bytes, id) {
    var h = host(), o = { msg: bytes };
    if (!h) return false;
    if (id) o.id = id;
    h.postMessage("kwmcore://sendMidiMessage/?" + JSON.stringify(o));
    return true;
  }

  // ---------------------------------------------------------------- JS errors (last 5)
  var errors = [], watching = false;
  function noteError(msg) {
    errors.push({ t: now(), m: cut(squash(msg), 160) });
    if (errors.length > 5) errors.shift();
  }
  function watchErrors() {
    if (watching || !root.addEventListener) return;
    watching = true;
    root.addEventListener("error", function (e) {
      var m = e && e.message, tg = e && e.target;
      if (!m && tg && tg !== root && (tg.src || tg.href)) m = "load failed: " + String(tg.src || tg.href).split("/").pop();
      noteError((m || "error") + (e && e.filename ? " @" + String(e.filename).split("/").pop() + ":" + e.lineno + ":" + e.colno : ""));
    }, true);
    root.addEventListener("unhandledrejection", function (e) {
      var r = e && e.reason;
      noteError("unhandled rejection: " + (r && r.message ? r.message : String(r)));
    });
  }

  // ---------------------------------------------------------------- what is on the screen
  var providers = [];
  function doc() { return root.document || null; }
  function style(el, pseudo) { try { return root.getComputedStyle ? root.getComputedStyle(el, pseudo || null) : null; } catch (e) { return null; } }
  /** a layout engine is there (a real WebView; jsdom has none: every rect is 0) */
  function hasLayout() {
    var d = doc();
    if (!d || !d.documentElement || !d.documentElement.getBoundingClientRect) return false;
    var r = d.documentElement.getBoundingClientRect();
    return r.width > 0 || r.height > 0;
  }
  function viewport() { return { l: 0, t: 0, r: root.innerWidth || 0, b: root.innerHeight || 0 }; }
  function hit(r, c) { return r && c && r.right > c.l && r.bottom > c.t && r.left < c.r && r.top < c.b && (r.width > 0 || r.height > 0); }
  var SKIP = { SCRIPT: 1, STYLE: 1, TEMPLATE: 1, NOSCRIPT: 1, HEAD: 1, META: 1, LINK: 1, svg: 1, SVG: 1, TITLE: 1 };
  var ALERT = /(^|\s)(banner|error|err|warn|alert)(\s|$)/;
  function isOn(el) {
    var c = " " + (el.className && el.className.baseVal === undefined ? el.className : "") + " ";
    return / (on|active|selected|sel|def) /.test(c) || el.getAttribute("aria-selected") === "true" || el.getAttribute("aria-pressed") === "true";
  }
  function inputText(el) {
    var ty = String(el.type || "text").toLowerCase();
    if (ty === "checkbox" || ty === "radio") return (el.checked ? "[x]" : "[ ]");
    if (ty === "range") return "=" + el.value + (el.min !== "" || el.max !== "" ? " (" + (el.min || 0) + ".." + (el.max || 100) + ")" : "");
    if (ty === "file") return "<file" + (el.files && el.files.length ? ": " + el.files[0].name : "") + ">";
    if (ty === "hidden" || ty === "password") return "";
    return "=" + (el.value === "" && el.placeholder ? "(" + el.placeholder + ")" : el.value);
  }
  /**
   * Visible text of the page as lines, top to bottom as drawn: tokens (text runs, "[button]", "# heading", control
   * values) inside the viewport, grouped into lines by their vertical centre. Without a layout engine (tests), in
   * document order. `*` after a button = selected / on.
   */
  function screenText() {
    var d = doc(), L = hasLayout(), view = viewport(), toks = [], alerts = [];
    if (!d || !d.body) return { lines: [], alerts: [] };
    function rectOf(node) {
      if (!L) return null;
      try {
        if (node.nodeType === 3) { var rg = d.createRange(); rg.selectNodeContents(node); return rg.getBoundingClientRect(); }
        return node.getBoundingClientRect();
      } catch (e) { return null; }
    }
    function add(s, node, head) {
      s = squash(s);
      if (!s) return;
      var r = rectOf(node);
      if (L && !hit(r, view)) return;
      toks.push({ s: cut(s, 80), y: r ? r.top + r.height / 2 : toks.length, x: r ? r.left : 0, h: r ? r.height : 0, n: toks.length, head: head });
    }
    function walk(el) {
      var tag = el.tagName, cs, i, c, sel, cls;
      if (SKIP[tag] || el.hidden) return;
      cs = L ? style(el) : null;
      if (cs && (cs.display === "none" || cs.visibility === "hidden" || cs.opacity === "0")) return;
      cls = typeof el.className === "string" ? el.className : "";
      if ((el.getAttribute("role") === "alert" || ALERT.test(cls) || el.id === "why" || el.id === "ca79ui-notice") && (!L || hit(rectOf(el), view))) {
        c = squash(el.textContent);
        if (c) alerts.push(cut(c, 120));
      }
      if (/^H[1-6]$/.test(tag) || tag === "SUMMARY" || tag === "LEGEND") return add("# " + el.textContent, el, true);
      if (tag === "BUTTON" || el.getAttribute("role") === "tab") return add("[" + squash(el.textContent) + "]" + (isOn(el) ? "*" : "") + (el.disabled ? "~" : ""), el);
      if (tag === "INPUT") return add(inputText(el), el);
      if (tag === "SELECT") { sel = el.options && el.selectedIndex >= 0 ? el.options[el.selectedIndex] : null; return add("=" + (sel ? sel.text : el.value) + " ▾", el); }
      if (tag === "TEXTAREA") return add("=" + cut(el.value, 60), el);
      if (tag === "PROGRESS") return add("=" + (el.max ? Math.round((el.value / el.max) * 100) : 0) + "%", el);
      if (tag === "IMG") return add(el.alt ? "<img " + el.alt + ">" : "", el);
      for (i = 0; i < el.childNodes.length; i++) {
        c = el.childNodes[i];
        if (tag === "DETAILS" && !el.open && c.tagName !== "SUMMARY") continue;
        if (c.nodeType === 3) add(c.nodeValue, c);
        else if (c.nodeType === 1) walk(c);
      }
    }
    walk(d.body);
    var lines = [], cur = null, i, tk;
    if (L) toks.sort(function (a, b) { return a.y - b.y || a.x - b.x; });
    for (i = 0; i < toks.length; i++) {
      tk = toks[i];
      if (L && cur && Math.abs(tk.y - cur.y) <= Math.max(6, Math.min(tk.h, cur.h) / 2) && !tk.head && !cur.head) { cur.parts.push(tk); continue; }
      cur = { y: tk.y, h: tk.h, head: tk.head, parts: [tk] };
      lines.push(cur);
    }
    var out = [], s;
    for (i = 0; i < lines.length; i++) {
      lines[i].parts.sort(function (a, b) { return L ? a.x - b.x : a.n - b.n; });
      s = cut(lines[i].parts.map(function (p) { return p.s; }).join(" "), 100);
      if (s !== out[out.length - 1]) out.push(s);
    }
    var al = [];
    for (i = 0; i < alerts.length; i++) if (al.indexOf(alerts[i]) < 0) al.push(alerts[i]);
    return { lines: out, alerts: al.slice(0, 4) };
  }

  function loaderLine() {
    var U = root.CA79UI, s, k, e, parts = [];
    if (!U || !U.state) return "no loader";
    try { s = U.state(); } catch (x) { return "loader error"; }
    parts.push("run " + s.running);
    for (k in s.slots || {}) {
      if (!s.slots.hasOwnProperty(k) || !s.slots[k]) continue;
      e = s.slots[k];
      parts.push(k + " " + (e.version || "?") + " " + e.state + (e.tries ? " t" + e.tries : "") + (e.why ? " (" + e.why + ")" : ""));
    }
    if (s.bundled) parts.push("bundled " + (s.bundled.version || "?"));
    if (s.rx) parts.push("rx " + s.rx + " " + s.got + "/" + s.total);
    return parts.join(" · ");
  }
  function settings() { try { return JSON.parse(root.localStorage.getItem("ca79ng.settings") || "{}") || {}; } catch (e) { return {}; } }
  function shortUa() {
    var ua = String((root.navigator && root.navigator.userAgent) || ""), a = /Android [\d.]+/.exec(ua), c = /(Chrome|HeadlessChrome)\/[\d.]+/.exec(ua);
    return (a ? a[0] + " " : "") + (/; wv\)/.test(ua) ? "WebView " : "") + (c ? c[0] : cut(ua, 60));
  }
  function pageName() {
    var p = String((root.location && root.location.pathname) || "");
    return /\/ng\/[^/]*$/.test(p) ? "ng" : /\/index\.html$|\/$/.test(p) ? "chooser" : p.split("/").pop() || "?";
  }
  function query(k) {
    var m = new RegExp("[?&]" + k + "=([^&]*)").exec(String((root.location && root.location.search) || ""));
    try { return m ? decodeURIComponent(m[1]) : ""; } catch (e) { return m ? m[1] : ""; }
  }

  /**
   * The report object, fitted to `max` UTF-8 bytes of JSON: the screen's lines are dropped from the bottom first
   * (`cut` = how many), then old JS errors, then long fields are shortened. Providers (the app, the probe page)
   * add or override fields: pg page, tab, con connection phase, ver, bl backlight %, dim, off auto display off min.
   */
  function report(max) {
    max = Math.max(256, Math.min(8192, max || REPORT_MAX));
    var d = doc(), st = settings(), i, k, p, o = {
      v: 1, pg: pageName(), ver: "",
      bl: st.brightness >= 1 && st.brightness <= 10 ? Math.round(st.brightness) * 10 : null,
      off: typeof st.autoOffMin === "number" ? st.autoOffMin : null,
      dim: !!(d && d.documentElement && / display-off /.test(" " + d.documentElement.className + " "))
    };
    for (i = 0; i < providers.length; i++) {
      try { p = providers[i]() || {}; for (k in p) if (p.hasOwnProperty(k)) o[k] = p[k]; } catch (e) { o.perr = cut(String(e), 80); }
    }
    o.ld = loaderLine();
    if (query("reason")) o.why = cut(query("reason") + (query("detail") ? ": " + query("detail") : ""), 160);
    o.ua = shortUa();
    o.vp = (root.innerWidth || 0) + "x" + (root.innerHeight || 0) + "@" + (root.devicePixelRatio || 1) + (root.pageYOffset ? " y" + Math.round(root.pageYOffset) : "");
    o.up = Math.round((now() - t0) / 1000);
    o.title = d ? cut(d.title || "", 40) : "";
    var tx = screenText(), n = now();
    o.err = tx.alerts;
    o.js = errors.map(function (e) { return e.m + " (" + Math.round((n - e.t) / 1000) + "s ago)"; });
    o.t = tx.lines;
    var size = function () { return utf8len(JSON.stringify(o)); }, dropped = 0;
    while (size() > max && o.t.length) { o.t.pop(); o.cut = ++dropped; }
    while (size() > max && o.js.length > 1) o.js.shift();
    var shorten = ["why", "ld", "ua", "title", "perr"], j;
    for (j = 0; j < shorten.length && size() > max; j++) if (typeof o[shorten[j]] === "string") o[shorten[j]] = cut(o[shorten[j]], 24);
    while (size() > max && o.err.length) o.err.pop();
    while (size() > max && o.js.length) o.js.pop();
    return o;
  }

  // ---------------------------------------------------------------- snapshot (DOM → canvas)
  function alpha(c) {
    if (!c || c === "transparent") return 0;
    var m = /^rgba\(([^)]*)\)/.exec(c);
    return m ? parseFloat(m[1].split(",")[3]) : 1;
  }
  function px(v) { var n = parseFloat(v); return isNaN(n) ? 0 : n; }
  function inter(a, r) {
    var c = { l: Math.max(a.l, r.left), t: Math.max(a.t, r.top), r: Math.min(a.r, r.right), b: Math.min(a.b, r.bottom) };
    return c.r > c.l && c.b > c.t ? c : null;
  }
  function radius(cs, w, h) {
    var v = cs.borderTopLeftRadius || "0", n = px(v);
    if (/%$/.test(v)) n = (Math.min(w, h) * n) / 100;
    return Math.max(0, Math.min(n, w / 2, h / 2));
  }
  function rpath(g, x, y, w, h, r) {
    g.beginPath();
    if (r <= 0.5) { g.rect(x, y, w, h); return; }
    g.moveTo(x + r, y);
    g.lineTo(x + w - r, y); g.arc(x + w - r, y + r, r, -Math.PI / 2, 0);
    g.lineTo(x + w, y + h - r); g.arc(x + w - r, y + h - r, r, 0, Math.PI / 2);
    g.lineTo(x + r, y + h); g.arc(x + r, y + h - r, r, Math.PI / 2, Math.PI);
    g.lineTo(x, y + r); g.arc(x + r, y + r, r, Math.PI, Math.PI * 1.5);
    g.closePath();
  }
  function accent() {
    var d = doc(), v = "";
    try { v = String(style(d.documentElement).getPropertyValue("--accent") || "").replace(/^\s+|\s+$/g, ""); } catch (e) { /* default */ }
    return v || "#10818e";
  }
  function font(cs) { return (cs.fontStyle || "normal") + " " + (cs.fontWeight || "400") + " " + (cs.fontSize || "16px") + " " + (cs.fontFamily || "sans-serif"); }

  function paintBox(g, el, cs, r) {
    var rad = radius(cs, r.width, r.height), bg = cs.backgroundColor, bw = [px(cs.borderTopWidth), px(cs.borderRightWidth), px(cs.borderBottomWidth), px(cs.borderLeftWidth)];
    var bc = [cs.borderTopColor, cs.borderRightColor, cs.borderBottomColor, cs.borderLeftColor], st = [cs.borderTopStyle, cs.borderRightStyle, cs.borderBottomStyle, cs.borderLeftStyle], i;
    if (alpha(bg) > 0) { g.fillStyle = bg; rpath(g, r.left, r.top, r.width, r.height, rad); g.fill(); }
    var sh = /inset/.test(cs.boxShadow || "") ? /(rgba?\([^)]*\)|#[0-9a-fA-F]+)\s+(-?[\d.]+)px\s+(-?[\d.]+)px/.exec(cs.boxShadow) : null;
    if (sh && alpha(sh[1]) > 0) {
      var dx = parseFloat(sh[2]), dy = parseFloat(sh[3]);
      g.fillStyle = sh[1];
      if (dy > 0) g.fillRect(r.left, r.top, r.width, dy); else if (dy < 0) g.fillRect(r.left, r.bottom + dy, r.width, -dy);
      if (dx > 0) g.fillRect(r.left, r.top, dx, r.height); else if (dx < 0) g.fillRect(r.right + dx, r.top, -dx, r.height);
    }
    for (i = 0; i < 4; i++) if (st[i] === "none" || st[i] === "hidden" || alpha(bc[i]) === 0) bw[i] = 0;
    if (!(bw[0] || bw[1] || bw[2] || bw[3])) return;
    if (bw[0] === bw[1] && bw[1] === bw[2] && bw[2] === bw[3] && bc[0] === bc[1] && bc[1] === bc[2] && bc[2] === bc[3]) {
      g.lineWidth = bw[0]; g.strokeStyle = bc[0];
      rpath(g, r.left + bw[0] / 2, r.top + bw[0] / 2, r.width - bw[0], r.height - bw[0], Math.max(0, rad - bw[0] / 2));
      g.stroke();
      return;
    }
    if (bw[0]) { g.fillStyle = bc[0]; g.fillRect(r.left, r.top, r.width, bw[0]); }
    if (bw[1]) { g.fillStyle = bc[1]; g.fillRect(r.right - bw[1], r.top, bw[1], r.height); }
    if (bw[2]) { g.fillStyle = bc[2]; g.fillRect(r.left, r.bottom - bw[2], r.width, bw[2]); }
    if (bw[3]) { g.fillStyle = bc[3]; g.fillRect(r.left, r.top, bw[3], r.height); }
  }

  /** ::before / ::after with a box of its own (the switch knob, the status dot); gradients and transforms are skipped. */
  function paintPseudo(g, el, cs, r, which) {
    var p = style(el, which);
    if (!p || !p.content || p.content === "none" || p.content === "normal" || p.display === "none" || p.opacity === "0") return;
    var w = px(p.width), h = px(p.height), x, y, txt = /^["'].+["']$/.test(p.content) ? p.content.slice(1, -1) : "";
    if (p.position === "absolute" || p.position === "fixed") {
      x = p.left !== "auto" && p.left !== "" ? r.left + px(cs.borderLeftWidth) + px(p.left) : r.right - px(cs.borderRightWidth) - px(p.right) - w;
      y = p.top !== "auto" && p.top !== "" ? r.top + px(cs.borderTopWidth) + px(p.top) : r.bottom - px(cs.borderBottomWidth) - px(p.bottom) - h;
    } else {
      x = which === "::before" ? r.left + px(cs.borderLeftWidth) + px(cs.paddingLeft) + px(p.marginLeft) : r.right - px(cs.borderRightWidth) - px(cs.paddingRight) - w;
      y = r.top + (r.height - h) / 2;
    }
    if (w > 0 && h > 0 && alpha(p.backgroundColor) > 0 && (!p.transform || p.transform === "none")) {
      g.fillStyle = p.backgroundColor;
      rpath(g, x, y, w, h, radius(p, w, h));
      g.fill();
    }
    if (txt && txt !== " ") { g.font = font(p); g.fillStyle = p.color; g.textBaseline = "middle"; g.fillText(txt, x, r.top + r.height / 2); }
  }

  /** a text node where the page laid it out: one run when it sits on one line, else word by word (a Range per word;
   *  a word broken over lines — word-break: break-all — character by character). Nothing is re-wrapped by measuring. */
  function paintText(g, node, cs, clip) {
    var raw = node.nodeValue, d = doc(), rg, rs, R = [], i, k, m, w, r, re = /\S+/g;
    if (!raw || !/\S/.test(raw)) return;
    function tx(s) { return cs.textTransform === "uppercase" ? s.toUpperCase() : cs.textTransform === "lowercase" ? s.toLowerCase() : s; }
    function draw(s, rc) { if (s && rc && rc.width > 0.5 && hit(rc, clip)) g.fillText(tx(s), rc.left, rc.top + rc.height / 2, rc.width + 1); }
    try { rg = d.createRange(); rg.selectNodeContents(node); rs = rg.getClientRects(); } catch (e) { return; }
    for (i = 0; i < rs.length; i++) if (rs[i].width > 0.5 && rs[i].height > 0.5) R.push(rs[i]);
    if (!R.length) return;
    g.font = font(cs); g.fillStyle = cs.color; g.textBaseline = "middle"; g.textAlign = "left";
    m = /\S/.exec(raw);
    if (R.length === 1 && raw.substring(m.index).replace(/\s+$/, "").indexOf("\n") < 0) {
      // one line box: the run as one string, from its first visible character to its last
      rg.setStart(node, m.index); rg.setEnd(node, raw.replace(/\s+$/, "").length);
      return draw(/^pre/.test(cs.whiteSpace || "") ? raw.replace(/^\s+|\s+$/g, "") : squash(raw), rg.getBoundingClientRect());
    }
    while ((m = re.exec(raw))) {
      w = m[0];
      rg.setStart(node, m.index); rg.setEnd(node, m.index + w.length);
      rs = rg.getClientRects();
      if (rs.length <= 1) { draw(w, rs[0]); continue; }
      for (k = 0; k < w.length; k++) { rg.setStart(node, m.index + k); rg.setEnd(node, m.index + k + 1); r = rg.getBoundingClientRect(); draw(w.charAt(k), r); }
    }
  }

  function paintReplaced(g, el, cs, r, ctx) {
    var tag = el.tagName, ty = String(el.type || "").toLowerCase(), cx, cy, f, ac = ctx.accent;
    if (tag === "INPUT" && (ty === "checkbox" || ty === "radio")) {
      if (/none/.test(cs.webkitAppearance || cs.appearance || "")) return paintPseudo(g, el, cs, r, "::after");
      g.strokeStyle = cs.color; g.lineWidth = 1.5; rpath(g, r.left + 1, r.top + 1, r.width - 2, r.height - 2, ty === "radio" ? r.width / 2 : 2); g.stroke();
      if (el.checked) { g.fillStyle = ac; rpath(g, r.left + 3, r.top + 3, r.width - 6, r.height - 6, ty === "radio" ? r.width / 2 : 1); g.fill(); }
      return;
    }
    if (tag === "INPUT" && ty === "range") {
      var mn = px(el.min || 0), mx = el.max === "" ? 100 : px(el.max), fr = mx > mn ? (px(el.value) - mn) / (mx - mn) : 0, th = 11;
      cy = r.top + r.height / 2;
      g.fillStyle = "#474647"; rpath(g, r.left, cy - 3, r.width, 6, 3); g.fill();
      cx = r.left + th + fr * (r.width - 2 * th);
      g.fillStyle = ac; rpath(g, r.left, cy - 3, cx - r.left, 6, 3); g.fill();
      g.beginPath(); g.arc(cx, cy, th, 0, Math.PI * 2); g.fill(); g.lineWidth = 2; g.strokeStyle = cs.color; g.stroke();
      return;
    }
    if (tag === "PROGRESS") {
      f = el.max ? Math.max(0, Math.min(1, el.value / el.max)) : 0;
      g.fillStyle = ac; rpath(g, r.left, r.top, r.width * f, r.height, radius(cs, r.width * f, r.height)); g.fill();
      return;
    }
    if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") {
      var s = tag === "SELECT" ? (el.selectedIndex >= 0 && el.options[el.selectedIndex] ? el.options[el.selectedIndex].text : "") : ty === "password" ? "••••" : ty === "file" ? (el.files && el.files.length ? el.files[0].name : "Choose file") : el.value;
      var ph = !s && el.placeholder;
      if (ty === "file") { g.fillStyle = "#474647"; rpath(g, r.left, r.top + 2, Math.min(110, r.width), r.height - 4, 4); g.fill(); }
      g.save(); g.beginPath(); g.rect(r.left, r.top, r.width - (tag === "SELECT" ? 24 : 0), r.height); g.clip();
      g.font = font(cs); g.fillStyle = ph ? "#888" : cs.color; g.textBaseline = "middle";
      g.fillText(String(ph ? el.placeholder : s).replace(/\s+/g, " "), r.left + px(cs.borderLeftWidth) + px(cs.paddingLeft) + (ty === "file" ? 6 : 0), r.top + r.height / 2);
      g.restore();
      if (tag === "SELECT") { // the chevron of styles.css (a background image there)
        cx = r.right - 16; cy = r.top + r.height / 2;
        g.strokeStyle = "#a2a2a2"; g.lineWidth = 2; g.beginPath(); g.moveTo(cx - 5, cy - 2); g.lineTo(cx, cy + 3); g.lineTo(cx + 5, cy - 2); g.stroke();
      }
      return;
    }
    if ((tag === "IMG" || tag === "CANVAS") && ctx.images) {
      try { if (tag === "CANVAS" || (el.complete && el.naturalWidth)) g.drawImage(el, r.left, r.top, r.width, r.height); } catch (e) { /* broken image */ }
      return;
    }
    if ((tag === "svg" || tag === "SVG") && el.__ca79shot && el.__ca79shot.ok) {
      try { g.drawImage(el.__ca79shot, r.left, r.top, r.width, r.height); } catch (e) { /* not decodable */ }
      return;
    }
    if (tag === "VIDEO" || tag === "IFRAME" || tag === "OBJECT" || tag === "EMBED") { g.fillStyle = "#333"; g.fillRect(r.left, r.top, r.width, r.height); }
  }
  var REPLACED = { INPUT: 1, SELECT: 1, TEXTAREA: 1, PROGRESS: 1, IMG: 1, CANVAS: 1, svg: 1, SVG: 1, VIDEO: 1, IFRAME: 1, OBJECT: 1, EMBED: 1 };

  function paintEl(g, el, clip, ctx, top) {
    var cs = style(el), tag = el.tagName, i, c, r, op, c2, visible;
    if (!cs || cs.display === "none" || SKIP[tag] && tag !== "svg" && tag !== "SVG") return;
    if (!top && el !== doc().body && (cs.position === "fixed" || (tag === "DIALOG" && el.open))) { ctx.later.push(el); return; }
    op = parseFloat(cs.opacity);
    if (op === 0) return;
    r = el.getBoundingClientRect();
    visible = cs.visibility !== "hidden";
    g.save();
    if (op < 1) g.globalAlpha = g.globalAlpha * op;
    if (visible && hit(r, clip)) paintBox(g, el, cs, r);
    if (REPLACED[tag]) {
      if (visible && hit(r, clip)) paintReplaced(g, el, cs, r, ctx);
      g.restore();
      return;
    }
    if (visible && hit(r, clip)) paintPseudo(g, el, cs, r, "::before");
    c2 = clip;
    if ((cs.overflowX && cs.overflowX !== "visible") || (cs.overflowY && cs.overflowY !== "visible")) {
      c2 = inter(clip, r);
      if (!c2) { g.restore(); return; }
      g.beginPath(); g.rect(c2.l, c2.t, c2.r - c2.l, c2.b - c2.t); g.clip();
    }
    for (i = 0; i < el.childNodes.length; i++) {
      c = el.childNodes[i];
      if (tag === "DETAILS" && !el.open && c.tagName !== "SUMMARY") continue;
      if (c.nodeType === 3) { if (visible) paintText(g, c, cs, c2); } else if (c.nodeType === 1) paintEl(g, c, c2, ctx, false);
    }
    if (visible && hit(r, clip)) paintPseudo(g, el, cs, r, "::after");
    g.restore();
  }

  /** an inline SVG (the tab icons) as an image: computed paint inlined, since the page's CSS does not reach it */
  var SVG_PROPS = ["fill", "stroke", "stroke-width", "stroke-linecap", "stroke-linejoin", "fill-opacity", "stroke-opacity", "opacity", "display"];
  function svgUrl(svg, r) {
    var cl = svg.cloneNode(true), a = [svg].concat([].slice.call(svg.getElementsByTagName("*"))), b = [cl].concat([].slice.call(cl.getElementsByTagName("*"))), i, k, cs, s;
    for (i = 0; i < a.length && i < b.length; i++) {
      cs = style(a[i]); s = "";
      if (cs) for (k = 0; k < SVG_PROPS.length; k++) if (cs.getPropertyValue(SVG_PROPS[k])) s += SVG_PROPS[k] + ":" + cs.getPropertyValue(SVG_PROPS[k]) + ";";
      b[i].setAttribute("style", s);
    }
    cl.setAttribute("xmlns", "http://www.w3.org/2000/svg");
    cl.setAttribute("width", String(Math.max(1, Math.round(r.width))));
    cl.setAttribute("height", String(Math.max(1, Math.round(r.height))));
    return "data:image/svg+xml;charset=utf-8," + encodeURIComponent(new root.XMLSerializer().serializeToString(cl));
  }
  function loadSvgs(view, done) {
    var d = doc(), list = [], all = d.getElementsByTagName("svg"), left = 0, fired = false, i, r, im;
    function fin() { if (!fired) { fired = true; done(list); } }
    for (i = 0; i < all.length && i < 64; i++) {
      r = all[i].getBoundingClientRect();
      if (!hit(r, view) || !root.Image || !root.XMLSerializer) continue;
      try { im = new root.Image(); im.src = svgUrl(all[i], r); } catch (e) { continue; }
      all[i].__ca79shot = im; list.push(all[i]); left++;
      im.onload = (function (m) { return function () { m.ok = true; if (--left === 0) fin(); }; })(im);
      im.onerror = function () { if (--left === 0) fin(); };
    }
    if (!left) fin(); else setTimeout(fin, 1500);
  }

  /** cb(rc, {bytes, fmt, w, h, ms, q}) — the viewport as drawn, `o.w` wide, JPEG at the quality that fits `o.kb` KiB */
  function snapshot(o, cb) {
    o = o || {};
    var d = doc(), vw = root.innerWidth || 0, vh = root.innerHeight || 0, W = Math.max(48, Math.min(480, o.w || SHOT_W)), T = Math.max(4, o.kb || SHOT_KB) * 1024, t1 = now(), cv, g;
    try { cv = d && d.createElement("canvas"); g = cv && cv.getContext ? cv.getContext("2d") : null; } catch (e) { g = null; }
    if (!g || !vw || !vh || !hasLayout()) return cb(RC.unsupported, null);
    var s = W / vw, H = Math.max(1, Math.round(vh * s)), view = viewport();
    cv.width = W; cv.height = H;
    loadSvgs(view, function (svgs) {
      var ctx = { later: [], images: !o.noImages, accent: accent() }, i, bytes = null, url, q = 0.7, fmt = FMT.jpeg;
      try {
        g.save(); g.scale(s, s);
        var hc = style(d.documentElement), bc = style(d.body);
        g.fillStyle = hc && alpha(hc.backgroundColor) > 0 ? hc.backgroundColor : bc && alpha(bc.backgroundColor) > 0 ? bc.backgroundColor : "#fff";
        g.fillRect(0, 0, vw, vh);
        paintEl(g, d.body, view, ctx, true);
        ctx.later.sort(function (a, b) { return (parseInt(style(a).zIndex, 10) || 0) - (parseInt(style(b).zIndex, 10) || 0); });
        for (i = 0; i < ctx.later.length; i++) {
          if (ctx.later[i].tagName === "DIALOG") { g.fillStyle = "rgba(0,0,0,0.4)"; g.fillRect(0, 0, vw, vh); }
          paintEl(g, ctx.later[i], view, ctx, true);
        }
        g.restore();
        if (/ display-off /.test(" " + d.documentElement.className + " ")) { // the app dimmed the backlight: say so on the picture
          g.fillStyle = "#e33"; g.font = "bold 12px sans-serif"; g.textBaseline = "top"; g.fillText("display off (backlight 0)", 4, 4);
        }
      } catch (e) {
        noteError("snapshot: " + (e && e.message ? e.message : e));
      }
      for (i = 0; i < svgs.length; i++) try { delete svgs[i].__ca79shot; } catch (e) { svgs[i].__ca79shot = null; }
      for (i = 0; i < 6; i++) {
        try { url = cv.toDataURL("image/jpeg", q); } catch (e) { url = null; }
        if (!url) break; // tainted by an image: drawn again without images below
        if (url.indexOf("data:image/jpeg") !== 0) { fmt = FMT.png; bytes = b64bytes(url); break; }
        bytes = b64bytes(url);
        if (bytes.length <= T || q <= 0.2) break;
        q = Math.max(0.2, Math.min(q - 0.1, q * Math.sqrt(T / bytes.length)));
      }
      if (!bytes && ctx.images) { o.noImages = true; return snapshot(o, cb); }
      if (!bytes) return cb(RC.internal, null);
      cb(RC.ok, { bytes: bytes, fmt: fmt, w: W, h: H, ms: now() - t1, q: q });
    });
  }

  // ---------------------------------------------------------------- ops 0x35 / 0x36
  var cap = null; // { tag, kind, busy, rc, bytes, fmt, w, h, ms, crc }
  function reply(op, raw) { return [0xf0].concat(TAG, [op | 0x40], pack7(raw), [0xf7]); }
  function capReply(c) {
    var len = c.bytes ? c.bytes.length : 0;
    return reply(OP.capture, [c.busy ? RC.busy : c.rc, c.tag, c.kind, c.fmt || 0].concat(le32(len), le32(c.crc || 0), le16(c.w || 0), le16(c.h || 0), le16(c.ms || 0)));
  }
  function onCapture(d) {
    var kind = d[0], tag = d[1], p1 = d[2] || 0, p2 = d[3] || 0, c, t1 = now();
    if (d.length < 2 || (kind !== KIND.report && kind !== KIND.snapshot)) return reply(OP.capture, [RC.args, tag || 0, kind || 0, 0]);
    if (cap && cap.tag === tag && cap.kind === kind) return capReply(cap); // a repeated ask (lost reply / polling): same capture
    c = cap = { tag: tag, kind: kind, busy: false, rc: RC.ok, bytes: null, fmt: 0, w: 0, h: 0, ms: 0, crc: 0 };
    if (kind === KIND.report) {
      try { c.bytes = utf8(JSON.stringify(report(p1 ? p1 * 64 : REPORT_MAX))); } catch (e) { c.rc = RC.internal; noteError("report: " + e); }
      c.fmt = FMT.json; c.ms = now() - t1;
      if (c.bytes) c.crc = crc32(c.bytes);
      return capReply(c);
    }
    c.busy = true;
    var done = false;
    // drawn after this reply has gone out: the shell call returns at once, the phone polls (rc 3 = busy)
    setTimeout(function () {
      try {
        api.render({ w: p1 ? p1 * 8 : SHOT_W, kb: p2 || SHOT_KB }, function (rc, res) {
          if (done) return;
          done = true;
          c.busy = false; c.rc = rc;
          if (res) { c.bytes = res.bytes; c.fmt = res.fmt; c.w = res.w; c.h = res.h; c.ms = res.ms; c.crc = crc32(res.bytes); }
        });
      } catch (e) { done = true; c.busy = false; c.rc = RC.internal; noteError("snapshot: " + e); }
    }, 0);
    return capReply(c);
  }
  function onRead(d) {
    var tag = d[0], off = ((d[1] & 0x7f) << 14) | ((d[2] & 0x7f) << 7) | (d[3] & 0x7f), n = d[4] || CHUNK, len;
    if (d.length < 4) return reply(OP.read, [RC.args, tag || 0].concat(le32(0)));
    if (!cap || cap.tag !== tag) return reply(OP.read, [RC.none, tag].concat(le32(off)));
    if (cap.busy) return reply(OP.read, [RC.busy, tag].concat(le32(off)));
    len = cap.bytes ? cap.bytes.length : 0;
    if (off > len) return reply(OP.read, [RC.offset, tag].concat(le32(off)));
    n = Math.min(CHUNK, n, len - off);
    return reply(OP.read, [RC.ok, tag].concat(le32(off), [].slice.call(cap.bytes.subarray(off, off + n))));
  }
  /**
   * op 0x37 Act (lets an automated test press panel buttons): `37 <tag> <action>
   * <id ASCII> [00 <value ASCII>]`, action 1 = click, 2 = set value (input / select, then input + change events).
   * Only an element id (letters, digits, - _) on the page shown now. Reply `77 <rc> <tag> <action>` (plain 7-bit):
   * rc 0 done · 1 bad args · 4 no such element.
   */
  function later(f) {
    if (typeof Promise === "function") Promise.resolve().then(f); else setTimeout(f, 0);
  }
  function onAct(d) {
    var tag = d[0] || 0, action = d[1] || 0, rest = d.slice(2), z = rest.indexOf(0), id, val, el;
    var str = function (a) { var o = ""; for (var i = 0; i < a.length; i++) o += String.fromCharCode(a[i]); return o; };
    id = str(z < 0 ? rest : rest.slice(0, z)); val = z < 0 ? "" : str(rest.slice(z + 1));
    var out = function (rc) { return [0xf0].concat(TAG, [OP.act | 0x40, rc, tag, action, 0xf7]); };
    if ((action !== 1 && action !== 2) || !/^[A-Za-z0-9_-]{1,40}$/.test(id)) return out(RC.args);
    el = document.getElementById(id);
    if (!el) return out(4);
    // after the reply (the filter sends it when this returns) — as a microtask, not a timer: a WebView whose activity
    // is in the background runs no timers, and the act answered rc 0 but never clicked (field 2026-11-15, record
    // 04_panel_ui_remote last lines [inference])
    later(function () {
      try {
        if (action === 1) el.click();
        else {
          el.value = val;
          var ev = function (n) { var e = document.createEvent("Event"); e.initEvent(n, true, true); el.dispatchEvent(e); };
          ev("input"); ev("change");
        }
      } catch (e) { noteError("act: " + e); }
    });
    return out(RC.ok);
  }
  /** One SysEx: the reply bytes for ops 0x35 / 0x36 / 0x37, else null (not ours: the next filter gets it). */
  function handle(msg) {
    if (!msg || msg.length < 8 || msg[0] !== 0xf0 || msg[msg.length - 1] !== 0xf7) return null;
    for (var i = 0; i < 5; i++) if (msg[1 + i] !== TAG[i]) return null;
    var op = msg[6], d = [], k;
    if (op !== OP.capture && op !== OP.read && op !== OP.act) return null;
    for (k = 7; k < msg.length - 1; k++) d.push(msg[k] & 0x7f);
    return op === OP.capture ? onCapture(d) : op === OP.act ? onAct(d) : onRead(d);
  }

  /**
   * Put the 0x35 / 0x36 filter in front of KWM.onMidiMessage (after CA79UI.hookKwm, whose filter would swallow
   * them otherwise — the loader keeps ops 0x35-0x3F to itself). Assignments (the app's installKwm) still reach the
   * handler below. A second, newer copy only swaps the handler (K.__ca79scr) — never a second wrapper.
   */
  function install(K) {
    K = K || root.KWM;
    watchErrors();
    if (!K) return null;
    if (K.__ca79scr) {
      if (!(K.__ca79scr.v >= VERSION)) { K.__ca79scr.handle = handle; K.__ca79scr.v = VERSION; }
      return K;
    }
    var slot = { handle: handle, v: VERSION }, dsc = Object.getOwnPropertyDescriptor(K, "onMidiMessage"), inner = dsc && !dsc.get ? dsc.value : null;
    var down = dsc && dsc.get ? function () { return dsc.get.call(K); } : function () { return inner; };
    var setDown = dsc && dsc.set ? function (f) { dsc.set.call(K, f); } : function (f) { inner = f; };
    function front(msg, id) {
      var r = null, f;
      try { r = slot.handle(msg, id); } catch (e) { r = null; noteError("screen op: " + e); }
      if (r) { sendMidi(r, id); return; }
      f = down();
      if (typeof f === "function" && f !== front) return f.apply(K, arguments);
    }
    try {
      Object.defineProperty(K, "onMidiMessage", { configurable: true, enumerable: true, get: function () { return front; }, set: function (f) { if (f !== front) setDown(f); } });
      Object.defineProperty(K, "__ca79scr", { configurable: true, enumerable: false, value: slot });
    } catch (e) { inner = K.onMidiMessage; K.onMidiMessage = front; K.__ca79scr = slot; }
    return K;
  }

  var api = {
    version: VERSION,
    install: install, handle: handle,
    /** fn() → fields merged into the report (later providers win) */
    provide: function (fn) { if (typeof fn === "function") providers.push(fn); },
    report: report,
    /** the text lines and banners of the report alone */
    text: screenText,
    snapshot: snapshot,
    /** the renderer used by Capture kind 2 (tests swap it) */
    render: function (o, cb) { snapshot(o, cb); },
    errors: function () { return errors.slice(); },
    noteError: noteError,
    _: { pack7: pack7, crc32: crc32, utf8: utf8, state: function () { return cap; } }
  };
  // a copy loaded earlier keeps its providers (the page registered them on it)
  if (root.CA79Screen && root.CA79Screen._providers) providers = root.CA79Screen._providers;
  api._providers = providers;
  root.CA79Screen = api;
})(typeof window !== "undefined" ? window : this);
