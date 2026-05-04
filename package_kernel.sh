#!/bin/bash
# =============================================================
# DBY-W09 Kernel Packaging Script
# Huawei MatePad 11 2021 — Snapdragon 865 — Kernel 4.19.157
# =============================================================
set -e

KERNEL_DIR="$(cd "$(dirname "$0")" && pwd)"
AK3_DIR="$KERNEL_DIR/AnyKernel3"
OUT_DIR="$KERNEL_DIR/out"
IMAGE="$KERNEL_DIR/arch/arm64/boot/Image.gz"
DATE=$(date +%Y%m%d-%H%M)
ZIP_NAME="DBY-W09_kernel-4.19.157_${DATE}.zip"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  DBY-W09 Kernel Packaging Script${NC}"
echo -e "${BLUE}  Huawei MatePad 11 2021 / Kona${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Check Image.gz
if [ ! -f "$IMAGE" ]; then
    echo -e "${RED}ERROR: Image.gz not found at:${NC}"
    echo -e "  $IMAGE"
    echo ""
    echo -e "${YELLOW}Build the kernel first:${NC}"
    echo "  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=clang \\"
    echo "    CLANG_TRIPLE=aarch64-linux-gnu- LD=ld.lld AR=llvm-ar NM=llvm-nm \\"
    echo "    OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip \\"
    echo "    HOSTCFLAGS=-fcommon -j\$(nproc) Image.gz"
    exit 1
fi

# Check zip
if ! command -v zip &>/dev/null; then
    echo -e "${RED}ERROR: 'zip' not installed.${NC}"
    echo "  sudo apt install zip"
    exit 1
fi

# Check AnyKernel3
if [ ! -d "$AK3_DIR" ]; then
    echo -e "${YELLOW}AnyKernel3 not found, cloning...${NC}"
    git clone --depth=1 https://github.com/osm0sis/AnyKernel3.git "$AK3_DIR"
fi

echo -e "${GREEN}[1/4] Copying kernel image...${NC}"
cp "$IMAGE" "$AK3_DIR/Image.gz"

echo -e "${GREEN}[2/4] Creating output directory...${NC}"
mkdir -p "$OUT_DIR"

echo -e "${GREEN}[3/4] Creating flashable zip...${NC}"
cd "$AK3_DIR"
zip -r9 "$OUT_DIR/$ZIP_NAME" \
    anykernel.sh \
    Image.gz \
    META-INF/ \
    modules/ \
    patch/ \
    ramdisk/ \
    tools/ \
    -x "*.git*" "*.DS_Store*" "*.placeholder" 2>/dev/null

echo -e "${GREEN}[4/4] Done!${NC}"
echo ""
echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}  Output: out/${ZIP_NAME}${NC}"
SIZE=$(du -sh "$OUT_DIR/$ZIP_NAME" | cut -f1)
echo -e "${GREEN}  Size:   ${SIZE}${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
echo -e "${YELLOW}Flashing instructions:${NC}"
echo ""
echo -e "${BLUE}Option A — via TWRP Recovery:${NC}"
echo "  1. Reboot to recovery (Vol Down + Power)"
echo "  2. Copy ${ZIP_NAME} to device"
echo "  3. Flash via TWRP → Install"
echo ""
echo -e "${BLUE}Option B — via Fastboot (recommended):${NC}"
echo "  1. Extract boot.img from your stock ROM"
echo "  2. magiskboot unpack boot.img"
echo "  3. cp arch/arm64/boot/Image.gz kernel"
echo "  4. magiskboot repack boot.img"
echo "  5. fastboot flash boot new-boot.img"
echo "  6. fastboot reboot"
echo ""
echo -e "${YELLOW}Note: Disable Secure Boot / unlock bootloader first${NC}"

# Cleanup
rm -f "$AK3_DIR/Image.gz"

echo ""
echo -e "${GREEN}Packaging complete!${NC}"
