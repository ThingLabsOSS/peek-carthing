#!/bin/sh
# peek-userspace.sh — comprehensive userspace hardware inventory for the
# Spotify Car Thing. Runs entirely from userspace (no module needed) —
# pulls everything sysfs + i2c-tools + /proc + /sys/kernel/debug expose.
#
# Pair with peek_carthing.ko if you also need raw MMIO register dumps
# (HHI clock tree, VPU/ENCL display block, etc) that aren't exposed to
# userspace because the running kernel has CONFIG_DEVMEM=n.
#
# Usage on the device (via adb / UART shell):
#   sh peek-userspace.sh > /tmp/hwinfo.txt
#   cat /tmp/hwinfo.txt
# Or remote-run via adb:
#   adb shell sh < peek-userspace.sh > hwinfo.txt
#
# Designed to "do no harm" — only reads, no writes. The i2c reads use
# the i2c-tools standard interface so they only ACK-probe and read; no
# writes to any chip.

set -u

section() { printf "\n==================== %s ====================\n" "$1"; }
subsec()  { printf "\n--- %s ---\n" "$1"; }
maybe()   { if command -v "$1" >/dev/null 2>&1; then "$@"; else echo "(skipped: $1 not found)"; fi; }
catf()    { if [ -r "$1" ]; then cat "$1"; else echo "(unreadable: $1)"; fi; }
catfh()   { if [ -r "$1" ]; then head -c "$2" "$1"; printf '\n'; else echo "(unreadable: $1)"; fi; }

section "Identity"
echo "date         : $(date)"
echo "hostname     : $(hostname 2>/dev/null)"
echo "uname        : $(uname -a)"
echo "uptime       : $(cut -d' ' -f1 < /proc/uptime 2>/dev/null) sec"
catf /sys/devices/soc0/family 2>/dev/null
catf /sys/devices/soc0/machine 2>/dev/null
catf /sys/firmware/devicetree/base/model 2>/dev/null
printf '\n'
catf /sys/firmware/devicetree/base/compatible 2>/dev/null
printf '\n'

section "Kernel + cmdline"
catf /proc/version
echo ""
echo "cmdline:"
catf /proc/cmdline
echo ""
echo "modules loaded:"
catf /proc/modules
echo ""
echo "tainted: $(catf /proc/sys/kernel/tainted)"
echo ""
echo "CONFIG_*: (only via /proc/config.gz, if present)"
gzip -dc /proc/config.gz 2>/dev/null | grep -E "DEVMEM|MODULE_SIG|FORCE_LOAD|MODVERSIONS|FTRACE|FUNCTION_TRACER" | head -20

section "CPU"
catfh /proc/cpuinfo 4096

section "Memory"
catfh /proc/meminfo 2048
echo ""
subsec "DMA / CMA"
catf /sys/kernel/debug/cma/cma-* 2>/dev/null
ls /sys/kernel/debug/cma/ 2>/dev/null

section "Loaded drivers / Device probe state"
echo "platform devices:"
ls /sys/bus/platform/devices/ 2>/dev/null | head -30
echo ""
echo "platform drivers bound:"
for d in /sys/bus/platform/drivers/*; do
    [ -d "$d" ] || continue
    bound=$(ls "$d" 2>/dev/null | grep -vE '^(bind|unbind|module|uevent)$' | tr '\n' ' ')
    [ -z "$bound" ] && continue
    echo "  $(basename "$d"): $bound"
done

section "eMMC"
for h in /sys/class/mmc_host/mmc*; do
    [ -d "$h" ] || continue
    echo "host: $(basename "$h")"
    for f in "$h"/mmc*/cid "$h"/mmc*/csd "$h"/mmc*/name "$h"/mmc*/manfid \
             "$h"/mmc*/oemid "$h"/mmc*/fwrev "$h"/mmc*/hwrev \
             "$h"/mmc*/preferred_erase_size; do
        [ -r "$f" ] && echo "  $(basename "$f"): $(cat "$f")"
    done
done
subsec "EXT_CSD (full 512 B via debugfs if present)"
for f in /sys/kernel/debug/mmc*/mmc*/ext_csd; do
    [ -r "$f" ] || continue
    echo "$f:"
    cat "$f"
done
subsec "Block devices"
maybe lsblk -o NAME,SIZE,MAJ:MIN,RO,FSTYPE,MODEL,VENDOR 2>/dev/null
echo ""
echo "raw block listing:"
ls -la /dev/mmcblk* /dev/sd* 2>/dev/null

section "Partition table (mmcblk0)"
catf /proc/partitions
echo ""
echo "first sector of mmcblk0 (looking for MBR or GPT signature):"
dd if=/dev/mmcblk0 bs=512 count=1 2>/dev/null | od -An -tx1 -w16 | head -8

section "Thermal"
for z in /sys/class/thermal/thermal_zone*; do
    [ -d "$z" ] || continue
    echo "$(basename "$z"): type=$(cat "$z"/type 2>/dev/null) temp=$(cat "$z"/temp 2>/dev/null)"
done

section "Voltage rails (regulators)"
catf /sys/kernel/debug/regulator/regulator_summary 2>/dev/null

section "Clock tree"
catf /sys/kernel/debug/clk/clk_summary 2>/dev/null

section "Pinctrl + GPIOs"
catf /sys/kernel/debug/pinctrl/pin-controller*/pinmux-pins 2>/dev/null | head -100
echo ""
subsec "Active GPIO state"
catf /sys/kernel/debug/gpio 2>/dev/null

section "Backlight"
for b in /sys/class/backlight/*; do
    [ -d "$b" ] || continue
    echo "$(basename "$b"):"
    for f in "$b"/brightness "$b"/max_brightness "$b"/bl_power "$b"/actual_brightness; do
        [ -r "$f" ] && echo "  $(basename "$f"): $(cat "$f")"
    done
done

section "PWM channels"
for c in /sys/class/pwm/pwmchip*; do
    [ -d "$c" ] || continue
    echo "$(basename "$c"): npwm=$(cat "$c"/npwm 2>/dev/null)"
    for p in "$c"/pwm*; do
        [ -d "$p" ] || continue
        echo "  $(basename "$p"): period=$(cat "$p"/period 2>/dev/null) duty=$(cat "$p"/duty_cycle 2>/dev/null) enable=$(cat "$p"/enable 2>/dev/null)"
    done
done

section "IIO (SARADC, temp, etc)"
for d in /sys/bus/iio/devices/iio:device*; do
    [ -d "$d" ] || continue
    echo "$(basename "$d"): name=$(cat "$d"/name 2>/dev/null)"
    for c in "$d"/in_voltage*_raw "$d"/in_temp*_raw; do
        [ -r "$c" ] && echo "  $(basename "$c"): $(cat "$c")"
    done
done

section "DRM/KMS"
for n in /sys/kernel/debug/dri/0/state \
         /sys/kernel/debug/dri/0/framebuffer \
         /sys/kernel/debug/dri/0/internal_clients; do
    [ -r "$n" ] || continue
    subsec "$(basename "$n")"
    cat "$n"
done
echo ""
echo "connectors:"
ls /sys/kernel/debug/dri/0/ 2>/dev/null
for c in /sys/kernel/debug/dri/0/*/modes; do
    [ -r "$c" ] || continue
    echo "$c:"
    cat "$c"
done

section "I2C bus survey"
for b in /dev/i2c-*; do
    [ -c "$b" ] || continue
    bn=$(basename "$b" | sed 's/^i2c-//')
    echo "i2c-$bn:"
    maybe i2cdetect -y -r "$bn" 2>/dev/null
done

subsec "MAX14656 (charger, i2c-2 0x35) — regs 0x00..0x09"
maybe i2cdump -y -r 0x00-0x09 2 0x35 b 2>/dev/null

subsec "TMD2772 (prox/ALS, i2c-2 0x39) — driver-bound, raw read may fail"
maybe i2cdump -y -r 0x00-0x1f 2 0x39 b 2>/dev/null || echo "(skipped — UU)"

subsec "TLSC6X (touch + panel-detect, i2c-0 0x2e) — driver-bound"
maybe i2cdump -y -r 0x00-0x3f 0 0x2e b 2>/dev/null || echo "(skipped — UU)"

subsec "MFi auth (i2c-3 0x10) — sleeping chip, may NAK"
maybe i2cget -y 3 0x10 0x00 b 2>/dev/null

section "USB"
echo "device gadget state:"
catf /sys/class/android_usb/android0/state 2>/dev/null
echo ""
echo "usb host devices:"
ls /sys/bus/usb/devices/ 2>/dev/null
echo ""
catf /sys/kernel/debug/usb/devices 2>/dev/null | head -60

section "Filesystem"
catf /proc/mounts
echo ""
df -h 2>/dev/null

section "Network"
ip addr 2>/dev/null || ifconfig -a 2>/dev/null
echo ""
catf /proc/net/dev

section "dmesg (boot tail)"
dmesg 2>/dev/null | tail -100 || echo "(dmesg unreadable)"

section "Random goodies"
echo "/proc/interrupts:"
catf /proc/interrupts
echo ""
echo "/proc/iomem:"
catf /proc/iomem
echo ""
echo "/proc/ioports:"
catf /proc/ioports

section "Apple MFi state (driver-bound)"
for d in /sys/bus/i2c/devices/3-0010 /sys/bus/i2c/devices/*-0010; do
    [ -d "$d" ] || continue
    echo "device: $d"
    ls "$d" 2>/dev/null
    for f in "$d"/serial "$d"/certificate "$d"/protocol_version; do
        [ -r "$f" ] && echo "  $(basename "$f"): $(cat "$f" 2>/dev/null)"
    done
done

echo ""
echo "==================== end of dump ===================="
