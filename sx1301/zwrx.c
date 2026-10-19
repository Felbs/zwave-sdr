/* zwrx: Z-Wave R2 (908.40 MHz, 40 kbps FSK) receiver on the Nebra SX1301.
 *
 * The FSK modem syncs on 0x5555 (preamble; same pattern in either bit
 * polarity), captures a fixed block of raw bytes (no CRC, no whitening,
 * no length byte) and prints it as hex with the concentrator timestamp.
 * Frame finding / checksums are done in software (zwparse.py).
 *
 * usage: zwrx [seconds] [radio_freq_hz] [if_offset_hz]
 */
#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include "loragw_hal.h"
#include "loragw_reg.h"

#define RAWLEN 80

static volatile int quit = 0;
static void sig(int s) { (void)s; quit = 1; }

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    uint32_t rf = argc > 2 ? (uint32_t)atol(argv[2]) : 908600000;
    int32_t ifo = argc > 3 ? atoi(argv[3]) : -200000;
    signal(SIGINT, sig); signal(SIGTERM, sig);

    struct lgw_conf_board_s b = { .lorawan_public = true, .clksrc = 1 };
    if (lgw_board_setconf(b)) { fprintf(stderr, "board conf failed\n"); return 1; }
    struct lgw_conf_rxrf_s r = { .enable = true, .freq_hz = rf, .rssi_offset = -159.0,
                                 .type = LGW_RADIO_TYPE_SX1257, .tx_enable = true,
                                 .tx_notch_freq = 129000 };
    if (lgw_rxrf_setconf(0, r)) { fprintf(stderr, "rf0 conf failed\n"); return 1; }
    r.tx_enable = false;
    if (lgw_rxrf_setconf(1, r)) { fprintf(stderr, "rf1 conf failed\n"); return 1; }
    struct lgw_conf_rxif_s off = { .enable = false };
    for (int i = 0; i < 9; i++) lgw_rxif_setconf(i, off);
    struct lgw_conf_rxif_s f = { .enable = true, .rf_chain = 0, .freq_hz = ifo,
                                 .bandwidth = BW_125KHZ, .datarate = 40000,
                                 .sync_word_size = 2, .sync_word = 0x5555 };
    if (lgw_rxif_setconf(9, f)) { fprintf(stderr, "fsk if conf failed\n"); return 1; }
    if (lgw_start() != LGW_HAL_SUCCESS) { fprintf(stderr, "lgw_start failed\n"); return 2; }

    /* raw Z-Wave framing: fixed length, no CRC, no DC-free coding */
    lgw_reg_w(LGW_FSK_PKT_MODE, 0);
    lgw_reg_w(LGW_FSK_CRC_EN, 0);
    lgw_reg_w(LGW_FSK_DCFREE_ENC, 0);
    lgw_reg_w(LGW_FSK_PKT_LENGTH, RAWLEN);
    fprintf(stderr, "zwrx: listening %.4f MHz, 40 kbps FSK, %d s\n", (rf + ifo) / 1e6, secs);

    struct lgw_pkt_rx_s pk[8];
    time_t end = time(NULL) + secs;
    while (!quit && time(NULL) < end) {
        int n = lgw_receive(8, pk);
        if (n < 0) { fprintf(stderr, "lgw_receive error\n"); break; }
        for (int i = 0; i < n; i++) {
            if (pk[i].modulation != MOD_FSK) continue;
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            printf("%ld.%03ld cnt_us=%u if=%u rssi=%.1f status=%u size=%u ",
                   (long)ts.tv_sec, ts.tv_nsec / 1000000, pk[i].count_us, pk[i].if_chain,
                   pk[i].rssi, pk[i].status, pk[i].size);
            for (int k = 0; k < pk[i].size; k++) printf("%02x", pk[i].payload[k]);
            printf("\n");
            fflush(stdout);
        }
        if (n == 0) { struct timespec w = {0, 2000000}; nanosleep(&w, NULL); }
    }
    lgw_stop();
    return 0;
}
