/* zwtx: send Z-Wave R2 (908.40 MHz, 40 kbps, ±20 kHz) frames from the Nebra SX1301.
 * Uses the patched HAL (lgw_fsk_raw): no length byte, no CRC, no whitening.
 * Sync word = 0x55F0 (last preamble byte + Z-Wave SOF). Real Z-Wave uses the
 * opposite bit polarity to the SX1301 TX (measured: real controller traffic decodes inverted
 * on the Nebra, our own TX does not), so by default the sync word and every byte
 * are inverted (0xAA0F, b^0xFF). ZW_NOINV=1 sends the SX1301-native polarity.
 *
 * usage: zwtx <count> <interval_ms> <hexframe-without-checksum>
 * The XOR checksum (init 0xFF) is appended here.
 */
#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "loragw_hal.h"
#include "loragw_reg.h"
#include "loragw_aux.h"

extern int lgw_fsk_raw;

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: zwtx count interval_ms hexframe\n"); return 1; }
    int count = atoi(argv[1]), ivl = atoi(argv[2]);
    const char *hx = argv[3];
    uint8_t fr[64]; int n = 0;
    for (size_t i = 0; i + 1 < strlen(hx) && n < 63; i += 2) { unsigned v; sscanf(hx + i, "%2x", &v); fr[n++] = v; }
    uint8_t cs = 0xFF; for (int i = 0; i < n; i++) cs ^= fr[i];
    fr[n++] = cs;
    int inv = getenv("ZW_NOINV") ? 0 : 1;
    if (fr[7] != n) fprintf(stderr, "warning: length byte %d != frame length %d\n", fr[7], n);

    struct lgw_conf_board_s b = { .lorawan_public = true, .clksrc = 1 };
    lgw_board_setconf(b);
    struct lgw_conf_rxrf_s r = { .enable = true, .freq_hz = 908600000, .rssi_offset = -159.0,
                                 .type = LGW_RADIO_TYPE_SX1257, .tx_enable = true, .tx_notch_freq = 129000 };
    lgw_rxrf_setconf(0, r);
    r.tx_enable = false;
    lgw_rxrf_setconf(1, r);
    struct lgw_conf_rxif_s off = { .enable = false };
    for (int i = 0; i < 9; i++) lgw_rxif_setconf(i, off);
    struct lgw_conf_rxif_s f = { .enable = true, .rf_chain = 0, .freq_hz = -200000,
                                 .bandwidth = BW_125KHZ, .datarate = 40000,
                                 .sync_word_size = 2, .sync_word = 0x55F0 };
    if (inv) f.sync_word = 0xAA0F;
    lgw_rxif_setconf(9, f);
    struct lgw_tx_gain_lut_s lut = { .size = 1 };
    lut.lut[0] = (struct lgw_tx_gain_s){ .dig_gain = 0, .pa_gain = 0, .dac_gain = 3, .mix_gain = 8, .rf_power = -6 };
    lgw_txgain_setconf(&lut);
    if (lgw_start() != LGW_HAL_SUCCESS) { fprintf(stderr, "lgw_start failed\n"); return 2; }
    lgw_fsk_raw = getenv("ZW_STOCK") ? 0 : 1;

    struct lgw_pkt_tx_s p;
    memset(&p, 0, sizeof p);
    p.freq_hz = 908400000; p.tx_mode = IMMEDIATE; p.rf_chain = 0; p.rf_power = -6;
    p.modulation = MOD_FSK; p.datarate = 40000; p.f_dev = 20; p.preamble = 10;
    p.size = n; memcpy(p.payload, fr, n);
    if (inv) for (int i = 0; i < n; i++) p.payload[i] ^= 0xFF;
    fprintf(stderr, "zwtx: %d x %d bytes (%s polarity):", count, n, inv ? "Z-Wave" : "native");
    for (int i = 0; i < n; i++) fprintf(stderr, " %02x", fr[i]);
    fprintf(stderr, "\n");
    for (int k = 0; k < count; k++) {
        int rc = lgw_send(p);
        uint8_t st = 0;
        do { wait_ms(2); lgw_status(TX_STATUS, &st); } while (st == TX_EMITTING || st == TX_SCHEDULED);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        printf("%ld.%03ld sent %d rc=%d\n", (long)ts.tv_sec, ts.tv_nsec / 1000000, k, rc);
        fflush(stdout);
        wait_ms(ivl);
    }
    lgw_stop();
    return 0;
}
