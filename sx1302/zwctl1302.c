/* zwctl1302: a small Z-Wave R2 hub on an SX1302/SX1250 concentrator (RAK5146 over USB).
 *
 * The hub owns a network (4-byte Home ID, kept in ./home_id or $ZW_HOME_FILE) and is node 1.
 *   zwctl1302 listen SECONDS            log every frame, ACK the ones sent to us
 *   zwctl1302 add SECONDS               add a device whose owner has put it in "add" mode:
 *                                       wait for its node-information frame, assign it the next
 *                                       node ID on our network, then ask for its capabilities
 *   zwctl1302 get NODE                  thermostat heating setpoint (Thermostat Setpoint CC 0x43)
 *   zwctl1302 set NODE DEG_F            set the heating setpoint
 *   zwctl1302 beam NODE HASH [get]      1.1 s wake-up beam for a battery (FLiRS) node, then optional get
 *
 * Verified against Z-Wave JS source: protocol CC 0x01; NIF 0x01, RequestNIF 0x02, AssignIDs 0x03
 * (node ID, then Home ID big-endian); NIF = 3 capability bytes, basic, generic, [specific], CC list.
 * From memory of the public protocol description (confirm on the first live test): a device in
 * add mode broadcasts its NIF with node ID 0 under its own temporary Home ID; the hub answers
 * AssignIDs to that address, the device ACKs and switches; the hub then requests the NIF again.
 * Frame control: byte 5 = 0x40 ACK-request | header type (1 singlecast, 3 ACK); byte 6 low nibble = sequence.
 * Radio: 908.40 MHz, 40 kbit/s, +-20 kHz, Z-Wave polarity (bytes and sync word inverted on this chip).
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "loragw_hal.h"
#include "loragw_reg.h"

extern int lgw_fsk_raw;
#define BLK 64                /* fixed receive block: a real thermostat NIF is ~50 bytes */
#define ZW_HOME_FILE (getenv("ZW_HOME_FILE") ? getenv("ZW_HOME_FILE") : "home_id")
#define ME 1
static uint8_t HOME[4];
static uint8_t seqno = 1;

static uint32_t now_us(void) { uint32_t t; lgw_get_instcnt(&t); return t; }
static double wall(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

/* ---- frames ---------------------------------------------------------------------------- */
typedef struct { uint8_t home[4], src, dst, type, ackreq, seq, len, payload[64]; int plen; uint32_t t_us; } frame_t;

static int parse(const uint8_t *raw, int n, frame_t *f) {
    /* raw starts at the Home ID (the modem consumed preamble + SOF); bits are inverted on this chip */
    uint8_t b[BLK + 2];
    for (int i = 0; i < n; i++) b[i] = raw[i] ^ 0xFF;
    if (n < 10) return 0;
    int L = b[7];
    if (L < 10 || L > n) return 0;
    uint8_t cs = 0xFF; for (int i = 0; i < L - 1; i++) cs ^= b[i];
    if (cs != b[L - 1]) return 0;
    memcpy(f->home, b, 4); f->src = b[4]; f->type = b[5] & 0x0F; f->ackreq = !!(b[5] & 0x40);
    f->seq = b[6] & 0x0F; f->len = L; f->dst = b[8];
    f->plen = L - 10; memcpy(f->payload, b + 9, f->plen);
    return 1;
}

static struct lgw_pkt_tx_s txp;
static int send_frame(const uint8_t *home, uint8_t src, uint8_t dst, uint8_t type, int ackreq, uint8_t seq, const uint8_t *pl, int plen) {
    uint8_t fr[80]; int n = 0;
    memcpy(fr, home, 4); n = 4; fr[n++] = src; fr[n++] = (ackreq ? 0x40 : 0) | type; fr[n++] = seq & 0x0F;
    fr[n++] = 10 + plen; fr[n++] = dst; memcpy(fr + n, pl, plen); n += plen;
    uint8_t cs = 0xFF; for (int i = 0; i < n; i++) cs ^= fr[i]; fr[n++] = cs;
    txp.size = n; for (int i = 0; i < n; i++) txp.payload[i] = fr[i] ^ 0xFF;
    txp.preamble = 20;
    int rc = lgw_send(&txp);
    uint8_t st = 0; int polls = 0;
    do { lgw_status(0, TX_STATUS, &st); polls++; } while ((st == TX_SCHEDULED || st == TX_EMITTING) && polls < 5000);
    return rc;
}

static void show(const frame_t *f, const char *tag) {
    printf("%s t=%u home=%02x%02x%02x%02x %d->%d %s%s seq=%d payload=", tag, f->t_us, f->home[0], f->home[1], f->home[2], f->home[3],
           f->src, f->dst, f->type == 3 ? "ACK" : f->type == 1 ? "single" : "type?", f->ackreq ? "+ackreq" : "", f->seq);
    for (int i = 0; i < f->plen; i++) printf("%02x", f->payload[i]);
    printf("\n"); fflush(stdout);
}

/* receive one frame within timeout; ACK frames addressed to us on our network that ask for it */
static int recv_frame(double timeout_s, frame_t *f) {
    struct lgw_pkt_rx_s pk[8];
    double end = wall() + timeout_s;
    while (wall() < end) {
        int n = lgw_receive(8, pk);
        for (int i = 0; i < n; i++) {
            if (pk[i].modulation != MOD_FSK) continue;
            if (!parse(pk[i].payload, pk[i].size > BLK ? BLK : pk[i].size, f)) {
                printf("raw block size=%u:", pk[i].size);
                for (int k = 0; k < (pk[i].size > 24 ? 24 : pk[i].size); k++) printf(" %02x", pk[i].payload[k] ^ 0xFF);
                printf("\n"); fflush(stdout);
                continue;
            }
            f->t_us = pk[i].count_us;
            if (f->ackreq && f->dst == ME && !memcmp(f->home, HOME, 4) && f->type == 1)
                send_frame(HOME, ME, f->src, 3, 0, f->seq, NULL, 0);
            return 1;
        }
        if (n == 0) usleep(500);
    }
    return 0;
}

/* send with ACK request, wait for the ACK, retry */
static int send_acked(const uint8_t *home, uint8_t dst, const uint8_t *pl, int plen, frame_t *ack) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t s = seqno++ & 0x0F;
        uint32_t t0 = now_us();
        send_frame(home, ME, dst, 1, 1, s, pl, plen);
        double end = wall() + 0.25;
        while (wall() < end) {
            if (!recv_frame(end - wall(), ack)) break;
            if (ack->type == 3 && ack->src == dst && !memcmp(ack->home, home, 4)) {
                printf("   ACK from node %d after %u us (attempt %d)\n", dst, ack->t_us - t0, attempt + 1);
                return 1;
            }
            show(ack, "   (other)");
        }
    }
    return 0;
}

static void nif_print(const uint8_t *p, int n) {
    /* over-the-air NIF: cap0 cap1 cap2 generic [specific] command-classes (no basic-class byte) */
    if (n < 5) { printf("   short NIF\n"); return; }
    int specific = p[1] & 0x04;
    printf("   NIF: listening=%d routing=%d speed40k=%d FLiRS=%s beaming=%d security=%d generic=%02x specific=%02x\n",
           !!(p[0] & 0x80), !!(p[0] & 0x40), !!(p[0] & 0x10), (p[1] & 0x60) == 0x40 ? "1000ms" : (p[1] & 0x60) == 0x20 ? "250ms" : "no",
           !!(p[1] & 0x10), p[1] & 1, p[3], specific ? p[4] : 0);
    printf("   command classes:");
    for (int i = specific ? 5 : 4; i < n; i++) printf(" %02x", p[i]);
    printf("\n");
}

/* ---- radio ----------------------------------------------------------------------------- */
static int radio_start(void) {
    struct lgw_conf_board_s b; memset(&b, 0, sizeof b);
    b.lorawan_public = true; b.clksrc = 0; b.com_type = LGW_COM_USB; strcpy(b.com_path, "/dev/ttyACM0");
    lgw_board_setconf(&b);
    struct lgw_conf_rxrf_s r; memset(&r, 0, sizeof r);
    r.enable = true; r.freq_hz = 908600000; r.rssi_offset = -215.4; r.type = LGW_RADIO_TYPE_SX1250; r.tx_enable = true;
    r.rssi_tcomp.coeff_c = 20.41; r.rssi_tcomp.coeff_d = 2162.56;
    lgw_rxrf_setconf(0, &r); r.tx_enable = false; lgw_rxrf_setconf(1, &r);
    struct lgw_conf_rxif_s f; memset(&f, 0, sizeof f);
    f.enable = true; f.rf_chain = 0; f.freq_hz = -200000; f.bandwidth = BW_125KHZ; f.datarate = 40000;
    f.sync_word_size = 2; f.sync_word = 0xAA0F;          /* Z-Wave SOF in chip polarity; TX uses the same */
    if (lgw_rxif_setconf(9, &f)) return -1;
    struct lgw_tx_gain_lut_s lut; memset(&lut, 0, sizeof lut);
    int pidx = getenv("ZW_PWRIDX") ? atoi(getenv("ZW_PWRIDX")) : 3;
    lut.size = 1; lut.lut[0].pwr_idx = pidx; lut.lut[0].mix_gain = 5; lut.lut[0].dac_gain = 3;
    lgw_txgain_setconf(0, &lut);
    if (lgw_start() != LGW_HAL_SUCCESS) return -2;
    lgw_reg_w(SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FSK_CFG_0_PKT_MODE, 0);
    lgw_reg_w(SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FSK_CFG_0_CRC_EN, 0);
    lgw_reg_w(SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FSK_CFG_0_DCFREE_ENC, 0);
    lgw_reg_w(SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FSK_PKT_LENGTH_PKT_LENGTH, BLK);
    /* wait up to 1023 bit-times (HAL default 128) for the start marker after the preamble is
       detected: a battery thermostat's preamble is ~170 bits and was timing out every time */
    lgw_reg_w(SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FSK_TIMEOUT_MSB_TIMEOUT, 3);
    lgw_reg_w(SX1302_REG_RX_TOP_LORA_SERVICE_FSK_FSK_TIMEOUT_LSB_TIMEOUT, 255);
    lgw_fsk_raw = 1;
    memset(&txp, 0, sizeof txp);
    txp.freq_hz = 908400000; txp.tx_mode = IMMEDIATE; txp.rf_chain = 0; txp.rf_power = 0;
    txp.modulation = MOD_FSK; txp.datarate = 40000; txp.f_dev = 20;
    return 0;
}

static void load_home(void) {
    FILE *fp = fopen(ZW_HOME_FILE, "rb");
    if (fp && fread(HOME, 1, 4, fp) == 4) { fclose(fp); return; }
    if (fp) fclose(fp);
    int fd = open("/dev/urandom", O_RDONLY); if (read(fd, HOME, 4) != 4) { HOME[0]=0xC0; HOME[1]=0xDE; HOME[2]=0x12; HOME[3]=0x34; } close(fd);
    HOME[0] = (HOME[0] & 0x3F) | 0xC0;       /* keep it in the range the protocol reserves for hubs (first bits 11) */
    fp = fopen(ZW_HOME_FILE, "wb"); fwrite(HOME, 1, 4, fp); fclose(fp);
}


/* 1.11 s wake-up beam for a sleeping (FLiRS) node: [preamble, SOF, 0x55, node, home-ID hash] repeated */
static int do_beam(int node, int hash) {
    uint8_t frag[25]; int k = 0;
    for (; k < 20; k++) frag[k] = 0x55;
    frag[k++] = 0xF0; frag[k++] = 0x55; frag[k++] = (uint8_t)node; frag[k++] = (uint8_t)hash;
    txp.size = 0; txp.preamble = 10;
    txp.payload[txp.size++] = 0x55 ^ 0xFF; txp.payload[txp.size++] = (uint8_t)node ^ 0xFF; txp.payload[txp.size++] = (uint8_t)hash ^ 0xFF;
    while (txp.size + k <= 250) { for (int i = 0; i < k; i++) txp.payload[txp.size++] = frag[i] ^ 0xFF; }
    double t0 = wall(); int pkts = 0;
    while (wall() - t0 < 1.15) {
        lgw_send(&txp); pkts++;
        uint8_t st = 0; int polls = 0;
        do { lgw_status(0, TX_STATUS, &st); polls++; } while ((st == TX_SCHEDULED || st == TX_EMITTING) && polls < 5000);
    }
    return pkts;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: see header\n"); return 1; }
    load_home();
    if (radio_start()) { fprintf(stderr, "radio start failed\n"); return 2; }
    fprintf(stderr, "zwctl1302: home %02x%02x%02x%02x node %d, 908.40 MHz\n", HOME[0], HOME[1], HOME[2], HOME[3], ME);
    frame_t f;
    const char *cmd = argv[1];

    if (!strcmp(cmd, "listen")) {
        double end = wall() + (argc > 2 ? atof(argv[2]) : 30);
        while (wall() < end) if (recv_frame(end - wall(), &f)) show(&f, "rx");
    }
    else if (!strcmp(cmd, "add")) {
        double end = wall() + (argc > 2 ? atof(argv[2]) : 60);
        int newnode = argc > 3 ? atoi(argv[3]) : 2;
        printf("waiting for a device in add mode (node-information frame from node 0)...\n"); fflush(stdout);
        while (wall() < end) {
            if (!recv_frame(end - wall(), &f)) break;
            show(&f, "rx");
            int off = (f.type == 5) ? 12 : 0;   /* type 5 = announcement inside a 12-byte hub-search envelope */
            if (!(f.src == 0 && f.plen >= off + 2 && f.payload[off] == 0x01 && f.payload[off + 1] == 0x01)) continue;
            printf("device announced itself under temporary home %02x%02x%02x%02x\n", f.home[0], f.home[1], f.home[2], f.home[3]);
            nif_print(f.payload + off + 2, f.plen - off - 2);
            uint8_t dev_home[4]; memcpy(dev_home, f.home, 4);
            uint8_t pl7[7] = {0x01, 0x03, (uint8_t)newnode, HOME[0], HOME[1], HOME[2], HOME[3]};   /* AssignIDs */
            printf("assigning node %d on our network...\n", newnode); fflush(stdout);
            if (!send_acked(dev_home, 0, pl7, 7, &f)) { printf("no ACK to AssignIDs\n"); continue; }
            usleep(100000);
            uint8_t req[2] = {0x01, 0x02};                                             /* RequestNIF */
            printf("asking node %d for its capabilities...\n", newnode); fflush(stdout);
            if (!send_acked(HOME, newnode, req, 2, &f)) { printf("no ACK to RequestNIF (device may still have switched)\n"); }
            double e2 = wall() + 3;
            while (wall() < e2 && recv_frame(e2 - wall(), &f)) {
                show(&f, "rx");
                if (f.plen >= 2 && f.payload[0] == 0x01 && f.payload[1] == 0x01 && !memcmp(f.home, HOME, 4)) {
                    printf("node %d is on our network\n", f.src); nif_print(f.payload + 2, f.plen - 2); break;
                }
            }
            break;
        }
    }
    else if (!strcmp(cmd, "scan")) {
        int node = atoi(argv[2]);
        int first = argc > 3 ? (int)strtol(argv[3], NULL, 16) : 0;
        uint8_t pl[3] = {0x43, 0x02, 0x01};
        for (int i = 0; i < 256; i++) {
            int hash = (first + i) & 0xFF;
            do_beam(node, hash);
            if (send_acked(HOME, node, pl, 3, &f)) {
                printf("FOUND beam hash %02x for node %d\n", hash, node); fflush(stdout);
                double e2 = wall() + 3;
                while (wall() < e2 && recv_frame(e2 - wall(), &f)) {
                    show(&f, "rx");
                    if (f.plen >= 5 && f.payload[0] == 0x43 && f.payload[1] == 0x03) { printf("setpoint report received\n"); break; }
                }
                break;
            }
            printf("hash %02x: no answer\n", hash); fflush(stdout);
        }
    }
    else if (!strcmp(cmd, "cmd")) {
        /* cmd NODE HASH HEX [HEX ...]: wake the node once, then send each payload with ACK request
           and print every frame that comes back within 2 s */
        int node = atoi(argv[2]); int hash = (int)strtol(argv[3], NULL, 16);
        for (int a = 4; a < argc; a++) {
            uint8_t pl[64]; int n = 0;
            for (size_t i = 0; i + 1 < strlen(argv[a]) && n < 63; i += 2) { unsigned v; sscanf(argv[a] + i, "%2x", &v); pl[n++] = v; }
            do_beam(node, hash);
            printf("send %s\n", argv[a]); fflush(stdout);
            if (!send_acked(HOME, node, pl, n, &f)) { printf("   no ACK\n"); continue; }
            double e2 = wall() + 2;
            while (wall() < e2 && recv_frame(e2 - wall(), &f)) if (f.src == node) show(&f, "   reply");
        }
    }
    else if (!strcmp(cmd, "get") || !strcmp(cmd, "set") || !strcmp(cmd, "beam")) {
        int node = atoi(argv[2]);
        int do_get = !strcmp(cmd, "get") || (argc > 4 && !strcmp(argv[4], "get"));
        if (!strcmp(cmd, "beam")) {
            /* measured on air: continuous [preamble, SOF, 0x55, node, home-ID hash] fragments for 1.11 s */
            int hash = (int)strtol(argv[3], NULL, 16);
            uint8_t frag[25]; int k = 0;
            for (; k < 20; k++) frag[k] = 0x55;
            frag[k++] = 0xF0; frag[k++] = 0x55; frag[k++] = (uint8_t)node; frag[k++] = (uint8_t)hash;
            txp.size = 0; txp.preamble = 10;
            txp.payload[txp.size++] = 0x55 ^ 0xFF; txp.payload[txp.size++] = (uint8_t)node ^ 0xFF; txp.payload[txp.size++] = (uint8_t)hash ^ 0xFF;
            while (txp.size + k <= 250) { for (int i = 0; i < k; i++) txp.payload[txp.size++] = frag[i] ^ 0xFF; }
            double t0 = wall(); int pkts = 0;
            while (wall() - t0 < 1.15) {
                lgw_send(&txp); pkts++;
                uint8_t st = 0; int polls = 0;
                do { lgw_status(0, TX_STATUS, &st); polls++; } while ((st == TX_SCHEDULED || st == TX_EMITTING) && polls < 5000);
            }
            printf("beam: %d packets in %.2f s for node %d hash %02x\n", pkts, wall() - t0, node, hash); fflush(stdout);
        }
        if (!strcmp(cmd, "set") && argc > 4) do_beam(node, (int)strtol(argv[4], NULL, 16));   /* set NODE DEG_F [HASH] */
        if (!strcmp(cmd, "get") && argc > 3) do_beam(node, (int)strtol(argv[3], NULL, 16));   /* get NODE [HASH] */
        if (!strcmp(cmd, "set")) {
            int tenths = (int)(atof(argv[3]) * 10 + 0.5);
            uint8_t pl[6] = {0x43, 0x01, 0x01, 0x2A, (uint8_t)(tenths >> 8), (uint8_t)tenths};  /* Setpoint Set, heating, precision 1, F, 2 bytes */
            printf(send_acked(HOME, node, pl, 6, &f) ? "setpoint set acknowledged\n" : "no ACK\n");
        }
        if (do_get) {
            uint8_t pl[3] = {0x43, 0x02, 0x01};                                        /* Setpoint Get, heating */
            if (!send_acked(HOME, node, pl, 3, &f)) printf("no ACK to Setpoint Get\n");
            double e2 = wall() + 3;
            while (wall() < e2 && recv_frame(e2 - wall(), &f)) {
                show(&f, "rx");
                if (f.plen >= 5 && f.payload[0] == 0x43 && f.payload[1] == 0x03) {
                    int prec = f.payload[3] >> 5, scale = (f.payload[3] >> 3) & 3, size = f.payload[3] & 7;
                    long v = 0; for (int i = 0; i < size; i++) v = (v << 8) | f.payload[4 + i];
                    if (size == 1 && v > 127) v -= 256;
                    if (size == 2 && v > 32767) v -= 65536;
                    double val = v; for (int i = 0; i < prec; i++) val /= 10;
                    printf("setpoint report: type %d = %.1f %s\n", f.payload[2], val, scale ? "F" : "C"); break;
                }
            }
        }
    }
    lgw_stop();
    return 0;
}
