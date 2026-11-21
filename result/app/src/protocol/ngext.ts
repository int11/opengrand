// OpenGrand extension SysEx (manufacturer ID 0x7D) — see result/protocol/REMOTE_PROTOCOL.md §10.
//
// Stock V2.19 drops 0x7D messages without a reply, a NAK, forwarding or any state
// change. So a reply to Identify *is*
// the "custom firmware is running" signal: on a stock piano the request times out.
//
//   request  F0 7D 43 41 37 39 <op> [args] F7
//   reply    F0 7D 43 41 37 39 <op|0x40> [data] F7
//
// One module per concern under protocol/ngext/; this file is the entry everything imports.
export * from "./ngext/wire.ts";
export * from "./ngext/decode.ts";
export * from "./ngver.gen.ts";
export * from "./ngext/client.ts";
export * from "./ngext/power.ts";
export * from "./ngext/startup.ts";
export * from "./ngext/recorder.ts";
export * from "./ngext/gpio.ts";
export * from "./ngext/canary.ts";
