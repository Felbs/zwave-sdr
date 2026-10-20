/* zwtx1302: Z-Wave R2 frames (908.40 MHz, 40 kbps, +-20 kHz FSK) from an SX1302/SX1250
 * concentrator over USB (RAK5146). Patched HAL (lgw_fsk_raw): fixed length, no CRC,
 * no whitening, no shaping, no length byte. Bytes and sync word inverted by default
 * (real Z-Wave sends a logical 1 on the lower tone); ZW_NOINV=1 for chip-native.
 * usage: zwtx1302 <count> <interval_ms> <hexframe-without-checksum>
 * env: ZW_PWRIDX (SX1250 power index, default 3), ZW_COM (default /dev/ttyACM0)
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "loragw_hal.h"

extern int lgw_fsk_raw;

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: zwtx1302 count interval_ms hexframe\n"); return 1; }
    int count = atoi(argv[1]), ivl = atoi(argv[2]);
    const char *hx = argv[3];
    uint8_t fr[64]; int n = 0;
    for (size_t i = 0; i + 1 < strlen(hx) && n < 63; i += 2) { unsigned v; sscanf(hx + i, "%2x", &v); fr[n++] = v; }
    uint8_t cs = 0xFF; for (int i = 0; i < n; i++) cs ^= fr[i];
    fr[n++] = cs;
    int inv = getenv("ZW_NOINV") ? 0 : 1;
    int pidx = getenv("ZW_PWRIDX") ? atoi(getenv("ZW_PWRIDX")) : 3;

    struct lgw_conf_board_s b; memset(&b, 0, sizeof b);
    b.lorawan_public = true; b.clksrc = 0; b.full_duplex = false; b.com_type = LGW_COM_USB;
    strncpy(b.com_path, getenv("ZW_COM") ? getenv("ZW_COM") : "/dev/ttyACM0", sizeof b.com_path - 1);
    if (lgw_board_setconf(&b)) { fprintf(stderr, "board conf failed\n"); return 1; }
    struct lgw_conf_rxrf_s r; memset(&r, 0, sizeof r);
    r.enable = true; r.freq_hz = 908600000; r.rssi_offset = -215.4; r.type = LGW_RADIO_TYPE_SX1250; r.tx_enable = true;
    r.rssi_tcomp.coeff_c = 20.41; r.rssi_tcomp.coeff_d = 2162.56;
    if (lgw_rxrf_setconf(0, &r)) { fprintf(stderr, "rf0 failed\n"); return 1; }
    r.tx_enable = false;
    if (lgw_rxrf_setconf(1, &r)) { fprintf(stderr, "rf1 failed\n"); return 1; }
    struct lgw_conf_rxif_s f; memset(&f, 0, sizeof f);
    f.enable = true; f.rf_chain = 0; f.freq_hz = -200000; f.bandwidth = BW_125KHZ; f.datarate = 40000;
    f.sync_word_size = 2; f.sync_word = inv ? 0xAA0F : 0x55F0;
    if (lgw_rxif_setconf(9, &f)) { fprintf(stderr, "fsk if failed\n"); return 1; }
    struct lgw_tx_gain_lut_s lut; memset(&lut, 0, sizeof lut);
    lut.size = 1; lut.lut[0].rf_power = 0; lut.lut[0].pa_gain = 0; lut.lut[0].pwr_idx = pidx; lut.lut[0].mix_gain = 5; lut.lut[0].dac_gain = 3;
    if (lgw_txgain_setconf(0, &lut)) { fprintf(stderr, "lut failed\n"); return 1; }
    if (lgw_start() != LGW_HAL_SUCCESS) { fprintf(stderr, "lgw_start failed\n"); return 2; }
    lgw_fsk_raw = 1;

    struct lgw_pkt_tx_s p; memset(&p, 0, sizeof p);
    p.freq_hz = 908400000; p.tx_mode = IMMEDIATE; p.rf_chain = 0; p.rf_power = 0;
    p.modulation = MOD_FSK; p.datarate = 40000; p.f_dev = 20; p.preamble = getenv("ZW_PREAMBLE") ? atoi(getenv("ZW_PREAMBLE")) : 10; p.size = n;
    memcpy(p.payload, fr, n);
    if (inv) for (int i = 0; i < n; i++) p.payload[i] ^= 0xFF;
    fprintf(stderr, "zwtx1302: %d x %d bytes, pwr_idx %d, %s polarity\n", count, n, pidx, inv ? "Z-Wave" : "native");
    for (int k = 0; k < count; k++) {
        int rc = lgw_send(&p);
        uint8_t st = 0;
        do { usleep(2000); lgw_status(0, TX_STATUS, &st); } while (st == TX_EMITTING || st == TX_SCHEDULED);
        printf("sent %d rc=%d\n", k, rc); fflush(stdout);
        usleep(ivl * 1000);
    }
    lgw_stop();
    return 0;
}
