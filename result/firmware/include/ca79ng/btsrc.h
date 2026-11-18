/* OpenGrand - Bluetooth audio source manager.
 *
 * The BT audio module (Bluegiga WT32i, iWRAP 6.2.0, A2DP sink + AVRCP controller) holds several sources at once but
 * plays one stream only (AN986 3.5.1: "only one of the connections can be streaming audio at a time"); nothing in the
 * stock firmware chooses between them (the stock sends no runtime command at all).  This module keeps a table of the
 * connected source devices from the module's own lines and - with RUNTIME iWRAP commands - makes the source the
 * user SELECTED the one that plays:
 *   - lines in:  RING <l> <addr> <ch> <profile>, CONNECT <l> <profile> <ch>, CALL <l>, NO CARRIER <l> ...,
 *                A2DP STREAMING START|STOP [<l>], A2DP CODEC <codec> <mode> <rate> ..., the iWRAP banner (`WRAP …`) /
 *                READY., the LIST reply, the `SET` listing (AUTOCALL state, the class of device, the AVRCP
 *                profile features and the audio routing line only), `AUDIO ROUTE <l> <type> <channels>` (RM 6.67 -
 *                printed only with the module's `SET CONTROL AUDIO … EVENT` flag: kept as the last routed link, no
 *                stream event);
 *   - commands out (the only ones it can form): `LIST`, `SET` (list the configuration - a read), `CLOSE <l>`,
 *     `CALL <addr> 19 A2DP`, `CALL <addr> 17 AVRCP`, the pairs `SET <l> SELECT` + `AVRCP PAUSE|PLAY` /
 *     `AVRCP PDU 31 1` (register the PLAYBACK_STATUS_CHANGED notification, RM 6.10 / AN986 6.3 - a query) /
 *     `A2DP STREAMING START` (RM 6.98: SET {link_id} SELECT picks the link the next command goes to - run-time,
 *     nothing is stored), `A2DP STREAMING STOP` (the sink stops a stream - the opt-in late stop only; host model:
 *     the old suspend, susp_ok), and ONLY from the explicit AUTOCALL op (bts_autocall): `SET CONTROL AUTOCALL` (RM 6.68 "Disable") /
 *     the line read before (restore), and ONLY from the explicit class-of-device op (bts_class): `SET BT CLASS <hex6>`
 *     (an audio sink class, default 240414) / the class read before (restore), and ONLY from the explicit AVRCP
 *     features op (bts_avrcp_features): `SET PROFILE AVRCP CONTROLLER <hex2>` (browsing bit 4 clear, default 0f) /
 *     the features read before (restore), and ONLY from the explicit audio routing op (bts_audio): `SET CONTROL AUDIO
 *     INTERNAL I2S_SLAVE EVENT` / `… EVENT KEEPALIVE` / the flags read before (restore) - and the power-on pin of the decided values through those three ops.  Host tests and the emulator round trip check the forms.  `CLOSE` only from the
 *     user's explicit acts (1D 05 act 0 / 3 / 6), the opt-in legacy mode NEWEST and (field 2026-11-09) a lone AVRCP
 *     browsing channel (`RING <l> <addr> 1b AVRCP` with no control link - the module rebooted after it): `CLOSE <l>` of
 *     that one link at once, mode SELECT, not with the opt-out flag 0x20, and our own `CALL … 17 AVRCP` still without
 *     CONNECT BTS_AVRCP_GIVEUP_MS after its `CALL <l>` answer (`CLOSE <l>` of that link id, field 2026-11-09) - never as part of a switch or the
 *     guard (field 2026-11-03: closing a source's A2DP dropped the phone's AVRCP and removed
 *     the Windows endpoint - Chrome's audio renderer broke until Chrome was restarted and Windows reconnected).
 *   - every module line and every command sent is kept in a line log (ring, read by op 1D 08; pairing keys of the
 *     `SET BT PAIR` lines are cut off).
 *
 * Selection (default mode SELECT, user requirement 2026-11-02 "다중연결해놓고 내가 기기 선택해서"):
 *   Every source keeps its A2DP + AVRCP links through a switch.  USE (「이 기기로 듣기」) runs a switch - ONE way since
 *   the experimental strategies were dropped (WAIT / HOLD / DIRECT / SELECT removed; op 1D 06 byte
 *   13 accepts 0 only): the source that plays gets AVRCP PAUSE (play-status rules below), then the switch waits for its
 *   own STOP (BTS_SW_HELD_MS, BUSY after; the opt-in late stop below may stop it).  Host model only (susp_ok, compiled
 *   out of the cave): no AVRCP -> suspended at once; still streaming BTS_SW_PAUSE_MS later -> the sink suspends it
 *   (`SET <its A2DP link> SELECT` + `A2DP STREAMING STOP`, AN986 6.1) -> gone within BTS_SW_SUSPEND_MS, else BUSY;
 *   a source that reports paused and still streams is suspended too (field 2026-11-09 evening - Windows silent on
 *   a stream the sink resumed - is met on the way back: PLAY first).  "Reports paused" = a report newer than its
 *   stream's own START (ps_old) - a stream started after the report is taken for playing: PAUSE.  A switch that ended
 *   PLAYING: another source's STREAMING STOP within BTS_SW_REASSERT_MS -> the sink's START on the target once more - the
 *   left phone's own late STOP muted the piano, field 2026-11-09 evening.
 *   -> the chosen device has no A2DP link: `CALL <addr> 19 A2DP` -> AVRCP PLAY BTS_SW_SETTLE_MS after its A2DP link
 *   (a fresh link without AVRCP: up to BTS_SW_FRESH_MS for its AVRCP to come back or its own start; the sink opens that
 *   AVRCP BTS_SW_AVRCP_MS after the link and the switch waits for it - up to BTS_SW_AVRCP_WAIT_MS) -> no stream within
 *   BTS_SW_READY_MS: "ready" (press play on it; its START within BTS_SW_LATE_MS after: "playing") - the sink starts it (`A2DP STREAMING START`) only when it reports
 *   PLAYING (its player plays, the module holds no stream of it), none within BTS_SW_START_MS: "ready".  NEVER a sink
 *   START to a source without an AVRCP control link (field 2026-11-10 night: the paused
 *   Windows PC (AVRCP off) answered the sink's START with its stream, and that stream stayed silent when it played -
 *   only a Windows PnP restart helped): "ready" at once, its own play opens its stream.  A target the sink suspended earlier: with AVRCP the PLAY decision first (its player restarts
 *   its own stream - field 2026-11-09: Windows put no sound on a stream the sink resumed), the sink's START after
 *   BTS_SW_PLAY_MS / without AVRCP.
 *   Its stream up is NOT "it sounds" (field 2026-11-02 afternoon: Windows streams silence on a fresh link while its
 *   player is paused): once the stream is up, AVRCP PLAY goes to it unless it already went on this link (no AVRCP:
 *   nothing more to send).  Result PLAYING = stream up (+ PLAY sent, sw_tried).
 *   PLAY only to a source that reports paused / stopped - Windows takes the PLAY passthrough as play/pause (field
 *   2026-11-02 night: PLAY to a playing PC paused it); no play status: only to one the piano paused itself (ps_ours).
 *   PAUSE not to one that reports paused (its open stream would start again).
 *   A stream of the target with no AVRCP to send PLAY on is not taken for "playing" while that AVRCP may still come
 *   (the switch waits for it, BTS_SW_AVRCP_WAIT_MS) and not before it lasted BTS_SW_STEADY_MS (field 2026-11-03: a
 *   paused phone's fresh link streamed 10 ms).  "Paused by the piano" (ps_ours) lives in the device entry - by address,
 *   through its links going and coming back - until it plays, the user picks another device or it is gone (no link,
 *   not parked).  Safety net: the switch ended without PLAY and the target's AVRCP comes within BTS_SW_LATE_MS reporting
 *   paused / stopped, paused by the piano, still selected -> PLAY once (BTS_SWT_LATE).
 *   Progress / result for the app: sw_phase / sw_result / sw_tried.
 * Play status: every AVRCP control link gets `AVRCP PDU 31 1` once (the INTERIM answer = the status now, a CHANGED =
 *   the new one, then registered again); REJ / no answer within BTS_PS_ANSWER_MS: not again on that link.
 *   GUARD: a source that is not the selected one starts streaming while the selected one is connected -> AVRCP
 *   PAUSE (host model, susp_ok: no AVRCP, or its 3rd start within 30 s - suspended at once; still streaming
 *   BTS_SW_PAUSE_MS later - suspended).  Never closed.  FOLLOW flag:
 *   it becomes the selected one instead.  Nothing selected yet / the selected one not connected: the first source that
 *   streams is adopted.
 *   AVRCP: a source with A2DP and no AVRCP control link gets one from the sink (`CALL <addr> 17 AVRCP`, once per A2DP
 *   episode) only after it had BTS_AVRCP_CALL_MS (4 s) to open its own - the switch target BTS_SW_AVRCP_MS, a suspect
 *   never; a CALL with no CONNECT within BTS_AVRCP_GIVEUP_MS is closed (`CLOSE <l>` of its `CALL <l>` id, RM 6.18) - a
 *   PC that got our early CALL left it unanswered and opened a lone browsing channel 11 s later, the module rebooted;
 *   left alone it opened its control channel itself 0.5 s after its A2DP (field 2026-11-09).  Module reboots (banner / READY. while powered) are counted; two within 60 s = the loop guard (no
 *   manager-initiated CALL for 5 min after the last one; nothing written to the module).
 * AUTOCALL (module setting, stored in the module): read from every `SET` listing.  Known OFF -> this module owns
 *   reconnecting, as the policy's reconnect choice says (policy recon, user decision 2026-11-09):
 *   BTS_RC_PREF_LAST (default) CALL the preferred device, else the last selected one; BTS_RC_PREF the preferred
 *   one only; BTS_RC_OFF none - when no A2DP link is up, backoff 4/8/15/30/60 s, at most BTS_RC_TRIES per episode,
 *   never while any source has A2DP (no ping-pong), never in standby.  A module power edge (power-on, and the wake from a
 *   standby that switched the module off) starts the same schedule from 0 - the selected device (RAM) survives the
 *   standby, so the wake calls as a power-on does (user decision 2026-11-14).  An episode ends only when an A2DP link stays
 *   BTS_RC_STABLE_MS; one the source closes sooner is a failed try (field 2026-11-09).  The selected device that closes
 *   its own A2DP (`NO CARRIER … ERROR 0`, not our CLOSE) after a normal connection was disconnected by the user there:
 *   it is not called as "the last selected" again until it is selected again or connects by itself and stays (the
 *   preferred device is still called, with the backoff).  Changing AUTOCALL is the explicit op only (bts_autocall: off / restore the exact line read before,
 *   else the stock factory line `SET CONTROL AUTOCALL 19 5000 A2DP`, REA 0x4013EE0C), never automatic.
 * CLASS OF DEVICE (module setting `SET BT CLASS <hex>`, stored in the module; field 2026-11-09):
 *   read from every `SET` listing.  The module listed `001f00` (major "uncategorized", no service bit) and an
 *   Android TV's pairing list (A2DP device criteria, BluetoothClass.doesClassMatch(PROFILE_A2DP): the Rendering
 *   service bit or an audio major class) dropped it.  The stock never writes it (no `CLASS` byte in CAx9_P06.SYS -
 *   REA).  Changing it: the explicit op (bts_class): write an audio sink class (Audio/Video major 0x04 +
 *   Rendering service bit, default BTS_COD_AUDIO_SINK) / restore the class read before this power-on (the first one
 *   read that is not such a class), else BTS_COD_IWRAP (the value the module listed live), the manager writes BTS_PIN_COD itself at power-on.
 * AVRCP PROFILE FEATURES (module setting `SET PROFILE AVRCP CONTROLLER <features>`, stored in the module; field
 *   2026-11-10): read from every `SET` listing.  AN986 3.2 / RN 6.5.3: bits 0-3 = AVRCP
 *   categories 1-4, bit 4 = browsing, broadcast in the SDP record.  The module listed `1f` (the stock factory line,
 *   REA 0x4013EDE8) and a Windows PC sometimes opened the browsing channel (PSM 0x1b) with no control channel - the
 *   module printed `AVRCP 5103 ERROR_NO_CONTROL_CHANNEL` and rebooted, even with the piano sending nothing.  Changing
 *   it: the explicit op (bts_avrcp_features): write controller features without the browsing bit (default
 *   BTS_AVF_NO_BROWSE) / restore the features read before this power-on (the first value read with the browsing
 *   bit), else BTS_AVF_FACTORY; only while the listed line is controller-only (a TARGET part is never rewritten); the
 *   module applies a profile change after its restart (RM 6.99 note) - the op does not restart it, the manager writes BTS_PIN_AVF + one restart at power-on.
 * AUDIO ROUTING (module setting `SET CONTROL AUDIO {sco} {a2dp} [EVENT] [KEEPALIVE] [AAx] [i2s_bits]`, RM 6.67, stored
 *   in the module; field 2026-11-11): read from every `SET` listing.  The stock factory line
 *   is `SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE` (REA 0x4013EE30, sent by FUN_4013E740); KEEPALIVE "prevents
 *   DSP from powering down between A2DP streams", EVENT prints `AUDIO ROUTE …` / `A2DP CODEC …`.  The phone (aptX) ->
 *   PC (SBC) switch stayed silent (H1 / H2).  Changing it: the explicit op (bts_audio): write one
 *   of two fixed lines (BTS_AUD_EVENT = KEEPALIVE off + EVENT, or EVENT + KEEPALIVE) / restore the flags read before
 *   this power-on (the first ones read without EVENT), else BTS_AUD_FACTORY; only while the listed line is
 *   `SET CONTROL AUDIO INTERNAL I2S_SLAVE` + EVENT / KEEPALIVE words only (any other form is never rewritten); the
 *   module applies it after its restart [inference: like RM 6.99] - the op does not restart it, the manager writes BTS_PIN_AUD + one restart at power-on.
 * Leaving a source that keeps its stream (never stopped by the sink): BTS_SW_HELD_MS (30 s), then BUSY; its
 *   stream gone within BTS_SW_RESUME_MS after that BUSY -> the switch goes on, else the BUSY result is cleared when it
 *   goes (field 2026-11-10 night: the PC let go 20 s after its pause, after the 12 s BUSY - nothing followed).
 *   Experimental option BTS_X_LATE_STOP (policy xopt, RAM, at power-on BTS_LATE_STOP_DEFAULT
 *   - on since 2026-11-13): a left source that has an AVRCP control link, reports paused / stopped and still streams the
 *   policy wait (xwait x 100 ms, at power-on BTS_XWAIT_DEFAULT - 0.1 s since 2026-11-13, 0 = the same; counted from its paused report; earlier builds: 4.5 s from our PAUSE) gets the sink's stop (`SET <its A2DP link> SELECT` + `A2DP STREAMING STOP`, event `late STOP`) -
 *   field 2026-11-11: after the module setting change Windows held its paused stream ~11 s and the earlier
 *   "silent PC after a sink STOP" may have been the module-side phone -> PC silence [inference] - this re-tests it.
 *   Never a source without AVRCP, one with no / a playing report, never in the guard; going back to it: PLAY (ps_ours).
 * Policy modes: SELECT (default) / OFF (table + manual actions only) / NEWEST (opt-in legacy, host model only - not in
 *   the slot payload (-DBTS_NO_NEWEST: the policy write refuses mode 1): two streams -> the
 *   newest wins after hold_ds, the others are closed; flags CLOSE_OTHERS / CALL_PREF / KEEP_PREF / RECOVER as before).
 * Line parser: a module line can come with bytes of another line in it (field 2026-11-09: our command's
 *   echo `SET 1 SELECT` interleaved with the phone's `AVRCP 6 … CHANGED … PLAYING` -> `SAVRCP 6 …` / `ET 1 SELECT`).  A
 *   line that does not start with a known word is searched for `AVRCP <n> …_RSP`, `A2DP STREAMING START|STOP <n>`,
 *   `RING <n> `, `NO CARRIER <n>` and parsed from there; a status line that still cannot be read (a garbled word, bytes
 *   after it, a fragment of `REGISTER_NOTIFICATION_RSP` / `PLAYBACK_STATUS_CHANGED`) asks that link's play status again
 *   (`AVRCP PDU 31 1`; no link id readable: every link registered / waiting for an answer) - at most BTS_PS_FIX_MAX
 *   times per device until a clean status line comes.
 * The policy is RAM; the port keeps recon + the preferred device + BTS_F_FOLLOW on NOR (bts_saved_*, the live value) and
 * at a power-on the startup setting decides what they start from (defaults / the snapshot's
 * NG part / the live value) - the rest starts from bts_policy_default at every power-on.  Context: one task (the cave's engine loop); no I/O here - the port feeds
 * bytes and sends the commands. */
#ifndef CA79NG_BTSRC_H
#define CA79NG_BTSRC_H

#include <stdbool.h>
#include <stdint.h>

#define BTS_DEV_MAX 4
#define BTS_LINKS 16
#define BTS_CMD_MAX 36
#define BTS_OUT_MAX 56   /* bts_next's out: `SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT KEEPALIVE` (52) + NUL - queued
                          * as a 2-byte token (BTS_Q_AUDIO, flags), formed only when it goes out */
#define BTS_Q_AUDIO '\x01'   /* queue tokens `<type> <0x40 | v >> 4> <0x40 | v & 15>`: the 8-bit setting lines */
#define BTS_Q_AVRCP '\x02'
#define BTS_Q 8
#define BTS_LINE_MAX 120

#define BTS_REJOIN_MS 30000u    /* a source closed this recently cannot win against a playing one */
#define BTS_BACKOFF_MS 600000u  /* the sweep does not close the same source twice within 10 min */
#define BTS_ANSWER_MS 1500u     /* a command with no line from the module within this: unanswered */
#define BTS_CMD_GAP_MS 150u     /* between two commands handed to the port */
#define BTS_LIST_DELAY_MS 300u  /* a link event -> LIST this much later (events coalesce) */
#define BTS_PREF_WAIT_MS 4000u
#define BTS_PREF_RETRY_MS 15000u
#define BTS_RECOVER_EXTRA_MS 4000u
#define BTS_RECOVER_GAP_MS 60000u
#define BTS_SW_PAUSE_MS 700u    /* switch: after AVRCP PAUSE, wait this long for STREAMING STOP before suspending */
#define BTS_SW_SUSPEND_MS 2000u /* ... after the sink's A2DP STREAMING STOP, the stream must be gone within this */
#define BTS_SW_CALL_MS 8000u    /* CALL <addr> 19 A2DP: the A2DP link within this */
#define BTS_SW_SETTLE_MS 500u   /* the target's A2DP link (up before, or just CALLed) with AVRCP: PLAY this much later */
#define BTS_SW_FRESH_MS 1500u   /* an A2DP link we just CALLed, no AVRCP on it: wait this long for its AVRCP to come back
                                 * (then PLAY) or for its own start, before the sink starts the stream */
#define BTS_SW_STEADY_MS 1000u  /* the target's stream with no AVRCP to send PLAY on counts only after lasting this long
                                 * (field 2026-11-03: a paused phone's fresh link - START, STOP 10 ms later) */
#define BTS_SW_AVRCP_MS 700u    /* the switch target has A2DP and no AVRCP control link this long: the sink opens AVRCP */
#define BTS_SW_AVRCP_WAIT_MS 3000u /* ... the switch waits at most this long (its stream / the settle phase) for that
                                    * AVRCP before going on without it */
#define BTS_SW_LATE_MS 5000u    /* the switch ended without PLAY: the target's AVRCP comes within this, reports paused /
                                 * stopped and the piano had paused it -> PLAY once */
#define BTS_SW_PLAY_MS 1200u    /* AVRCP PLAY to one that reports PLAYING (KEPT): STREAMING START within this, else the
                                 * sink starts the stream itself */
#define BTS_SW_READY_MS 2500u   /* AVRCP PLAY sent: STREAMING START within this, else "ready" (press play on it) - field
                                 * 2026-11-11 (①): the phone opened its stream 1.4 s after PLAY, the old
                                 * 1.2 s said 「시작하지 못했습니다」 though it played.  A START of the target within
                                 * BTS_SW_LATE_MS after such a "ready" turns the result into "playing" */
#define BTS_SW_START_MS 1000u   /* sink-side `A2DP STREAMING START`: the stream within this, else PLAY / "ready" */
enum { BTS_RA_LATE = 1, BTS_RA_PRE = 2 };
#define BTS_SW_REASSERT_MS 6000u /* after a switch ended PLAYING: another source's STREAMING STOP within this, the target
                                 * streaming -> `SET <its A2DP> SELECT` + `A2DP STREAMING START` once (field 2026-11-09
                                 * evening: the left phone's own STOP 3 s after the PC's START muted the piano) */
#define BTS_PZ_RECHECK_MS 1000u /* our AVRCP PAUSE got no play-status report within this: register its notification
                                 * again, once (field 2026-11-10: the phone paused, no CHANGED - the piano went on
                                 * believing it played) */
#define BTS_SW_HELD_MS 30000u   /* switch: the other sources (never stopped by the sink) stop their streams themselves
                                 * within this, else BUSY (field: Android ~3 s after AVRCP PAUSE, Windows 9-10 s, 20 s after
                                 * the user's pause (2026-11-10 night - 12 s was too short) or never) */
/* the late stop's defaults - one line each to flip after a field test */
#define BTS_LATE_STOP_DEFAULT 1 /* 1 = option BTS_X_LATE_STOP on at every power-on (bts_policy_default), 0 = off */
#define BTS_XWAIT_DEFAULT 1u    /* option BTS_X_LATE_STOP's wait at every power-on, 100 ms units (1 = 0.1 s): the left
                                 * source still streams this long after it reported paused / stopped (its own report, not
                                 * our PAUSE) -> the sink stops it.  Field 2026-11-11 (4.5 s
                                 * from our PAUSE): the PC reports PAUSED 0.1-0.3 s after our PAUSE and then holds ~11 s,
                                 * the phone holds 2.9-3.3 s after its PAUSED; the late STOP did Windows no harm (3 runs).
                                 * 2026-11-13: 0.5 s -> PC→phone 2.2-2.9 s, phone→PC 1.5-1.6 s, the phone unharmed
                                 * (aptX kept, PLAY alone resumes); 0.1 s -> phone→PC 1.3 s, PC→phone 1.9-2.7 s, unharmed - on +
                                 * 0.1 s chosen */
#define BTS_SW_LATE_STOP_MS (BTS_XWAIT_DEFAULT * 100u) /* policy xwait 0 (an app that writes 0): the default wait */
#define BTS_XWAIT_MAX 100u      /* policy xwait (100 ms units, 0 = BTS_SW_LATE_STOP_MS): at most 10.0 s */
#define BTS_SW_RESUME_MS 60000u /* ... BUSY that way: their streams all gone within this after the BUSY -> the switch goes
                                 * on (else the BUSY result is cleared once they are gone) */
#define BTS_ECHO_MS 1000u       /* the module's echo of our `A2DP STREAMING START|STOP` (no link id) comes within this */
#define BTS_PS_DELAY_MS 300u    /* an AVRCP control link up this long: register its playback-status notification */
#define BTS_PS_ANSWER_MS 1500u  /* ... no INTERIM within this: the target does not report its play status (not again on
                                 * this link) */
#define BTS_PS_FIX_MAX 3u       /* a garbled status line asks the play status again at most this often per device until
                                 * a clean status line (field 2026-11-09) */
#define BTS_GUARD_MS 300u       /* GUARD: a stream of a source that is not the selected one lasts this long -> refused */
#define BTS_GUARD_RETRY_MS 3000u /* ... refused again if it still streams this long after the refusal */
#define BTS_GUARD_WIN_MS 30000u /* ... refused BTS_GUARD_CLOSE_N times within this -> suspended at once (no PAUSE) */
#define BTS_GUARD_CLOSE_N 3u
#define BTS_SET_READ_MS 3000u   /* module on -> read the configuration (`SET`) once, this much later */
#define BTS_SET_LIST_MS 2500u   /* a `SET` listing without its closing bare `SET` line within this: no decision */
#define BTS_RC_TRIES 10u        /* reconnect (AUTOCALL off): CALLs per episode */
#define BTS_RC_STABLE_MS 3000u  /* reconnect: A2DP up this long = a connection (the episode ends, the tries start again);
                                 * a source's A2DP closed sooner by its side was a failed attempt (field 2026-11-09: the
                                 * TV the user had disconnected accepted every CALL and closed it 0.3 s later - the
                                 * backoff restarted each time, a CALL every ~5 s); an A2DP closed by the source (`NO
                                 * CARRIER … ERROR 0`, not our CLOSE) after this long = the user disconnected it there */
#define BTS_CALL_MS 10000u      /* a CALL we made: CONNECT lines without an address belong to it this long */
#define BTS_AVRCP_CALL_MS 4000u /* a source with A2DP and no AVRCP control link this long: the sink opens AVRCP (once) -
                                 * the source opens its own first (field 2026-11-09: a Windows PC opened 17 0.5 s after
                                 * its A2DP when the sink did not call; our earlier CALL stayed unanswered 11 s, the PC
                                 * opened a lone 1b and the module rebooted).  A source whose
                                 * lone browsing channel preceded a module reboot (the suspect) never gets it from the sink */
#define BTS_AVRCP_GIVEUP_MS 1500u /* our `CALL … 17 AVRCP` with no CONNECT this long after its `CALL <l>` answer: `CLOSE <l>`
                                   * - never pending when a browsing channel comes (field 2026-11-09: phones answer within
                                   * 0.5 s; the PC never did) */
#define BTS_ORPHAN_REBOOT_MS 3000u /* a lone browsing channel (PSM 0x1b, no 0x17) this shortly before a reboot: suspect */
#define BTS_BOOT_GRACE_MS 5000u /* a module banner / READY. this soon after the module was switched on: its own start */
#define BTS_LOOP_WIN_MS 60000u  /* two module reboots within this: the reboot-loop guard */
#define BTS_LOOP_HOLD_MS 300000u /* ... holds while the last reboot is younger than this (no manager-initiated CALL) */
#define BTS_LOG_CAP 1536u       /* line log ring (bytes): record = len, flags, t (ms, u32 LE), text */
#define BTS_LOG_HDR 6u
#define BTS_SEQ_MASK 0x3FFFu    /* log sequence numbers are 14 bits (two 7-bit bytes on the wire) */

enum { BTS_MODE_OFF = 0, BTS_MODE_NEWEST = 1, BTS_MODE_SELECT = 2 };
/* (the switching strategies - policy strat, op 1D 06 byte 13 - are gone: one way
 * to switch; the port answers byte 13 = 0, 1D 04 byte 5 bits 4-6 = 1, and refuses a write of another value) */
/* the piano's own reconnect while the module's AUTOCALL is off (policy recon; op 1D 06 byte 14, 1D 04 byte 116 bits 0-1
 * = recon + 1; user decision 2026-11-09) */
enum { BTS_RC_OFF = 0 /* never calls by itself */, BTS_RC_PREF = 1 /* the preferred device only */,
       BTS_RC_PREF_LAST = 2 /* the preferred one, else the last selected one (default - the behaviour before) */,
       BTS_RC_MAX = 2 };
/* experimental switch options (policy xopt; op 1D 06 byte 15, 1D 04 byte 116 bit 7 - bit 6 = the payload knows them;
 * RAM only, never on NOR) */
enum { BTS_X_LATE_STOP = 0x01 /* a source left by the switch that has AVRCP, reports paused /
                               * stopped and still streams the policy wait (xwait) after that report: the sink stops its
                               * stream (`SET <l> SELECT` + `A2DP STREAMING STOP`) - default BTS_LATE_STOP_DEFAULT */,
       BTS_X_ALL = 0x01 };
/* the policy's kept part (kvstore key KV_KEY_BT_POLICY = the live value; the startup settings' NG part carries the same
 * bytes): version 1, recon, has_pref, pref[6], kept flags (bit 0 = BTS_F_FOLLOW - since the
 * startup-settings NG part; a build before writes 0 there and reads only bytes 0..8) */
#define BTS_SAVED_RAW 10u
enum { BTS_F_CLOSE_OTHERS = 0x01, BTS_F_CALL_PREF = 0x02, BTS_F_KEEP_PREF = 0x04, BTS_F_RECOVER = 0x08, BTS_F_FOLLOW = 0x10,
       BTS_F_NO_AVRCP_CALL = 0x20 /* opt out: the sink never opens AVRCP to a source by itself */,
       BTS_F_NO_PS = 0x40 /* opt out: no play-status notification (AVRCP PDU 31) - the "paused by us" rule only */,
       BTS_F_ALL = 0x7F };
/* play status of a source (AVRCP 1.4 PLAYBACK_STATUS_CHANGED as iWRAP prints it, RM 6.10.2 / AN986 6.3.4); ERROR =
 * the AVRCP 1.4 value 0xFF (`ERROR` / `255`, 2026-11-03 - before it read as unknown) */
enum { BTS_PS_UNKNOWN = 0, BTS_PS_STOPPED = 1, BTS_PS_PLAYING = 2, BTS_PS_PAUSED = 3, BTS_PS_SEEKING = 4,
       BTS_PS_ERROR = 5 };
/* the play-status byte of a device (op 1D 04 layout 2, bytes 112..115): status in bits 0-2 + these */
enum { BTS_PSB_LIVE = 0x08 /* registered: the target notifies changes */, BTS_PSB_NONE = 0x10 /* the target did not answer
       the registration (rejected / no INTERIM) */, BTS_PSB_OURS = 0x20 /* the piano paused it and nothing since showed it
       playing */ };
/* second device state byte (op 1D 04 layout 2) */
enum { BTS_D2_SELECTED = 0x01, BTS_D2_REFUSED = 0x02, BTS_D2_AVRCP = 0x04, BTS_D2_TARGET = 0x08, BTS_D2_CALLING = 0x10,
       BTS_D2_PARKED = 0x20 /* its sound link was closed by the piano: picking it calls it again */ };
/* switch phase / result (op 1D 04 layout 2) */
enum { BTS_SW_IDLE = 0, BTS_SW_PAUSE = 1, BTS_SW_SUSPEND = 2, BTS_SW_CLOSE = 3, BTS_SW_LINK = 4, BTS_SW_CALL = 5,
       BTS_SW_SETTLE = 6, BTS_SW_PLAY = 7, BTS_SW_START = 8 /* sink-side A2DP STREAMING START sent */,
       BTS_SW_REOPEN = 9 /* (no longer entered - firmware up to 2026-11-03 a12c870a reopened the target's A2DP) */,
       BTS_SW_DIRECT = 10 /* (no longer entered - strategies DIRECT / SELECT, removed) */ };
/* what the switch tried on the target (op 1D 04 layout 2 tail, sticky until the next switch) */
enum { BTS_SWT_PLAY = 0x01, BTS_SWT_START = 0x02, BTS_SWT_REOPEN = 0x04, BTS_SWT_FRESH = 0x08 /* the target was CALLed */,
       BTS_SWT_PLAY_UP = 0x10 /* AVRCP PLAY sent after its stream was already up (field 2026-11-02 afternoon: Windows opens
                               * the stream of a fresh A2DP link while its player stays paused - silence until PLAY) */,
       BTS_SWT_KEPT = 0x20 /* no PLAY: its play status said it plays already (Windows takes PLAY as a toggle - field
                            * 2026-11-02 night, ota_main 9e6c8c32: PLAY to a playing PC paused it) */,
       BTS_SWT_NOT_OURS = 0x40 /* no PLAY: play status unknown and the piano had not paused it */,
       BTS_SWT_LATE = 0x80 /* AVRCP PLAY sent after the switch had ended (its AVRCP came back late, reported paused,
                            * the piano had paused it) - with BTS_SWT_PLAY */ };
/* module health (op 1D 04 layout 2 tail) */
enum { BTS_MH_LOOP = 0x01 /* reboot loop: manager-initiated CALLs held */, BTS_MH_SUSPECT = 0x02 /* a lone browsing
       channel preceded a reboot */, BTS_MH_PIN = 0x04 /* a decided module value was not kept this
       power-on: the module did not take a write, or the restart / its read back did not come */ };
enum { BTS_RES_NONE = 0, BTS_RES_PLAYING = 1, BTS_RES_READY = 2, BTS_RES_BUSY = 3, BTS_RES_NO_LINK = 4,
       BTS_RES_CANCELLED = 5 };
/* AUTOCALL as read from the module's `SET` listing; the explicit op */
/* `A2DP CODEC {codec} {channel_mode} {rate} BITPOOL …` (RM 6.67 SET CONTROL AUDIO events) */
enum { BTS_CODEC_NONE = 0, BTS_CODEC_SBC = 1, BTS_CODEC_AAC = 2, BTS_CODEC_APTX = 3, BTS_CODEC_APTX_LL = 4, BTS_CODEC_OTHER = 5 };
enum { BTS_AC_UNKNOWN = 0, BTS_AC_ON = 1, BTS_AC_OFF = 2 };
enum { BTS_AC_READ = 1, BTS_AC_SET_OFF = 2, BTS_AC_RESTORE = 3 };
/* class of device as read from the module's `SET` listing; the explicit op (1D 0A) */
#define BTS_COD_AUDIO_SINK 0x240414u   /* Rendering + Audio service bits, Audio/Video major, Loudspeaker minor */
#define BTS_COD_IWRAP 0x001F00u        /* the module's value read live 2026-11-09 (major uncategorized, no service) */
enum { BTS_COD_UNKNOWN = 0, BTS_COD_KNOWN = 1 };
enum { BTS_CO_READ = 1, BTS_CO_WRITE = 2, BTS_CO_RESTORE = 3 };
/* AVRCP profile features as read from the module's `SET` listing; the explicit op (1D 0C) */
#define BTS_AVF_BROWSE 0x10u           /* AN986 3.2 / RN 6.5.3: bit 4 = browsing (bits 0-3 = categories 1-4) */
#define BTS_AVF_NO_BROWSE 0x0Fu        /* what the write writes by default: categories 1-4, no browsing */
#define BTS_AVF_FACTORY 0x1Fu          /* the stock factory line `SET PROFILE AVRCP CONTROLLER 1f` (REA 0x4013EDE8) */
enum { BTS_AVF_UNKNOWN = 0, BTS_AVF_CTRL = 1 /* `SET PROFILE AVRCP CONTROLLER <hex>` alone */,
       BTS_AVF_OTHER = 2 /* no AVRCP line, or another form (TARGET too): the op does not write */ };
enum { BTS_AV_READ = 1, BTS_AV_WRITE = 2, BTS_AV_RESTORE = 3 };
/* audio routing `SET CONTROL AUDIO INTERNAL I2S_SLAVE [EVENT] [KEEPALIVE]` as read from the `SET` listing; the explicit
 * op (1D 0D).  Flags: */
#define BTS_AUD_EVENT 0x01u            /* EVENT: `AUDIO ROUTE …` / `A2DP CODEC …` lines (RM 6.67) */
#define BTS_AUD_KEEP 0x02u             /* KEEPALIVE: the DSP stays powered between A2DP streams (RM 6.67) */
#define BTS_AUD_FACTORY BTS_AUD_KEEP   /* the stock factory line `… INTERNAL I2S_SLAVE KEEPALIVE` (REA 0x4013EE30) */
enum { BTS_AUD_UNKNOWN = 0, BTS_AUD_STD = 1 /* `SET CONTROL AUDIO INTERNAL I2S_SLAVE` + EVENT / KEEPALIVE only */,
       BTS_AUD_OTHER = 2 /* no such line, or another form (other routing, AAx, i2s bits): the op does not write */ };
enum { BTS_AU_READ = 1, BTS_AU_WRITE = 2, BTS_AU_RESTORE = 3 };
/* DECIDED MODULE VALUES - kept by the manager itself at power-on (user decision 2026-11-13;
 * the values written in the field 11-09 / 11-11 and working).  After the first complete `SET` listing of
 * a power-on (bts_init) a line that differs from its decided value is written through the op's own path (bts_class /
 * bts_avrcp_features / bts_audio: one line + `SET` read back, one op at a time); a line not listed / not the plain form
 * is never written.  Only when the AVRCP or the audio line was written (the module applies them after its restart - RM
 * 6.99 note / [inference]) the module is restarted ONCE the way op 1D 07 does (recover_req -> the port's stock off / on)
 * and the listing read back.  Once per power-on: a write the module did not take is not tried again (the op's rc
 * BTS_STATE, health BTS_MH_PIN); never while a switch runs, never during the reboot-loop guard; a live apply (handover
 * block taken, or a controller bound at the first tick) writes but never restarts - the module
 * restarts at the next power-on.  Ops 1D 0A / 0C / 0D stay. */
#define BTS_PIN_COD BTS_COD_AUDIO_SINK /* `SET BT CLASS 240414` (factory 001f00): TVs list the piano as an audio device */
#define BTS_PIN_AVF BTS_AVF_NO_BROWSE  /* `SET PROFILE AVRCP CONTROLLER 0f` (factory 1f): no browsing channel to open */
#define BTS_PIN_AUD BTS_AUD_EVENT      /* `SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT` (factory `… KEEPALIVE`) */
#define BTS_PIN_RESTART_MS 3000u       /* the restart asked: the module goes off within this, else given up */
#define BTS_PIN_CHECK_MS 6000u         /* the read back after the restart: its listing within this, else given up */
/* the pin's phase (bts_t.pin) */
enum { BTS_PIN_WAIT = 0 /* the first complete listing of this power-on */, BTS_PIN_WRITE = 1 /* the differing lines go */,
       BTS_PIN_RESTART = 2 /* module restart asked */, BTS_PIN_BACK = 3 /* the module is back: read the listing */,
       BTS_PIN_CHECK = 4 /* that listing decides */, BTS_PIN_DONE = 5 /* nothing (more) to do this power-on */ };
/* the settings (bts_t.pin_todo / pin_w / pin_bad) */
enum { BTS_PB_COD = 0x01, BTS_PB_AVF = 0x02, BTS_PB_AUD = 0x04 };
/* device state bits (op 1D 04) */
enum { BTS_D_STREAM = 0x01, BTS_D_INCOMING = 0x02, BTS_D_CLOSED = 0x04, BTS_D_PREF = 0x08, BTS_D_ACTIVE = 0x10,
       BTS_D_ADDR = 0x20, BTS_D_CLOSING = 0x40 };
/* profiles of a link */
enum { BTS_P_NONE = 0, BTS_P_A2DP = 1, BTS_P_AVRCP = 2, BTS_P_OTHER = 3 };
/* rc of the manual actions */
enum { BTS_OK = 0, BTS_BAD = 1, BTS_UNKNOWN = 2, BTS_FULL = 3, BTS_NO_AVRCP = 4,
       BTS_STATE = 5 /* AUTOCALL op: off needs AUTOCALL read ON first, restore needs it read OFF; class op: the class
                      * read first and not already the value to write; AVRCP features op: the same, and the line
                      * controller-only; audio op: the same, and the line the plain form (BTS_AUD_STD) */ };
/* manual actions (op 1D 05 act) */
enum { BTS_ACT_CLOSE = 0, BTS_ACT_USE = 1, BTS_ACT_CALL = 2, BTS_ACT_ONLY = 3, BTS_ACT_PAUSE = 4, BTS_ACT_PLAY = 5,
       BTS_ACT_MUTE = 6 /* close the device's A2DP link only (AVRCP stays) */, BTS_ACT_MAX = 6 };
/* line log record flags */
enum { BTS_L_TX = 0x01, BTS_L_TRUNC = 0x02, BTS_L_EVENT = 0x04 };

typedef struct {
    uint8_t mode, flags;
    uint8_t hold_ds;   /* two streams: the newest wins after this many 0.1 s (1..250) */
    uint8_t sweep_s;   /* CLOSE_OTHERS: after this many s of streaming (1..250) */
    uint8_t has_pref;
    uint8_t pref[6];   /* preferred device address, most significant byte first (as printed) */
    uint8_t recon;     /* BTS_RC_* (AUTOCALL off) */
    uint8_t xopt;      /* BTS_X_* experimental options (RAM only, BTS_LATE_STOP_DEFAULT at every power-on; op 1D 06
                        * byte 15) */
    uint8_t xwait;     /* BTS_X_LATE_STOP wait after the paused report, 100 ms units (BTS_XWAIT_DEFAULT at every
                        * power-on), 0 = BTS_SW_LATE_STOP_MS (1..BTS_XWAIT_MAX; RAM only; op 1D 06 byte 16, 1D 04 byte
                        * 119) */
} bts_policy_t;

typedef struct {
    uint8_t used, addr_known, incoming, streaming, swept, closing;
    uint8_t refusing, refuse_n;   /* GUARD: this stream was refused; refusals in the window */
    uint8_t addr[6];
    uint16_t links, a2dp;     /* bit per iWRAP link id */
    uint32_t seq;             /* STREAMING START order */
    uint32_t stream_ms;       /* streaming for this long */
    uint32_t closing_ms;
    uint32_t refuse_ms;       /* since the last refusal (window / retry) */
    uint8_t avrcp_called;     /* the sink opened AVRCP to it in this A2DP episode (once) */
    uint8_t a2dp_closing;     /* its A2DP signalling link was CLOSEd by us (not again until it goes / 3 s) */
    uint8_t parked;           /* we closed its A2DP to switch away: kept in the table with no link (to be picked again) */
    uint8_t codec;            /* BTS_CODEC_* of its last stream (the `A2DP CODEC` event), 0 unknown */
    uint32_t a2dp_close_ms;
    uint32_t noavrcp_ms;      /* A2DP up, no AVRCP control link, this long */
    /* play status (AVRCP PDU 31 PLAYBACK_STATUS_CHANGED notification on its AVRCP control link) */
    uint8_t ps;               /* BTS_PS_* */
    uint8_t ps_link;          /* the AVRCP link the registration belongs to, + 1 (0 none) */
    uint8_t ps_pend, ps_live, ps_none;   /* registration sent, no answer yet / registered / the target does not answer */
    uint8_t ps_ours;          /* the piano sent it AVRCP PAUSE and nothing since showed it playing */
    uint8_t fresh;            /* its A2DP link came from our CALL (or the sink started its stream) and no stream on it has
                               * lasted BTS_SW_STEADY_MS yet: its start says nothing of the player (Windows streams a fresh
                               * link while paused; field 2026-11-03: a paused phone's START, STOP 10 ms later) */
    uint32_t ps_wait;         /* since the AVRCP link came / the registration went */
    uint32_t a2dp_ms;         /* its A2DP link(s) up this long (this A2DP episode) */
    uint8_t ps_fix;           /* registrations asked again after a garbled status line, since its last clean one */
    uint8_t susp;             /* the sink suspended its stream (A2DP STREAMING STOP) and it has not started since - a
                               * switch back asks its player first (AVRCP), the sink's START only without AVRCP / after */
    uint8_t ps_old;           /* its stream started by itself (not fresh) after its last play-status report: that report
                               * is older than the stream - it plays (field 2026-11-09 evening: a phone paused / played
                               * on its own, the CHANGED lost, the stale "paused" skipped the PAUSE) */
    uint8_t pz_wait;          /* our AVRCP PAUSE went and no play-status report since: re-registered after
                               * BTS_PZ_RECHECK_MS (once per PAUSE) */
    uint32_t pz_ms;
    uint32_t pzr_ms;          /* since it last began to report paused / stopped (ps_paused false -> true; BTS_X_LATE_STOP) */
    uint8_t held;             /* streaming with no AVRCP control link: the sink did not stop it (logged once per stream) */
} bts_dev_t;

typedef struct {
    uint8_t used, addr[6];
    uint32_t ago_ms;
} bts_closed_t;

typedef struct {
    /* (pauses, plays, no_avrcp, switches, ac_writes, starts_sent, reopens, ps_regs, ps_events
     * removed - counted, never read: not on the wire, not in a test) */
    uint16_t lines, closes, calls, lists, unanswered, recoveries, dual, starts;
    uint16_t suspends, refusals, reconnects;
    uint16_t reboots, orphans, avrcp_calls;
    uint16_t play_skips, pause_skips;
    uint16_t garbled, recovered;
    uint16_t orphan_closes;       /* lone AVRCP browsing channels (0x1b, no control link) closed at once */
    uint16_t avrcp_giveups;       /* our `CALL … 17 AVRCP` with no CONNECT within BTS_AVRCP_GIVEUP_MS: CLOSE <l> */
    uint16_t cod_writes;          /* `SET BT CLASS …` sent (the class op only) */
    uint16_t avf_writes;          /* `SET PROFILE AVRCP …` sent (the AVRCP features op only) */
    uint16_t held;                /* a source the sink did not stop (never since 2026-11-10: a Windows audio device stays
                                   * silent after a sink-initiated suspend until its PnP restart) */
    /* (lists, starts, orphans, orphan_closes, avrcp_giveups, cod_writes, avf_writes, play_skips, pause_skips, held:
     * counted on the host only - the cave builds with -DBTS_NO_TEST_STATS) */
} bts_stats_t;

/* an 8-bit module setting read from the `SET` listing with its explicit op (AVRCP features 1D 0C, audio routing 1D 0D):
 * state 1 = the plain form (BTS_AVF_CTRL / BTS_AUD_STD), 2 = another form; the same layout as the named fields */
typedef struct {
    uint8_t state, saved, op, rc;
    uint8_t now, orig, want;
    uint8_t set_state, set_v;
} bts_set8_t;

typedef struct {
    uint8_t buf[BTS_LOG_CAP];
    uint16_t head, used;      /* oldest record's offset, bytes in use */
    uint16_t first, next;     /* sequence numbers (14 bits) of the oldest record / the next one */
} bts_log_t;

/* live apply handover: the devices connected when the old payload was dropped and the selected
 * one, for the payload applied live after it (bts_handover_save / _load) */
typedef struct {
    uint8_t armed;            /* a live apply handed devices over: they are checked / called back (BTS_HO_MS at most) */
    uint8_t listed;           /* the first LIST reply after it has ended */
    uint8_t ev;               /* a stream line came after it (the handed-over stream state is stale then) */
    uint8_t n, sel_stream;    /* devices handed over (the selected one first) / the selected one was streaming */
    uint8_t addr[BTS_DEV_MAX][6];
    uint8_t tries[BTS_DEV_MAX], done[BTS_DEV_MAX];
    uint32_t ms, wait;
} bts_ho_t;

typedef struct {
    bts_policy_t pol;
    bts_dev_t dev[BTS_DEV_MAX];
    bts_closed_t closed[BTS_DEV_MAX];
    int8_t link_dev[BTS_LINKS];
    uint8_t link_prof[BTS_LINKS];
    uint8_t link_ch[BTS_LINKS];   /* L2CAP PSM as printed (0x19 A2DP, 0x17 AVRCP control, 0x1b AVRCP browsing) */
    uint32_t seq;
    int8_t active;            /* device index of the source that plays (or last played), -1 */
    /* line assembler */
    char line[BTS_LINE_MAX];
    uint8_t len, ovf;
    /* LIST reply */
    uint8_t listing, list_left;
    uint16_t list_seen;
    uint8_t list_want;
    uint32_t list_wait;
    /* commands for the port */
    char q[BTS_Q][BTS_CMD_MAX];
    uint8_t q_n;
    uint32_t gap_ms;
    uint8_t waiting, no_answer;
    uint32_t wait_ms;
    /* module */
    uint8_t module_on;
    uint32_t ready_ms;
    uint8_t pref_tries;
    uint32_t pref_wait;
    uint32_t dual_ms, recover_ago;
    uint8_t recover_req;
    /* selection (RAM, survives module restarts): the source that should play */
    uint8_t has_sel, sel_addr[6];
    /* the switch (USE) */
    uint8_t sw_phase, sw_result, sw_play, sw_tried;
    uint8_t sw_pl;            /* AVRCP PLAY went to the target on its current A2DP link (a reopen / CALL clears it) */
    uint8_t sw_addr[6];
    uint32_t sw_ms, sw_total, sw_last_ms;
    uint8_t sw_late;          /* the switch ended without PLAY: a late PLAY is armed (BTS_SW_LATE_MS) */
    uint8_t susp_paused;      /* RAM, not on the wire, 0 after bts_init: 1 = the guard suspends a source that reports
                               * paused too (up to ota_main e5bb2372 - host model tests of that path; the switch
                               * suspends it anyway since 11-09 evening) */
    uint8_t sw_reassert;      /* BTS_RA_*: the target streamed when picked (its START re-sent once) / its START re-sent
                               * after another source's late STOP (once per switch) */
    uint32_t sw_ago;          /* since the last switch ended (saturates) */
    uint8_t sw_held;          /* the switch waits for the other sources to stop their streams themselves (BTS_SW_HELD_MS);
                               * kept after a BUSY so: the switch goes on when they let go (BTS_SW_RESUME_MS) */
    uint8_t susp_ok;          /* RAM, not on the wire, 0 after bts_init: 1 = the sink may stop a source's stream (`A2DP
                               * STREAMING STOP`) that has AVRCP - the behaviour up to ota_main c0b89c60, kept for the host
                               * model tests of that path.  0 (the default since 2026-11-10): the
                               * piano never stops a source's stream - a Windows Bluetooth audio device stays silent after
                               * a sink-initiated suspend until its PnP device is restarted */
    uint8_t echo_ss;          /* our `A2DP STREAMING START` (1) / `STOP` (2) went: its echo (no link id) is expected */
    uint8_t sel_out;          /* our `SET <l> SELECT` went: its echo lets the paired command go */
    int8_t sel_lnk;           /* the <l> of that `SET <l> SELECT`: its echo is known by it even with its head cut off */
    uint32_t echo_ms;
    uint32_t sw_late_ms;
    /* a CALL we made: its links (CALL <l> / address-less CONNECT) belong to this address */
    uint8_t call_on, call_avrcp, call_addr[6];   /* call_avrcp: the CALL is `… 17 AVRCP` */
    uint32_t call_ms;
    int8_t av_lid;            /* the link id our `CALL … 17 AVRCP` was answered with (`CALL <l>`) while no CONNECT on it, -1 */
    uint32_t av_ms;           /* ... since that answer (BTS_AVRCP_GIVEUP_MS -> CLOSE <l>) */
    /* module health: reboots it did by itself (banner / READY. while on), the loop guard, the suspect source */
    uint8_t loop, reboot_seen, has_suspect, suspect[6];
    uint32_t reboot_ago, orphan_ago;
    uint8_t orphan_addr[6], has_orphan;
    /* the last `A2DP CODEC` event: codec, rate (0 44.1k, 1 48k, 2 other), age; attached to the stream it belongs to */
    uint8_t codec, codec_rate, codec_dev;
    uint32_t codec_ago;
    /* AUTOCALL (from the `SET` listing) and the explicit op */
    uint8_t ac_state, ac_saved, ac_op, ac_rc;
    char ac_line[BTS_CMD_MAX];     /* the AUTOCALL line read while ON (restore writes it back) */
    uint8_t set_listing, set_seen_ac, set_read;
    uint8_t set_lines;             /* lines of the listing in progress: kept out of the line log (one summary event) */
    char set_ac[BTS_CMD_MAX];
    uint32_t set_ms;
    /* class of device (from the `SET` listing) and the explicit op */
    uint8_t cod_state, cod_saved, cod_op, cod_rc, set_seen_cod;
    uint32_t cod, cod_orig, cod_want, set_cod;   /* now / read first (restore) / the op's target / this listing's */
    /* AVRCP controller features (from the `SET` listing) and the explicit op */
    union {
        struct {
            uint8_t avf_state, avf_saved, avf_op, avf_rc;
            uint8_t avf, avf_orig, avf_want;     /* now / read first (restore) / the op's target */
            uint8_t set_avf_state, set_avf;      /* this listing's */
        };
        bts_set8_t avs;
    };
    uint8_t set_seen_avf;
    /* reconnect (AUTOCALL off) */
    uint8_t rc_tries;
    uint32_t rc_wait;
    uint32_t rc_up_ms;        /* some A2DP link up this long (0 none) - BTS_RC_STABLE_MS ends the episode */
    uint8_t sel_norc;         /* the selected device closed its own A2DP after a normal connection (the user disconnected
                               * it there, field 2026-11-09): not the "last selected" reconnect target until it is selected
                               * again / connects and stays (RAM - this power-on; the preferred device is not affected) */
    uint32_t now_ms;          /* time since bts_init (the log's clock) */
    bts_stats_t n;
    bts_log_t log;
    /* audio routing flags (BTS_AUD_*, from the `SET` listing) and the explicit op */
    union {
        struct {
            uint8_t aud_state, aud_saved, aud_op, aud_rc;
            uint8_t aud, aud_orig, aud_want;     /* now / read first (restore) / the op's target */
            uint8_t set_aud_state, set_aud;      /* this listing's */
        };
        bts_set8_t aus;
    };
    /* the last `AUDIO ROUTE <l> <type> …` event (RM 6.67, only with the module's EVENT flag): 1 + link id (0 none since
     * the module started), its type A2DP - op 1D 0D reports it with the link's device */
    uint8_t route, route_a2dp;
    bts_ho_t ho;              /* live apply handover */
    /* the decided module values: phase BTS_PIN_*, settings still to write / written / not kept (BTS_PB_*) */
    uint8_t pin, pin_todo, pin_w, pin_bad;
    uint8_t set_done;         /* a complete `SET` listing came (since bts_init / the pin's read back asked) */
    uint8_t live;             /* this payload came by a live apply (a handover block taken, or the port saw a controller
                               * bound at its first tick): the pin never restarts */
    uint32_t pin_ms;
} bts_t;

void bts_policy_default(bts_policy_t *p);
/* 0 = accepted (mode / flags / ranges valid) */
int bts_policy_check(const bts_policy_t *p);
/* the part of the policy kept on NOR: encode / apply a stored record onto p (true = applied; a record of another
 * version or with a value out of range changes nothing; a 9-byte record (no flags byte) leaves BTS_F_FOLLOW off) */
void bts_saved_encode(const bts_policy_t *p, uint8_t out[BTS_SAVED_RAW]);
bool bts_saved_apply(bts_policy_t *p, const uint8_t *raw, uint32_t n);
void bts_init(bts_t *b, const bts_policy_t *pol);
/* module power (stock word 0x4139E7F0): an edge forgets every link */
void bts_module(bts_t *b, bool on);
/* raw module bytes (lines end at CR / LF) */
void bts_rx(bts_t *b, const uint8_t *bytes, uint32_t n);
/* one module line without CR / LF */
void bts_line(bts_t *b, const char *line, uint32_t len);
/* time; can_act = the piano is on and the module may be sent commands (policy runs only then) */
void bts_tick(bts_t *b, uint32_t elapsed_ms, bool can_act);
/* next command for the module (NUL-terminated, without CR LF; cap BTS_OUT_MAX holds any), length or 0; the caller sends
 * it + "\r\n" */
uint32_t bts_next(bts_t *b, char *out, uint32_t cap);
/* module health bits (BTS_MH_*) */
uint8_t bts_health(const bts_t *b);
/* manual: close every link of the device / make it the only source (close the others - last resort) / call it */
int bts_close(bts_t *b, const uint8_t addr[6]);
int bts_select(bts_t *b, const uint8_t addr[6]);
int bts_call(bts_t *b, const uint8_t addr[6]);
/* 「이 기기로 듣기」: select the device and run the switch (the one playing is paused - never closed; CALL
 * the chosen one if it has no A2DP, resume / PLAY it); play false: select + stop the others only */
int bts_use(bts_t *b, const uint8_t addr[6], bool play);
/* the explicit AUTOCALL op (BTS_AC_READ / SET_OFF / RESTORE) - never called by the manager itself */
int bts_autocall(bts_t *b, uint8_t op);
/* the restore line (the one read before, else the stock factory line); NUL-terminated */
const char *bts_autocall_line(const bts_t *b);
/* a class the class op may write: Audio/Video major (0x04), the Rendering service bit, format bits 0, 24 bits */
bool bts_cod_ok(uint32_t cod);
/* the explicit class-of-device op (BTS_CO_READ / WRITE cod (0 = BTS_COD_AUDIO_SINK, else bts_cod_ok) / RESTORE) - the manager
 * calls it itself only to write BTS_PIN_COD at power-on; one op at a time with the AUTOCALL op (either one's read-back pending: BTS_FULL) */
int bts_class(bts_t *b, uint8_t op, uint32_t cod);
/* the class restore writes (the one read before, else BTS_COD_IWRAP) */
uint32_t bts_class_restore(const bts_t *b);
/* features the AVRCP op may write: categories only (1..0x0F - at least one, AN986 3.2), the browsing bit clear */
bool bts_avf_ok(uint8_t f);
/* the explicit AVRCP features op (BTS_AV_READ / WRITE f (0 = BTS_AVF_NO_BROWSE, else bts_avf_ok) / RESTORE) - the manager
 * calls it itself only to write BTS_PIN_AVF at power-on; one op at a time with the AUTOCALL and class ops (a read-back pending: BTS_FULL) */
int bts_avrcp_features(bts_t *b, uint8_t op, uint8_t f);
/* the features restore writes (the ones read before, else BTS_AVF_FACTORY) */
uint8_t bts_avf_restore(const bts_t *b);
/* flags the audio op may write: BTS_AUD_EVENT (KEEPALIVE off + EVENT) or BTS_AUD_EVENT | BTS_AUD_KEEP (EVENT added) */
bool bts_aud_ok(uint8_t f);
/* the explicit audio routing op (BTS_AU_READ / WRITE f (0 = BTS_AUD_EVENT, else bts_aud_ok) / RESTORE) - the manager calls
 * it itself only to write BTS_PIN_AUD at power-on; one op at a time with the AUTOCALL, class and AVRCP ops (a read-back pending: BTS_FULL) */
int bts_audio(bts_t *b, uint8_t op, uint8_t f);
/* the flags restore writes (the ones read before, else BTS_AUD_FACTORY) */
uint8_t bts_aud_restore(const bts_t *b);
/* the line for flags f: `SET CONTROL AUDIO INTERNAL I2S_SLAVE[ EVENT][ KEEPALIVE]` (NUL-terminated, < BTS_CMD_MAX) */
void bts_audio_line(char *out, uint8_t f);
/* the selected device's table index, -1 */
int bts_sel_dev(const bts_t *b);
/* second state byte (BTS_D2_*) */
uint8_t bts_dev_state2(const bts_t *b, int i);
/* play-status byte (BTS_PS_* | BTS_PSB_*) */
uint8_t bts_dev_ps(const bts_t *b, int i);
/* AVRCP PAUSE (play false) / PLAY to one device; BTS_NO_AVRCP when it has no AVRCP link */
int bts_avrcp(bts_t *b, const uint8_t addr[6], bool play);
/* op 1D 05 act (BTS_ACT_*) */
int bts_action(bts_t *b, uint8_t act, const uint8_t addr[6]);
/* the line log: add a record (module line, a command sent (BTS_L_TX), an event (BTS_L_EVENT)) */
void bts_log(bts_t *b, uint8_t flags, const char *text, uint32_t n);
/* records from sequence `from` on (older ones skipped), 7-bit wire form: seq (2 x 7 bits, high first), t (10 ms,
 * 4 x 7 bits, high first), flags, len, text (bytes >= 0x80 as '?') - as many whole records as fit in cap (at
 * least the first, text cut to fit).  Returns the bytes written; *first / *next = the log's sequence range. */
uint32_t bts_log_read(const bts_t *b, uint16_t from, uint8_t *out, uint32_t cap, uint16_t *first, uint16_t *next);
/* the power port's BT hold: is the source streaming on A2DP link `l` (the link of
 * the last STREAMING START; -1 / not in the table: the one device that streams) playing as its AVRCP play status says?
 * 1 it reports PLAYING, 0 it reports anything else (paused, stopped, seeking, error), -1 not known (no such device, no
 * AVRCP control link, the registration not answered yet / rejected / unanswered, notifications off) */
int bts_stream_playing(const bts_t *b, int l);
/* A2DP links known (any device) - the power port keeps its link state while this is non-zero */
uint16_t bts_a2dp_links(const bts_t *b);
/* the RECOVER request (read once) */
bool bts_take_recover(bts_t *b);
/* device state bits (BTS_D_*) */
uint8_t bts_dev_state(const bts_t *b, int i);
int bts_streams(const bts_t *b);

/* ---- live apply handover ------------------------------------------------------------------
 * A live apply (OTA op 0x15) drops the old payload's RAM: the selected device and the link table.  The old payload writes
 * this block into a fixed RAM word range that the apply does not touch (bt_cave.c bt_handover) when it says it is idle; the
 * new one reads it once before its first tick and clears it.  Taken only when it is whole (magic, version, CRC) and
 * written less than BTS_HO_FRESH_US before (a power cycle never hands anything over - SDRAM is not kept, and a stamp of an
 * older power-on is out of the window).  Then: the selection comes back, the first LIST rebuilds the table (the module
 * keeps its links over an apply); a handed-over device that LIST does not show is called back (`CALL <addr> 19 A2DP`,
 * the selected one first, one at a time, BTS_HO_TRIES each, BTS_HO_GAP_MS apart, within BTS_HO_MS) - whatever the
 * reconnect setting says: that setting is about power-on, this restores the user's own links.  Never on a cold boot. */
#define BTS_HO_RAW 52u
#define BTS_HO_VER 1u
#define BTS_HO_FRESH_US 5000000u   /* written at most this long ago (a live apply switches within ~0.1 s of it) */
#define BTS_HO_LIST_MS 2500u       /* no LIST reply this long after the handover: LIST again */
#define BTS_HO_TRIES 3u            /* CALLs per device that did not stay */
#define BTS_HO_GAP_MS 5000u        /* between the CALLs of one device */
#define BTS_HO_MS 60000u           /* the handover ends this long after the apply */
/* the block (BTS_HO_RAW B, LE): 0 "C79B" · 4 version · 5 flags (bit 0 selected known, bit 1 the selected one streams) ·
 * 6 device count (<= BTS_DEV_MAX) · 7 0 · 8 selected address · 14 0 0 · 16 addresses (6 B each, BTS_DEV_MAX) · 40 0 x 4 ·
 * 44 stamp (hal µs at the write) · 48 CRC-32 of bytes 0..47 */
enum { BTS_HO_F_SEL = 0x01, BTS_HO_F_STREAM = 0x02 };
/* the devices with an A2DP link now (and the selection), stamped `stamp_us`; returns BTS_HO_RAW */
uint32_t bts_handover_save(const bts_t *b, uint32_t stamp_us, uint8_t out[BTS_HO_RAW]);
/* right after bts_init: take a block written by the payload before (`now_us` - stamp < BTS_HO_FRESH_US); the number of
 * devices handed over, -1 = no valid / fresh block (cold boot: nothing changes) */
int bts_handover_load(bts_t *b, const uint8_t *raw, uint32_t n, uint32_t now_us);

#endif
