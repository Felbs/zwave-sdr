/* zwack1302: listen for Z-Wave R2 frames on an SX1302 (RAK5146 over USB) and ACK the
 * ones addressed to MY_NODE that ask for it, as fast as the host can turn around.
 * RX: FSK sync 0x5555 (polarity-agnostic), fixed 24-byte blocks, no CRC/whitening;
 * the SOF and checksum are found here. TX: patched HAL (lgw_fsk_raw), Z-Wave polarity.
 * usage: zwack1302 <seconds> [my_node]
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "loragw_hal.h"
#include "loragw_reg.h"

extern int lgw_fsk_raw;
#define BLK (getenv("ZW_BLK") ? atoi(getenv("ZW_BLK")) : 12)
#define BLKMAX 64

static int find_frame(const uint8_t *raw, int n, uint8_t *fr, int *flen) {
    for (int inv = 0; inv < 2; inv++) {
        uint8_t bits[(BLKMAX + 2) * 8];
        for (int i = 0; i < n * 8; i++) bits[i] = ((raw[i / 8] >> (7 - i % 8)) & 1) ^ inv;
        static const uint8_t pat[16] = {0,1,0,1,0,1,0,1,1,1,1,1,0,0,0,0};
        for (int i = 0; i + 16 + 80 <= n * 8 && i < 1; i++) {
            if (memcmp(bits + i, pat, 16)) continue;
            int j = i + 16, nb = (n * 8 - j) / 8;
            for (int k = 0; k < nb; k++) { uint8_t v = 0; for (int b = 0; b < 8; b++) v = (v << 1) | bits[j + 8 * k + b]; fr[k] = v; }
            int L = fr[7];
            if (L < 10 || L > nb) continue;
            uint8_t cs = 0xFF; for (int k = 0; k < L - 1; k++) cs ^= fr[k];
            if (cs != fr[L - 1]) continue;
            *flen = L; return inv;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 30;
    int me = argc > 2 ? atoi(argv[2]) : 2;
    struct lgw_conf_board_s b; memset(&b, 0, sizeof b);
    b.lorawan_public = true; b.clksrc = 0; b.com_type = LGW_COM_USB; strcpy(b.com_path, "/dev/ttyACM0");
    lgw_board_setconf(&b);
    struct lgw_conf_rxrf_s r; memset(&r, 0, sizeof r);
    r.enable = true; r.freq_hz = 908600000; r.rssi_offset = -215.4; r.type = LGW_RADIO_TYPE_SX1250; r.tx_enable = true;
    r.rssi_tcomp.coeff_c = 20.41; r.rssi_tcomp.coeff_d = 2162.56;
    lgw_rxrf_setconf(0, &r); r.tx_enable = false; lgw_rxrf_setconf(1, &r);
    struct lgw_conf_rxif_s f; memset(&f, 0, sizeof f);
    f.enable = true; f.rf_chain = 0; f.freq_hz = -200000; f.bandwidth = BW_125KHZ; f.datarate = 40000;
    f.sync_word_size = 2; f.sync_word = 0xAA0F;   /* Z-Wave SOF, chip-native polarity; TX uses it too */
    if (lgw_rxif_setconf(9, &f)) { fprintf(stderr, "fsk if failed\n"); return 1; }
    struct lgw_tx_gain_lut_s lut; memset(&lut, 0, sizeof lut);
    lut.size = 1; lut.lut[0].pwr_idx = 3; lut.lut[0].mix_gain = 5; lut.lut[0].dac_gain = 3;
    lgw_txgain_setconf(0, &lut);
    if (lgw_start() != LGW_HAL_SUCCESS) { fprintf(stderr, "lgw_start failed\n"); return 2; }
    lgw_reg_w(SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FSK_CFG_0_PKT_MODE, 0);
    lgw_reg_w(SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FSK_CFG_0_CRC_EN, 0);
    lgw_reg_w(SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FSK_CFG_0_DCFREE_ENC, 0);
    lgw_reg_w(SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FSK_PKT_LENGTH_PKT_LENGTH, BLK);
    lgw_fsk_raw = 1;
    fprintf(stderr, "zwack1302: node %d listening 908.40 MHz for %d s\n", me, secs);

    struct lgw_pkt_rx_s pk[8];
    struct lgw_pkt_tx_s tx; memset(&tx, 0, sizeof tx);
    tx.freq_hz = 908400000; tx.tx_mode = IMMEDIATE; tx.rf_chain = 0; tx.rf_power = 0;
    tx.modulation = MOD_FSK; tx.datarate = 40000; tx.f_dev = 20; tx.preamble = 10;
    time_t end = time(NULL) + secs;
    int got = 0, acked = 0;
    while (time(NULL) < end) {
        int n = lgw_receive(8, pk);
        if (n <= 0) continue;
        uint32_t t_fetch; lgw_get_instcnt(&t_fetch);
        for (int i = 0; i < n; i++) {
            if (pk[i].modulation != MOD_FSK) continue;
            uint8_t fr[BLKMAX + 2], raw[BLKMAX + 2]; int L = 0;
            int nraw = pk[i].size > BLKMAX ? BLKMAX : pk[i].size;
            raw[0] = 0xAA; raw[1] = 0x0F; memcpy(raw + 2, pk[i].payload, nraw);   /* put back the sync the modem consumed */
            int inv = find_frame(raw, nraw + 2, fr, &L);
            got++;
            if (inv < 0) { printf("rx cnt=%u size=%u no-frame\n", pk[i].count_us, pk[i].size); continue; }
            int src = fr[4], dst = fr[8], ackreq = fr[5] & 0x40, seq = fr[6] & 0x0F;
            uint32_t t_send = 0;
            if (dst == me && ackreq) {
                uint8_t a[10];
                memcpy(a, fr, 4); a[4] = me; a[5] = 0x03; a[6] = seq; a[7] = 10; a[8] = src;
                uint8_t cs = 0xFF; for (int k = 0; k < 9; k++) cs ^= a[k]; a[9] = cs;
                tx.size = 10; for (int k = 0; k < 10; k++) tx.payload[k] = a[k] ^ 0xFF;   /* Z-Wave polarity */
                lgw_get_instcnt(&t_send);
                int rc = lgw_send(&tx);
                uint8_t st = 0; int polls = 0;
                do { lgw_status(0, TX_STATUS, &st); polls++; } while ((st == TX_SCHEDULED || st == TX_EMITTING) && polls < 2000);
                uint32_t t_done; lgw_get_instcnt(&t_done);
                printf("[send rc=%d last_status=%u polls=%d done +%u us] ", rc, st, polls, t_done - pk[i].count_us);
                acked++;
            }
            printf("rx cnt=%u fetched=+%u us %s %d->%d seq=%d ackreq=%d inv=%d%s",
                   pk[i].count_us, t_fetch - pk[i].count_us, ackreq ? "singlecast" : "frame", src, dst, seq, ackreq ? 1 : 0, inv,
                   t_send ? "" : "\n");
            if (t_send) printf(" -> ACK queued at +%u us after rx timestamp\n", t_send - pk[i].count_us);
            fflush(stdout);
        }
    }
    lgw_stop();
    fprintf(stderr, "blocks %d, acked %d\n", got, acked);
    return 0;
}
