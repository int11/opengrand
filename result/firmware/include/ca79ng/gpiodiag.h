/* OpenGrand — GPIO diagnostic (read only): a snapshot of the GPIO banks the partial-standby design depends on, and a
 * change log of the named pins with an optional notice to one bound controller.  Nothing here writes a pin, a
 * direction, a pin-mux register or stock RAM.
 *
 * The open pins it is meant to settle are those of the power GPIO analysis (op A / op B - op C, the toggle, is NOT
 * here).
 *
 * Register layout (OMAP-L13x GPIO header - [inferred] from the stock
 * code's use, the same assumption as every GPIO name in the records): base 0x01E26000, per bank pair DIR +0, OUT +4,
 * SET +8, CLR +0xC, IN +0x10; pair 23 at +0x38, 45 at +0x60, 67 at +0x88.  Addresses read by the stock code and cited
 * in the records: IN_DATA23 0x01E26048 (GP3[8] / GP2[10] polls), OUT_DATA67 0x01E2608C (amp / mute / NOR window),
 * IN_DATA45 0x01E26070 (headphones), DIR / OUT of the three pairs (BootProgram DSP-stage table).
 * The pin-mux block PINMUX0..19 0x01C14120..0x01C1416C: only PINMUX2 0x01C14128 is read by the stock (BT detection); the other 19 addresses are the SoC data sheet's [unverified on this image] - reads of SYSCFG0
 * have no side effect (the KICK lock guards writes only).
 *
 * Stock RAM bytes reported as they are (REA): 0x4139E7FA headphone state, 0x4139E7FF GP3[8]
 * sample of the 20 ms tick, 0x4139E7FD its event (1 = back to 0, 2 = went 1), 0x4139E80A model byte, 0x4139E80B chip-A
 * reg 0x10 base value (0xDD / 0xDC: which codec path the boot took). */
#ifndef CA79NG_GPIODIAG_H
#define CA79NG_GPIODIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MMIO (read only) */
#define GD_A_GPIO      0x01E26000u
#define GD_A_DIR23     (GD_A_GPIO + 0x38u)
#define GD_A_OUT23     (GD_A_GPIO + 0x3Cu)
#define GD_A_IN23      (GD_A_GPIO + 0x48u)
#define GD_A_DIR45     (GD_A_GPIO + 0x60u)
#define GD_A_OUT45     (GD_A_GPIO + 0x64u)
#define GD_A_IN45      (GD_A_GPIO + 0x70u)
#define GD_A_DIR67     (GD_A_GPIO + 0x88u)
#define GD_A_OUT67     (GD_A_GPIO + 0x8Cu)
#define GD_A_IN67      (GD_A_GPIO + 0x98u)
#define GD_A_PINMUX0   0x01C14120u
#define GD_PINMUX_N    20u
/* stock RAM (read only) */
#define GD_A_HP        0x4139E7FAu
#define GD_A_GP3_LEVEL 0x4139E7FFu
#define GD_A_GP3_EVENT 0x4139E7FDu
#define GD_A_MODEL     0x4139E80Au
#define GD_A_CHIPA     0x4139E80Bu

/* the watched signals: index -> register word, bit (the change log and the notice report the index) */
enum {
    GD_W_IN23 = 0, GD_W_IN45 = 1, GD_W_OUT45 = 2, GD_W_OUT67 = 3, GD_WORDS = 4
};
enum {
    GD_S_GP3_8 = 0,    /* IN23 bit 24   mute request input                                */
    GD_S_GP3_1 = 1,    /* IN23 bit 17   power switch (1 = released)                        */
    GD_S_GP5_14 = 2,   /* IN45 bit 30   headphone detect                                   */
    GD_S_GP6_15 = 3,   /* OUT67 bit 15  speaker path (amp)                                 */
    GD_S_GP6_11 = 4,   /* OUT67 bit 11  output mute release (1 = sound)                    */
    GD_S_GP6_13 = 5,   /* OUT67 bit 13  codec line (chip A power / reset side)             */
    GD_S_GP7_12 = 6,   /* OUT67 bit 28  codec line (chip B side)                           */
    GD_S_GP4_15 = 7,   /* OUT45 bit 15  USB memory port VBUS                               */
    GD_S_GP5_15 = 8,   /* OUT45 bit 31  BT audio module                                    */
    GD_S_GP4_13 = 9,   /* OUT45 bit 13  BT MIDI module power                               */
    GD_SIGNALS = 10
};

/* change log */
#define GD_RING 16u
#define GD_EVENT 8   /* seq u16, t_ms u32, signal u8, level u8 */
typedef struct {
    uint16_t seq;     /* 1, 2, ... (wraps; 0 is never used)          */
    uint32_t t_ms;    /* ms since this payload started                */
    uint8_t sig;      /* GD_S_*                                       */
    uint8_t level;    /* the new level, 0 / 1                         */
} gd_event_t;

typedef struct {
    bool started;              /* the first sample is the baseline: no event        */
    uint16_t levels;           /* bit i = last level of signal i                    */
    uint16_t seq;              /* last event's seq (0 = none yet)                   */
    uint8_t head;              /* next ring slot                                    */
    uint8_t n;                 /* events in the ring (<= GD_RING)                   */
    gd_event_t ring[GD_RING];
    uint32_t samples;          /* gd_sample calls                                   */
} gd_t;

void gd_init(gd_t *g);
/* one pass: the four watched words; appends an event per signal whose level changed (none on the first call) and
 * returns how many it appended */
unsigned gd_sample(gd_t *g, uint32_t t_ms, const uint32_t w[GD_WORDS]);
/* the k-th newest event (0 = newest), NULL past what the ring holds */
const gd_event_t *gd_newest(const gd_t *g, unsigned k);
/* the level of signal `sig` in these words */
uint8_t gd_level(unsigned sig, const uint32_t w[GD_WORDS]);
void gd_encode_event(const gd_event_t *e, uint8_t out[GD_EVENT]);

/* what the port read for a snapshot */
typedef struct {
    uint32_t dir[3], out[3], in[3];   /* pairs 23, 45, 67 */
    uint8_t hp, gp3_level, gp3_event, model, chip_a;
    uint32_t pinmux[GD_PINMUX_N];
} gd_regs_t;

/* op 0x17 snapshot record (REMOTE_PROTOCOL §10.11), little-endian */
#define GD_LAYOUT 1
#define GD_SNAP 130
#define GD_F_NOTIFY 0x01   /* change notices are on (set by the port)        */
#define GD_F_STARTED 0x02  /* the change log has its baseline                */
/* raw: 0 layout · 1 flags · 2 notice port (0..3, 0x7F none) · 3 seq u16 (last event) · 5 DIR23 · 9 OUT23 · 13 IN23 ·
 * 17 DIR45 · 21 OUT45 · 25 IN45 · 29 DIR67 · 33 OUT67 · 37 IN67 (u32 each) · 41 hp · 42 GP3[8] sample · 43 GP3[8]
 * event · 44 model byte · 45 chip-A base · 46 PINMUX0..19 (u32 each) · 126 t_ms u32 */
void gd_encode_snapshot(const gd_t *g, const gd_regs_t *r, uint8_t flags, uint8_t port, uint32_t t_ms,
                        uint8_t raw[GD_SNAP]);
/* op 17 01 change log: 0 layout · 1 n · 2 seq u16 (last) · 4 n events oldest first (GD_EVENT each) */
#define GD_LOG_MAX (4 + GD_RING * GD_EVENT)
size_t gd_encode_log(const gd_t *g, uint8_t raw[GD_LOG_MAX]);

#endif
