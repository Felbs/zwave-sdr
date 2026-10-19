#!/bin/sh
# Build zwrx/zwtx against Semtech lora_gateway v5.0.1 with the raw-FSK patch.
# Defaults are for a Nebra Outdoor (CM3, SX1301 on /dev/spidev1.2, which only
# starts at 2 MHz SPI). Override for other boards:
#   SPI_DEV=/dev/spidev0.0 SPI_HZ=8000000 ./build.sh
set -e
SPI_DEV=${SPI_DEV:-/dev/spidev1.2}
SPI_HZ=${SPI_HZ:-2000000}
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
[ -d lora_gateway ] || git clone --depth 1 --branch v5.0.1 https://github.com/Lora-net/lora_gateway.git
cd lora_gateway
git checkout -- libloragw/src/loragw_hal.c libloragw/src/loragw_spi.native.c
git apply "$here/libloragw-raw-fsk.patch"
sed -i "s|^#define SPI_SPEED .*|#define SPI_SPEED       $SPI_HZ|; s|^#define SPI_DEV_PATH .*|#define SPI_DEV_PATH    \"$SPI_DEV\"|" libloragw/src/loragw_spi.native.c
make -C libloragw libloragw.a
cd "$here"
L=lora_gateway/libloragw
gcc -O2 -Wall -std=c99 -I$L/inc zwrx.c -L$L -lloragw -lrt -lm -o zwrx
gcc -O2 -Wall -std=c99 -I$L/inc zwtx.c -L$L -lloragw -lrt -lm -o zwtx
echo "built ./zwrx ./zwtx (SPI $SPI_DEV @ $SPI_HZ Hz)"
