/* OpenGrand host simulator.
 *
 * Runs the complete firmware core against a reference synthesizer. Line
 * commands on stdin (one per line, '#' comments):
 *
 *   midi <port> <hex bytes>     bytes arriving on a port (din|usb|panel|ble)
 *   key <note> <vel> [hold_ms]  press a key (note 21..108) with a velocity;
 *                               hold_ms releases it after that time
 *   up <note>                   release a key
 *   pedal <damper|sostenuto|soft> <0..127>
 *   wait <ms>                   advance time (and render audio)
 *   status                      print a one-line state summary
 *   usb attach|detach           plug / unplug the simulated USB memory
 *   ls                          list the USB memory as the firmware sees it
 *   bt <iWRAP line>             a line from the BT audio module (A2DP STREAMING START 0, ...)
 *   btlink 0|1                  the stock A2DP link flag (&& module power)
 *   apo                         print the APO / BT audio state and the power policy
 *   standby [enter|wake|off]    print the partial standby state / enter / wake (power button) /
 *                               real off (power button held). Power policy: 0x7D op 0x18
 *   ack auto|off                auto: the simulator ACKs (07) every fn 0x10 / 0x03 frame the
 *                               firmware sends, as a controller does — the default without
 *                               --realtime, so scripted runs (make check, p3_map.py, whose
 *                               stock side stubs the ACK wait the same way) see whole dumps
 *   quit
 *
 * Every MIDI message the firmware transmits is printed as
 *   tx <port> <HEX>
 * which scripts/dev-server.mjs (app) forwards to the browser over WebSocket.
 *
 * Options: --wav <file>   write the rendered audio (44.1 kHz stereo 16-bit)
 *          --flash <file> persist the settings flash between runs
 *          --realtime     advance time with the wall clock (interactive use)
 *          --data <dir>   persist recorder songs (RecorderData) as <dir>/songNN.mid
 *          --usb <dir>    host directory used as USB memory (attached at start)
 */
#define _POSIX_C_SOURCE 200809L
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ca79ng/piano.h"
#include "hal_power_sim.h"
#include "hostfs.h"
#include "synth.h"

#define SR 44100
#define FLASH_SECTOR 8192u

static synth_t synth;
static sound_engine_t engine;
static piano_t piano;
static kvstore_t kv;
static rec_event_t rec_pool[REC_MAX_EVENTS], rec_stage[REC_MAX_EVENTS];
static uint8_t flash_mem[2][FLASH_SECTOR];
static const char *flash_path;
static FILE *wav;
static uint32_t wav_frames;
static uint32_t now_us;
static int pending_release[KB_KEYS]; /* ms left until auto release, 0 = none */
static const char *data_dir;
static hostfs_t usb_host;
static hal_fs_t usb_fs;
static bool have_usb;
static uint8_t smf_buf[2u * 1024u * 1024u];

/* ---------------- RecorderData (file backed) ---------------- */
static void song_path(uint8_t song, char *out, size_t cap) { snprintf(out, cap, "%s/song%02u.mid", data_dir, song); }
static void plat_save_song(void *c, uint8_t song, const uint8_t *smf, uint32_t len)
{
    (void)c;
    if (!data_dir) return;
    char p[1024];
    song_path(song, p, sizeof p);
    if (len == 0) { remove(p); return; }
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(smf, 1, len, f); fclose(f); }
}
static int32_t plat_load_song(void *c, uint8_t song, uint8_t *buf, uint32_t cap)
{
    (void)c;
    if (!data_dir) return -1;
    char p[1024];
    song_path(song, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (int32_t)n;
}

static const char *PORT_NAMES[MIDI_PORT_COUNT] = {"din", "usb", "panel", "ble", "internal"};

/* ---------------- flash (file backed) ---------------- */
static void flash_sync(void)
{
    if (!flash_path) return;
    FILE *f = fopen(flash_path, "wb");
    if (f) { fwrite(flash_mem, 1, sizeof flash_mem, f); fclose(f); }
}
static bool f_erase(void *c, uint32_t s) { (void)c; memset(flash_mem[s], 0xFF, FLASH_SECTOR); flash_sync(); return true; }
static bool f_prog(void *c, uint32_t s, uint32_t o, const void *d, uint32_t n)
{
    (void)c;
    for (uint32_t i = 0; i < n; i++) flash_mem[s][o + i] &= ((const uint8_t *)d)[i];
    flash_sync();
    return true;
}
static bool f_read(void *c, uint32_t s, uint32_t o, void *d, uint32_t n) { (void)c; memcpy(d, flash_mem[s] + o, n); return true; }
static const hal_flash_t FLASH = {NULL, FLASH_SECTOR, f_erase, f_prog, f_read};

/* ---------------- platform ---------------- */
/* ---------------- scripted controller ACKs ---------------- */
static bool auto_ack;
static struct { uint8_t port, key[3]; } ackq[256];
static unsigned ackq_n;

static void plat_tx(void *c, midi_port_t port, const uint8_t *m, uint16_t n)
{
    (void)c;
    if (auto_ack && n >= 10 && m[0] == 0xF0 && m[1] == 0x40 && (m[3] == 0x10 || m[3] == 0x03) && m[4] == 0x04 &&
        ackq_n < sizeof ackq / sizeof ackq[0]) {
        ackq[ackq_n].port = (uint8_t)port;
        memcpy(ackq[ackq_n].key, m + 6, 3);
        ackq_n++;
    }
    printf("tx %s", PORT_NAMES[port]);
    for (uint16_t i = 0; i < n; i++) printf(" %02X", m[i]);
    putchar('\n');
    fflush(stdout);
}
static void plat_click(void *c, bool accent, uint8_t vol) { (void)c; synth_click(&synth, accent, vol); }
static void plat_off(void *c) { (void)c; printf("power off\n"); fflush(stdout); }

/* ---------------- audio ---------------- */
static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4, riff = 36 + data, sr = SR, br = SR * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static void advance_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        now_us += 1000;
        piano_tick(&piano, 1);
        for (int k = 0; k < KB_KEYS; k++) {
            if (pending_release[k] && --pending_release[k] == 0) {
                piano_key_edge(&piano, (uint8_t)k, 2, false, now_us);
                piano_key_edge(&piano, (uint8_t)k, 1, false, now_us + 500);
            }
        }
        float buf[2 * (SR / 1000 + 1)];
        int frames = SR / 1000 + ((i % 10) == 0 ? 1 : 0); /* 44.1 frames per ms on average */
        int fin[64];
        int nf = synth_render(&synth, buf, frames, fin, 64);
        for (int k = 0; k < nf; k++) voice_release_finished(&piano.voices, fin[k]);
        if (wav) {
            for (int f = 0; f < frames * 2; f++) {
                int16_t s = (int16_t)(buf[f] * 32000.0f);
                fwrite(&s, 2, 1, wav);
            }
            wav_frames += (uint32_t)frames;
        }
    }
}

/* travel time that produces roughly the requested scanner velocity */
static uint32_t travel_for_velocity(int vel)
{
    for (uint32_t t = 1000; t < 200000; t += 50)
        if (key_scanner_velocity(&piano.scanner, t) <= vel)
            return t;
    return 120000;
}

/* feed the queued ACKs back; each may release the next frame, which queues its own ACK */
static void ack_drain(void)
{
    while (ackq_n) {
        uint8_t port = ackq[0].port, k[3];
        memcpy(k, ackq[0].key, 3);
        memmove(ackq, ackq + 1, (ackq_n - 1) * sizeof ackq[0]);
        ackq_n--;
        const uint8_t a[] = {0xF0, 0x40, 0x7F, 0x07, 0x08, 0x02, k[0], k[1], k[2], 0xF7};
        piano_midi_in(&piano, (midi_port_t)port, a, sizeof a);
    }
}

static int parse_port(const char *s)
{
    for (int i = 0; i < MIDI_PORT_COUNT; i++)
        if (strcmp(s, PORT_NAMES[i]) == 0) return i;
    return -1;
}

/* The piano's panel-UI relay (cave relay_cave.c in slot payload ota_main, feat2 0x04; REMOTE_PROTOCOL.md 10.7):
 * 0x7D ops 0x30-0x3F from a controller port go to the panel port as they came, that port becomes the reply port;
 * the panel's replies 0x70-0x7F go back to it.  Rules R1-R3 (frames over 256 B and other directions dropped); no
 * TX rings here, so no R4.  Simulator only - lets the app's sender (panel UI update, panel screen report /
 * snapshot) talk to a panel page through this process (app scripts/panelsim.ts).  The core never sees them. */
static int relay_src = -1;
static bool sim_relay(int port, const uint8_t *m, uint16_t n)
{
    static const uint8_t tag[] = {0xF0, 0x7D, 0x43, 0x41, 0x37, 0x39};
    const int panel = MIDI_PORT_PANEL;
    if (n < 8 || memcmp(m, tag, sizeof tag) != 0 || m[n - 1] != 0xF7)
        return false;
    uint8_t op = m[6];
    bool req = op >= 0x30 && op <= 0x3F, rep = op >= 0x70 && op <= 0x7F;
    if (!req && !rep)
        return false;
    if (n > 256)
        return true;
    if (req) {
        if (port == panel) return true;
        relay_src = port;
        plat_tx(NULL, (midi_port_t)panel, m, n);
        return true;
    }
    if (port == panel && relay_src >= 0)
        plat_tx(NULL, (midi_port_t)relay_src, m, n);
    return true;
}

static void handle_line(char *line)
{
    char *cmd = strtok(line, " \t\r\n");
    if (!cmd || cmd[0] == '#') return;
    if (strcmp(cmd, "midi") == 0) {
        char *ps = strtok(NULL, " \t\r\n");
        int port = ps ? parse_port(ps) : -1;
        if (port < 0) { printf("err bad port\n"); return; }
        uint8_t bytes[1024];
        uint16_t n = 0;
        for (char *h; (h = strtok(NULL, " \t\r\n")) && n < sizeof bytes;)
            bytes[n++] = (uint8_t)strtoul(h, NULL, 16);
        if (sim_relay(port, bytes, n))
            return;
        piano_midi_in(&piano, (midi_port_t)port, bytes, n);
    } else if (strcmp(cmd, "key") == 0) {
        char *a = strtok(NULL, " \t\r\n"), *b = strtok(NULL, " \t\r\n"), *c = strtok(NULL, " \t\r\n");
        if (!a || !b) return;
        int key = atoi(a) - KB_LOWEST_NOTE;
        if (key < 0 || key >= KB_KEYS) return;
        uint32_t travel = travel_for_velocity(atoi(b));
        piano_key_edge(&piano, (uint8_t)key, 1, true, now_us);
        piano_key_edge(&piano, (uint8_t)key, 2, true, now_us + travel);
        if (c) pending_release[key] = atoi(c);
    } else if (strcmp(cmd, "up") == 0) {
        char *a = strtok(NULL, " \t\r\n");
        int key = a ? atoi(a) - KB_LOWEST_NOTE : -1;
        if (key < 0 || key >= KB_KEYS) return;
        piano_key_edge(&piano, (uint8_t)key, 2, false, now_us);
        piano_key_edge(&piano, (uint8_t)key, 1, false, now_us + 500);
    } else if (strcmp(cmd, "pedal") == 0) {
        char *a = strtok(NULL, " \t\r\n"), *b = strtok(NULL, " \t\r\n");
        if (!a || !b) return;
        pedal_id_t id = strcmp(a, "soft") == 0 ? PEDAL_SOFT : (strcmp(a, "sostenuto") == 0 ? PEDAL_SOSTENUTO : PEDAL_DAMPER);
        int v = atoi(b);
        uint16_t lo = piano.pedals.adc_min[id], hi = piano.pedals.adc_max[id];
        piano_pedal(&piano, id, (uint16_t)(lo + (hi - lo) * v / 127));
    } else if (strcmp(cmd, "wait") == 0) {
        char *a = strtok(NULL, " \t\r\n");
        advance_ms(a ? (uint32_t)atoi(a) : 1);
    } else if (strcmp(cmd, "status") == 0) {
        int son = 0;
        float emax = 0;
        for (int i = 0; i < SYNTH_VOICES; i++)
            if (synth.v[i].on) { son++; if (synth.v[i].env * synth.v[i].amp > emax) emax = synth.v[i].env * synth.v[i].amp; }
        printf("status synth=%d level=%.4f voices=%d mode=%d metro=%d bpm=%d rec=%d song=%d sound=%d/%d/%d tune=%.1f"
               " music=%d usb=%d events=%u ble=%d lock=%d\n",
               son, emax, voice_active_count(&piano.voices), piano.layout.mode, piano.metro.running, piano.metro.bpm,
               piano.rec.state, piano.rec.cur_song, piano.layout.main.msb, piano.layout.main.lsb,
               piano.layout.main.program, piano.tuning.master_hz, piano.music_mode, piano.usb_mounted,
               piano.rec.song[piano.rec.cur_song].count, piano.ble.enabled, piano.lock);
    } else if (strcmp(cmd, "usb") == 0) {
        char *a = strtok(NULL, " \t\r\n");
        if (!have_usb) { printf("err no --usb directory\n"); fflush(stdout); return; }
        usb_host.attached = a && strcmp(a, "attach") == 0;
    } else if (strcmp(cmd, "ls") == 0) {
        char name[FS_NAME_MAX];
        uint32_t size;
        for (uint32_t i = 0; have_usb && usb_fs.list(usb_fs.ctx, i, name, sizeof name, &size); i++)
            printf("file %u %s %u\n", i, name, size);
        printf("ls end\n");
    } else if (strcmp(cmd, "bt") == 0) {
        char *rest = strtok(NULL, "\r\n");
        if (rest) piano_bt_line(&piano, rest, (uint16_t)strlen(rest));
    } else if (strcmp(cmd, "btlink") == 0) {
        char *a = strtok(NULL, " \t\r\n");
        piano_bt_link(&piano, a && atoi(a) != 0);
    } else if (strcmp(cmd, "apo") == 0) {
        printf("apo bt=%s setting=%u idle_ms=%u holding=%d held_ms=%u capped=%d standby=%d"
               " policy=%s/%u/%d/%u\n",
               bt_audio_state_name(bt_audio_state(&piano.bt)), piano.power.setting, piano.power.idle_ms,
               piano.apo_hold.holding, piano.apo_hold.held_ms, piano.apo_hold.capped, piano.sb.state,
               piano.policy.apo_standby ? "standby" : "off", piano.policy.hold.max_hold_ms,
               piano.policy.hold.hold_connected, piano.policy.standby_cap_ms);
    } else if (strcmp(cmd, "standby") == 0) {
        char *a = strtok(NULL, " \t\r\n");
        if (a && strcmp(a, "enter") == 0) piano_standby_enter(&piano);
        else if (a && strcmp(a, "wake") == 0) piano_standby_wake(&piano, SB_WAKE_POWER_BUTTON);
        else if (a && strcmp(a, "off") == 0) piano_hard_off(&piano);
        printf("standby state=%d cap_ms=%u t_ms=%u amp=%d mute=%d bt_audio=%d\n", piano.sb.state, piano.sb.cap_ms,
               piano.sb.t_ms, hal_power_sim.amp, hal_power_sim.mute, hal_power_sim.bt_audio);
    } else if (strcmp(cmd, "ack") == 0) {
        char *a = strtok(NULL, " \t\r\n");
        auto_ack = a && strcmp(a, "auto") == 0;
        ackq_n = 0;
    } else if (strcmp(cmd, "quit") == 0) {
        if (wav) { wav_header(wav, wav_frames); fclose(wav); }
        exit(0);
    } else {
        printf("err unknown command %s\n", cmd);
    }
    ack_drain();
    fflush(stdout);
}

int main(int argc, char **argv)
{
    bool realtime = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            wav = fopen(argv[++i], "wb");
            if (wav) wav_header(wav, 0);
        } else if (strcmp(argv[i], "--flash") == 0 && i + 1 < argc) {
            flash_path = argv[++i];
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (strcmp(argv[i], "--usb") == 0 && i + 1 < argc) {
            hostfs_bind(&usb_host, argv[++i], &usb_fs);
            have_usb = true;
        } else if (strcmp(argv[i], "--realtime") == 0) {
            realtime = true;
        }
    }
    memset(flash_mem, 0xFF, sizeof flash_mem);
    if (flash_path) {
        FILE *f = fopen(flash_path, "rb");
        if (f) {
            if (fread(flash_mem, 1, sizeof flash_mem, f) != sizeof flash_mem)
                memset(flash_mem, 0xFF, sizeof flash_mem);
            fclose(f);
        }
    }
    kv_mount(&kv, &FLASH, 0, 1);
    synth_init(&synth, SR);
    synth_bind(&synth, &engine);
    hal_power_sim_reset();
    static standby_ops_t sb_ops;
    hal_standby_ops(&sb_ops);
    piano_platform_t plat = {.standby = &sb_ops, .midi_tx = plat_tx, .metronome_click = plat_click, .power_off = plat_off,
                             .save_song = plat_save_song, .load_song = plat_load_song,
                             .smf_buf = smf_buf, .smf_cap = sizeof smf_buf,
                             .usb = have_usb ? &usb_fs : NULL, .dsp_version = "SIM-SYNTH 1.0",
                             .fw_checksum = 0x00C079A0u};
    piano_init(&piano, &plat, &engine, &kv, rec_pool, rec_stage);
    printf("ready OpenGrand sim %s\n", CA79NG_VERSION);
    fflush(stdout);

    char line[4096];
    auto_ack = !realtime;
    if (!realtime) {
        while (fgets(line, sizeof line, stdin))
            handle_line(line);
        if (wav) { wav_header(wav, wav_frames); fclose(wav); }
        return 0;
    }
    /* realtime: 1 ms ticks driven by the monotonic clock, stdin polled */
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    char acc[8192];
    size_t accn = 0;
    for (;;) {
        if (poll(&pfd, 1, 1) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            ssize_t r = read(STDIN_FILENO, acc + accn, sizeof acc - accn - 1);
            if (r <= 0) break;
            accn += (size_t)r;
            char *nl;
            while ((nl = memchr(acc, '\n', accn))) {
                size_t l = (size_t)(nl - acc) + 1;
                memcpy(line, acc, l);
                line[l] = 0;
                memmove(acc, acc + l, accn - l);
                accn -= l;
                handle_line(line);
            }
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
        if (ms > 0) {
            advance_ms((uint32_t)(ms > 100 ? 100 : ms));
            last = now;
        }
    }
    return 0;
}
