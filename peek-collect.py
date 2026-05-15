#!/usr/bin/env python3
"""
peek-collect.py — robotically thorough Car Thing hardware inventory.

Runs ON THE DEVICE. Collects every piece of hardware info I could think
of and emits one JSON document on stdout. Pair with peek_mmio.ko for
MMIO register dumps that userspace can't reach (stock kernel has
CONFIG_DEVMEM=n).

Sections produced (one top-level JSON key each):
  meta              tool version + capture timestamp + device serial
  system            kernel banner, uname, uptime, loadavg, /proc/cmdline
  soc               MIDR-decoded CPU info, on-die serial, features
  memory            full /proc/meminfo
  dram              from kmod: controller regs + per-byte-lane DLL training
  gpu               from kmod: Mali ID decoded (Mali-G31 etc), shader cores
  display           from kmod: ENCL timing decoded to h/v_period etc,
                    HHI MIPI dphy bias triplet, panel variant via TLSC6X
  emmc              CID/CSD/EXT_CSD + controller regs (kmod) + sysfs paths
  i2c               every bus enumerated, every chip ACKing + register dumps
  saradc            all 8 channels raw + decoded (PCB/BT/DRAM NTC, board rev)
  thermal           every registered zone + cooked temp_c + cooling devs
  audio             PDM mic state from kmod (ringbuffer addr live)
  usb               host devices + gadget state + DWC3 regs (kmod)
  drm               active mode, framebuffers, connectors via debugfs
  modules           lsmod + per-module parameters + tainted flag decode
  pinctrl           full pinmux state if debugfs mounted
  regulators        regulator_summary if debugfs mounted
  clocks            clk_summary if debugfs mounted
  gpio              full GPIO sysfs + debugfs state
  dtb               full devicetree walk from /sys/firmware/devicetree
  iio               every IIO device + every channel
  block             every block device + size + model + holders
  net               every net interface + MAC + state + counters
  rtc               /sys/class/rtc state if present
  bluetooth         /sys/class/bluetooth/hci0 state if present
  processes        ps + per-pid cmdline/status (top N by RSS)
  filesystem        /proc/mounts, /proc/partitions, df
  kmod_dump         raw MMIO hex from peek_mmio.ko if loadable
  proc              selected /proc files (interrupts, iomem, ioports, …)

Run on device:
    python3 peek-collect.py > hwinfo.json

Or remotely:
    adb shell python3 - < peek-collect.py > hwinfo.json
"""

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

VERSION = "0.2"
TOOL_NAME = "peek-collect"


# -----------------------------------------------------------------------
# small utilities
# -----------------------------------------------------------------------

def slurp(path, max_bytes=64 * 1024):
    """Read a file, return text or None on error. Cap size for huge files."""
    try:
        with open(path, "rb") as f:
            data = f.read(max_bytes)
        return data.decode("utf-8", errors="replace")
    except OSError:
        return None


def slurp_lines(path, max_bytes=64 * 1024):
    s = slurp(path, max_bytes)
    if s is None:
        return None
    return s.rstrip("\n").split("\n")


def run(cmd, timeout=5):
    """Run a shell command, return (stdout, stderr, returncode). Never raise."""
    try:
        r = subprocess.run(
            cmd, shell=isinstance(cmd, str), capture_output=True,
            text=True, timeout=timeout,
        )
        return r.stdout, r.stderr, r.returncode
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError) as e:
        return "", str(e), -1


def parse_kv(text, sep=":"):
    """Parse key: value lines from a /proc-style file. Returns dict.
    Multiple values for the same key are joined into a list."""
    if not text:
        return {}
    out = {}
    for line in text.split("\n"):
        if sep not in line:
            continue
        k, _, v = line.partition(sep)
        k, v = k.strip(), v.strip()
        if not k:
            continue
        if k in out:
            if isinstance(out[k], list):
                out[k].append(v)
            else:
                out[k] = [out[k], v]
        else:
            out[k] = v
    return out


def dir_listing(path):
    try:
        return sorted(os.listdir(path))
    except OSError:
        return []


def safe_int(v, default=None):
    try:
        return int(v, 0) if isinstance(v, str) else int(v)
    except (ValueError, TypeError):
        return default


# -----------------------------------------------------------------------
# section: system / kernel / cmdline
# -----------------------------------------------------------------------

def collect_system():
    uname_s, _, _ = run(["uname", "-a"])
    loadavg = slurp("/proc/loadavg") or ""
    uptime_s = (slurp("/proc/uptime") or "0 0").split()
    hostname = (slurp("/etc/hostname") or "").strip()
    return {
        "uname": uname_s.strip(),
        "kernel_version": (slurp("/proc/version") or "").strip(),
        "hostname": hostname,
        "uptime_sec": float(uptime_s[0]) if uptime_s else None,
        "idle_sec": float(uptime_s[1]) if len(uptime_s) > 1 else None,
        "loadavg": loadavg.strip(),
        "cmdline": (slurp("/proc/cmdline") or "").strip(),
        "tainted": slurp("/proc/sys/kernel/tainted") and slurp("/proc/sys/kernel/tainted").strip(),
        "tainted_decoded": decode_tainted(safe_int(slurp("/proc/sys/kernel/tainted") or "0")),
    }


def decode_tainted(v):
    if v is None:
        return []
    names = {
        0: "PROPRIETARY_MODULE (P)",
        1: "FORCED_MODULE (F)",
        2: "FORCED_RMMOD (R)",
        3: "MACHINE_CHECK (M)",
        4: "BAD_PAGE (B)",
        5: "USER (U)",
        6: "DIE (D)",
        7: "OVERRIDDEN_ACPI_TABLE (A)",
        8: "WARN (W)",
        9: "CRAP (C)",
        10: "FW_CRASHED (I)",
        11: "FW_OOPS (O — legacy)",
        12: "OOT_MODULE (O)",
        13: "UNSIGNED_MODULE (E)",
    }
    return [names.get(b, f"bit_{b}") for b in range(32) if v & (1 << b)]


# -----------------------------------------------------------------------
# section: soc / cpu
# -----------------------------------------------------------------------

def decode_midr(midr):
    impl = (midr >> 24) & 0xff
    variant = (midr >> 20) & 0xf
    arch = (midr >> 16) & 0xf
    part = (midr >> 4) & 0xfff
    rev = midr & 0xf
    impl_names = {
        0x41: "ARM Limited",
        0x42: "Broadcom",
        0x43: "Cavium",
        0x44: "DEC",
        0x4e: "NVIDIA",
        0x50: "Applied Micro",
        0x51: "Qualcomm",
        0x53: "Samsung",
        0x56: "Marvell",
        0x66: "Faraday",
        0x69: "Intel",
        0xc0: "Ampere",
    }
    part_names = {
        0xd03: "Cortex-A53",
        0xd04: "Cortex-A35",
        0xd05: "Cortex-A55",
        0xd07: "Cortex-A57",
        0xd08: "Cortex-A72",
        0xd09: "Cortex-A73",
        0xd0a: "Cortex-A75",
        0xd0b: "Cortex-A76",
    }
    return {
        "implementer": f"0x{impl:02x} ({impl_names.get(impl, '?')})",
        "variant": f"0x{variant:x}",
        "architecture": f"0x{arch:x}",
        "part": f"0x{part:03x} ({part_names.get(part, '?')})",
        "revision": rev,
    }


def collect_soc():
    cpuinfo = slurp("/proc/cpuinfo") or ""
    cpus = []
    current = {}
    for line in cpuinfo.split("\n"):
        if not line.strip():
            if current:
                cpus.append(current)
                current = {}
            continue
        if ":" in line:
            k, v = line.split(":", 1)
            current[k.strip()] = v.strip()
    if current:
        cpus.append(current)

    serial = None
    for cpu in cpus:
        if "Serial" in cpu:
            serial = cpu["Serial"]
            break

    # MIDR — peek_mmio.ko prints it, but we can also read via /proc/cpuinfo
    # (the CPU implementer/part/rev fields). Reconstruct an effective MIDR.
    midr = None
    if cpus:
        c = cpus[0]
        try:
            midr = (
                (int(c.get("CPU implementer", "0"), 16) << 24)
                | (int(c.get("CPU variant", "0"), 16) << 20)
                | (int(c.get("CPU architecture", "0")) << 16)
                | (int(c.get("CPU part", "0"), 16) << 4)
                | int(c.get("CPU revision", "0"))
            )
        except (ValueError, TypeError):
            pass

    # filter out the trailing block (just "Serial" and "Hardware") that
    # /proc/cpuinfo emits once after the per-CPU sections.
    real_cpus = [c for c in cpus if "processor" in c]

    return {
        "cpu_count": len(real_cpus),
        "midr": f"0x{midr:08x}" if midr else None,
        "midr_decoded": decode_midr(midr) if midr else None,
        "features": (cpus[0].get("Features") if cpus else "").split() if cpus else [],
        "bogomips": cpus[0].get("BogoMIPS") if cpus else None,
        "hardware_field": cpus[0].get("Hardware") if cpus else None,
        "serial_hex": serial,
        "per_cpu": cpus,
    }


# -----------------------------------------------------------------------
# section: memory
# -----------------------------------------------------------------------

def collect_memory():
    raw = slurp("/proc/meminfo") or ""
    return {
        "meminfo": parse_kv(raw),
        "vmstat": parse_kv(slurp("/proc/vmstat") or "", sep=" "),
        "slabinfo_present": Path("/proc/slabinfo").exists(),
    }


# -----------------------------------------------------------------------
# section: thermal
# -----------------------------------------------------------------------

def collect_thermal():
    zones = []
    for d in dir_listing("/sys/class/thermal"):
        if not d.startswith("thermal_zone"):
            continue
        base = f"/sys/class/thermal/{d}"
        ztype = (slurp(f"{base}/type") or "").strip()
        temp_raw = safe_int((slurp(f"{base}/temp") or "0").strip())
        zones.append({
            "id": d,
            "type": ztype,
            "temp_millidegc": temp_raw,
            "temp_c": temp_raw / 1000.0 if temp_raw is not None else None,
            "mode": (slurp(f"{base}/mode") or "").strip(),
            "policy": (slurp(f"{base}/policy") or "").strip(),
        })
    cooling = []
    for d in dir_listing("/sys/class/thermal"):
        if not d.startswith("cooling_device"):
            continue
        base = f"/sys/class/thermal/{d}"
        cooling.append({
            "id": d,
            "type": (slurp(f"{base}/type") or "").strip(),
            "cur_state": safe_int((slurp(f"{base}/cur_state") or "").strip()),
            "max_state": safe_int((slurp(f"{base}/max_state") or "").strip()),
        })
    return {"zones": zones, "cooling_devices": cooling}


# -----------------------------------------------------------------------
# section: iio (SARADC + TMD2772 etc)
# -----------------------------------------------------------------------

# G12A SARADC channel-to-function mapping from carthing DT
SARADC_FUNCTION = {
    0: "PCB thermal (NTC rt3)",
    1: "board revision (resistor divider)",
    2: "BT chip thermal (NTC rt1)",
    3: "DRAM thermal (NTC rt2)",
    4: "unused",
    5: "unused",
    6: "unused",
    7: "unused (sometimes 0V ref)",
}


def collect_iio():
    devs = []
    for d in dir_listing("/sys/bus/iio/devices"):
        if not d.startswith("iio:device"):
            continue
        base = f"/sys/bus/iio/devices/{d}"
        name = (slurp(f"{base}/name") or "").strip()
        channels = {}
        for entry in dir_listing(base):
            m = re.match(r"^in_voltage(\d+)_raw$", entry)
            if not m:
                continue
            ch = int(m.group(1))
            raw = safe_int((slurp(f"{base}/{entry}") or "").strip())
            mean = safe_int((slurp(f"{base}/in_voltage{ch}_mean_raw") or "").strip())
            channels[ch] = {
                "raw": raw,
                "mean_raw": mean,
                "function": SARADC_FUNCTION.get(ch) if name == "meson-g12a-saradc" else None,
            }
        # Other channel kinds (temp, illuminance, etc.)
        other = {}
        for entry in dir_listing(base):
            if entry.endswith("_raw") and not entry.startswith("in_voltage"):
                other[entry] = (slurp(f"{base}/{entry}") or "").strip()
        devs.append({
            "id": d,
            "name": name,
            "voltage_channels": channels,
            "other_readings": other,
        })
    return devs


# -----------------------------------------------------------------------
# section: emmc
# -----------------------------------------------------------------------

def collect_emmc():
    hosts = []
    for h in dir_listing("/sys/class/mmc_host"):
        base = f"/sys/class/mmc_host/{h}"
        cards = []
        for c in dir_listing(base):
            cdir = f"{base}/{c}"
            if not Path(f"{cdir}/cid").exists():
                continue
            fields = {}
            for k in ["cid", "csd", "name", "manfid", "oemid", "fwrev",
                      "hwrev", "serial", "date", "preferred_erase_size",
                      "scr", "erase_size", "type"]:
                v = slurp(f"{cdir}/{k}")
                if v is not None:
                    fields[k] = v.strip()
            cards.append({"id": c, **fields})
        hosts.append({"id": h, "cards": cards})

    # try to grab EXT_CSD via debugfs (if exposed)
    ext_csd = {}
    for p in Path("/sys/kernel/debug").rglob("ext_csd"):
        try:
            ext_csd[str(p)] = slurp(str(p), max_bytes=4096)
        except OSError:
            pass

    return {"hosts": hosts, "ext_csd": ext_csd}


# -----------------------------------------------------------------------
# section: i2c — every bus + every detected chip + register dumps
# -----------------------------------------------------------------------

KNOWN_I2C_CHIPS = {
    (2, 0x35): ("MAX14656", "USB charger detector", 0x09),
    (2, 0x39): ("TMD2772", "prox + ALS", 0x1f),
    (0, 0x2e): ("TLSC6X", "touch + panel-variant probe", 0x3f),
    (3, 0x10): ("Apple MFi", "auth coprocessor (sleeping, may NAK)", 0x05),
}


def collect_i2c():
    buses = []
    for d in dir_listing("/dev"):
        m = re.match(r"^i2c-(\d+)$", d)
        if not m:
            continue
        bus = int(m.group(1))
        detect, _, _ = run(["i2cdetect", "-y", "-r", str(bus)], timeout=3)
        detected = []
        # parse detect output: hex addresses + "UU" for driver-bound
        for line in detect.split("\n")[1:]:
            parts = line.split()
            if not parts or not parts[0].endswith(":"):
                continue
            row = int(parts[0].rstrip(":"), 16) * 16
            for i, cell in enumerate(parts[1:17] if len(parts) > 1 else []):
                if cell != "--":
                    addr = row + i
                    detected.append({
                        "address": f"0x{addr:02x}",
                        "state": "driver-bound" if cell == "UU" else "available",
                    })

        chips = {}
        for (b, addr), (chip_name, desc, last_reg) in KNOWN_I2C_CHIPS.items():
            if b != bus:
                continue
            out, _, rc = run(
                ["i2cdump", "-y", "-r", f"0x00-0x{last_reg:02x}",
                 str(bus), f"0x{addr:02x}", "b"],
                timeout=3,
            )
            if rc == 0:
                chips[f"0x{addr:02x}"] = {
                    "name": chip_name,
                    "description": desc,
                    "register_dump": out.strip(),
                }
        buses.append({
            "bus": bus,
            "detected": detected,
            "known_chips": chips,
        })
    return buses


# -----------------------------------------------------------------------
# section: drm / display
# -----------------------------------------------------------------------

def collect_drm():
    out = {}
    base = "/sys/kernel/debug/dri/0"
    for entry in ["state", "framebuffer", "internal_clients", "name"]:
        s = slurp(f"{base}/{entry}", max_bytes=16384)
        if s:
            out[entry] = s.strip()
    # connector subdirs (typically named like "card0-DSI-1" or similar)
    out["connectors"] = {}
    for c in dir_listing(base):
        cpath = f"{base}/{c}"
        if not Path(cpath).is_dir():
            continue
        conn = {}
        for f in ["modes", "edid", "status", "force", "modes_str"]:
            v = slurp(f"{cpath}/{f}")
            if v is not None:
                conn[f] = v.strip()
        out["connectors"][c] = conn
    return out


# -----------------------------------------------------------------------
# section: modules / drivers
# -----------------------------------------------------------------------

MODULE_FLAGS = {
    "P": "TAINT_PROPRIETARY_MODULE",
    "F": "TAINT_FORCED_MODULE",
    "O": "TAINT_OOT_MODULE",
    "E": "TAINT_UNSIGNED_MODULE",
    "C": "TAINT_CRAP",
}


def collect_modules():
    raw = slurp("/proc/modules") or ""
    mods = []
    for line in raw.split("\n"):
        parts = line.split()
        if len(parts) < 4:
            continue
        name, size, refcount, deps = parts[0], parts[1], parts[2], parts[3]
        flags = parts[5] if len(parts) > 5 else ""
        mods.append({
            "name": name,
            "size": int(size),
            "refcount": int(refcount),
            "depends_on": [] if deps == "-" else deps.rstrip(",").split(","),
            "state": parts[4] if len(parts) > 4 else "",
            "flags": flags.strip("()"),
            "address": parts[6] if len(parts) > 6 else None,
        })
    return mods


# -----------------------------------------------------------------------
# section: USB
# -----------------------------------------------------------------------

def collect_usb():
    devs = []
    for d in dir_listing("/sys/bus/usb/devices"):
        base = f"/sys/bus/usb/devices/{d}"
        info = {}
        for k in ["idVendor", "idProduct", "manufacturer", "product",
                  "serial", "speed", "version", "bDeviceClass",
                  "bMaxPower"]:
            v = slurp(f"{base}/{k}")
            if v is not None:
                info[k] = v.strip()
        if info:
            devs.append({"id": d, **info})
    gadget_state = (slurp("/sys/class/android_usb/android0/state") or "").strip()
    return {
        "devices": devs,
        "android_gadget_state": gadget_state or None,
    }


# -----------------------------------------------------------------------
# section: network
# -----------------------------------------------------------------------

def collect_net():
    ifs = []
    for d in dir_listing("/sys/class/net"):
        base = f"/sys/class/net/{d}"
        ifs.append({
            "name": d,
            "address": (slurp(f"{base}/address") or "").strip(),
            "operstate": (slurp(f"{base}/operstate") or "").strip(),
            "carrier": (slurp(f"{base}/carrier") or "").strip(),
            "mtu": (slurp(f"{base}/mtu") or "").strip(),
            "type": (slurp(f"{base}/type") or "").strip(),
        })
    return {"interfaces": ifs}


# -----------------------------------------------------------------------
# section: gpu (Mali) — sysfs paths
# -----------------------------------------------------------------------

def collect_gpu_sysfs():
    """Mali driver exposes a fair amount under /sys/class/misc/mali0/."""
    out = {}
    for path in ["/sys/class/misc/mali0", "/sys/devices/platform/ffe40000.bifrost"]:
        if not Path(path).exists():
            continue
        for entry in dir_listing(path):
            v = slurp(f"{path}/{entry}", max_bytes=2048)
            if v is not None and len(v) < 4096:
                out[f"{path}/{entry}"] = v.strip()
    return out


# -----------------------------------------------------------------------
# section: pinctrl / clk / regulator debugfs
# -----------------------------------------------------------------------

def collect_debugfs_dumps():
    out = {}
    for p in ["/sys/kernel/debug/clk/clk_summary",
              "/sys/kernel/debug/regulator/regulator_summary",
              "/sys/kernel/debug/gpio",
              "/sys/kernel/debug/wakeup_sources"]:
        v = slurp(p, max_bytes=64 * 1024)
        if v is not None:
            out[p] = v
    # pinctrl is per-controller
    for p in Path("/sys/kernel/debug/pinctrl").glob("*/pinmux-pins") if Path("/sys/kernel/debug/pinctrl").exists() else []:
        v = slurp(str(p), max_bytes=64 * 1024)
        if v is not None:
            out[str(p)] = v
    return out


# -----------------------------------------------------------------------
# section: block / filesystem
# -----------------------------------------------------------------------

def collect_block():
    devs = []
    for d in dir_listing("/sys/block"):
        base = f"/sys/block/{d}"
        info = {
            "name": d,
            "size_sectors": safe_int((slurp(f"{base}/size") or "").strip()),
            "removable": (slurp(f"{base}/removable") or "").strip(),
            "ro": (slurp(f"{base}/ro") or "").strip(),
            "model": (slurp(f"{base}/device/model") or "").strip(),
            "vendor": (slurp(f"{base}/device/vendor") or "").strip(),
            "partitions": [],
        }
        for sub in dir_listing(base):
            if sub.startswith(d):
                psize = safe_int((slurp(f"{base}/{sub}/size") or "").strip())
                info["partitions"].append({
                    "name": sub,
                    "size_sectors": psize,
                })
        devs.append(info)
    return {
        "devices": devs,
        "mounts": slurp("/proc/mounts"),
        "partitions": slurp("/proc/partitions"),
    }


# -----------------------------------------------------------------------
# section: dtb walk
# -----------------------------------------------------------------------

def walk_dtb(path="/sys/firmware/devicetree/base", max_depth=8, _depth=0):
    if _depth >= max_depth:
        return {"__truncated__": True}
    if not Path(path).is_dir():
        return None
    out = {}
    for entry in sorted(os.listdir(path)):
        ep = f"{path}/{entry}"
        try:
            if os.path.isdir(ep):
                out[entry] = walk_dtb(ep, max_depth, _depth + 1)
            else:
                with open(ep, "rb") as f:
                    raw = f.read(256)
                # try string decode; fallback to hex
                try:
                    s = raw.rstrip(b"\x00").decode("utf-8")
                    if all(c.isprintable() or c in "\n\t " for c in s):
                        out[entry] = s
                        continue
                except UnicodeDecodeError:
                    pass
                out[entry] = "0x" + raw.hex()
        except OSError:
            out[entry] = "<read error>"
    return out


# -----------------------------------------------------------------------
# section: assorted /proc files
# -----------------------------------------------------------------------

def collect_proc():
    out = {}
    for p in ["/proc/interrupts", "/proc/iomem", "/proc/ioports",
              "/proc/devices", "/proc/diskstats", "/proc/stat",
              "/proc/buddyinfo", "/proc/zoneinfo", "/proc/swaps",
              "/proc/misc"]:
        v = slurp(p, max_bytes=128 * 1024)
        if v is not None:
            out[p] = v
    return out


# -----------------------------------------------------------------------
# section: processes
# -----------------------------------------------------------------------

def collect_processes():
    procs = []
    for pid_dir in dir_listing("/proc"):
        if not pid_dir.isdigit():
            continue
        base = f"/proc/{pid_dir}"
        comm = (slurp(f"{base}/comm") or "").strip()
        cmdline = (slurp(f"{base}/cmdline") or "").replace("\x00", " ").strip()
        status = parse_kv(slurp(f"{base}/status") or "")
        procs.append({
            "pid": int(pid_dir),
            "comm": comm,
            "cmdline": cmdline,
            "ppid": safe_int(status.get("PPid")),
            "uid": status.get("Uid"),
            "vm_rss_kb": safe_int((status.get("VmRSS") or "").split()[0] if status.get("VmRSS") else None),
            "threads": safe_int(status.get("Threads")),
        })
    procs.sort(key=lambda p: -(p["vm_rss_kb"] or 0))
    return procs


# -----------------------------------------------------------------------
# section: kmod MMIO — load peek_mmio.ko, parse dmesg
# -----------------------------------------------------------------------

def parse_kmod_dmesg():
    """Run dmesg, extract peek_mmio: lines, group by section, parse hex grid."""
    out, _, _ = run(["dmesg"])
    sections = {}
    current_section = None
    current_rows = {}
    for line in out.split("\n"):
        m = re.search(r"peek_mmio: === (.+?) ===", line)
        if m:
            if current_section:
                sections[current_section] = current_rows
            current_section = m.group(1)
            current_rows = {}
            continue
        m = re.search(r"peek_mmio:\s+\+0x([0-9a-f]{4}):\s+([0-9a-f ]+)", line)
        if m and current_section:
            off = int(m.group(1), 16)
            words = [int(w, 16) for w in m.group(2).split() if len(w) == 8]
            current_rows[f"0x{off:04x}"] = words
            continue
        m = re.search(r"peek_mmio:\s+(\w+_\w+)\s*=\s*0x([0-9a-f]+)", line)
        if m and current_section:
            current_rows[m.group(1)] = f"0x{m.group(2)}"
    if current_section:
        sections[current_section] = current_rows

    # Decode known sections into typed fields
    decoded = {}
    for section, rows in sections.items():
        decoded[section] = {"raw": rows}

    # ENCL timing decode
    encl_key = next((k for k in decoded if "near-ENCL" in k), None)
    if encl_key:
        rows = decoded[encl_key]["raw"]
        # The ENCL timing registers are at offset 0x2c0..0x2ec
        # within VPU+0x7000. So our +0x02c0 row should have:
        #   word 0: MAX_PXCNT, word 1: HAVON_END, word 2: HAVON_BEGIN,
        #   word 3: VAVON_ELINE
        r2c0 = rows.get("0x02c0")
        r2d0 = rows.get("0x02d0")
        r2e0 = rows.get("0x02e0")
        if r2c0 and r2d0 and r2e0 and len(r2c0) >= 4:
            decoded[encl_key]["panel_timing"] = {
                "max_pxcnt": r2c0[0], "h_total": r2c0[0] + 1,
                "havon_end": r2c0[1], "havon_begin": r2c0[2],
                "vavon_eline": r2c0[3], "vavon_bline": r2d0[0],
                "hso_end": r2d0[2], "vso_eline": r2e0[2],
                "max_lncnt": r2e0[3], "v_total": r2e0[3] + 1,
            }

    # HHI MIPI DPHY bias
    hhi_key = next((k for k in decoded if k.startswith("HHI clock")), None)
    if hhi_key:
        rows = decoded[hhi_key]["raw"]
        r0 = rows.get("0x0000")
        if r0 and len(r0) >= 3:
            decoded[hhi_key]["mipi_dphy_bias"] = {
                "MIPI_CNTL0": f"0x{r0[0]:08x}",
                "MIPI_CNTL1": f"0x{r0[1]:08x}",
                "MIPI_CNTL2": f"0x{r0[2]:08x}",
                "dif_ref_ctl1_byte": f"0x{(r0[0] >> 16) & 0xff:02x}",
            }

    # Mali GPU id decode
    mali_key = next((k for k in decoded if "Mali" in k), None)
    if mali_key:
        rows = decoded[mali_key]["raw"]
        r0 = rows.get("0x0000")
        if r0:
            gpu_id = r0[0]
            product_id = (gpu_id >> 16) & 0xffff
            product_map = {
                0x6956: "Mali-T62x", 0x6860: "Mali-T76x",
                0x7090: "Mali-G31", 0x7091: "Mali-G51",
                0x7092: "Mali-G71", 0x7093: "Mali-G31",
                0x7212: "Mali-G52", 0x6220: "Mali-G76",
            }
            decoded[mali_key]["gpu_id"] = {
                "raw": f"0x{gpu_id:08x}",
                "product_id": f"0x{product_id:04x}",
                "product": product_map.get(product_id, "unknown"),
                "major_rev": (gpu_id >> 8) & 0xff,
                "minor_rev": gpu_id & 0xff,
            }

    # Audio bus PDM ring buffer
    audio_key = next((k for k in decoded if "Audio bus" in k), None)
    if audio_key:
        rows = decoded[audio_key]["raw"]
        r100 = rows.get("0x0100")
        r110 = rows.get("0x0110")
        if r100 and r110 and len(r100) >= 4 and len(r110) >= 3:
            decoded[audio_key]["pdm_toddr"] = {
                "ringbuffer_start": f"0x{r100[2]:08x}",
                "ringbuffer_end": f"0x{r100[3]:08x}",
                "ringbuffer_size_bytes": r100[3] - r100[2],
                "write_pointer": f"0x{r110[2]:08x}",
            }

    return decoded


def load_kmod_and_collect():
    """Try to insmod peek_mmio.ko (looking in common paths), then parse output.

    If insmod returns EBUSY ("Device or resource busy") the module already
    ran during this boot and its dmesg output is still in the ring — we
    parse that, no fresh load needed."""
    candidates = [
        "/tmp/peek_mmio.ko",
        "/data/peek_mmio.ko",
        "./peek_mmio.ko",
    ]
    ko = next((p for p in candidates if Path(p).exists()), None)
    if not ko:
        return {"status": "no peek_mmio.ko found", "tried": candidates}

    out, err, rc = run(["insmod", ko], timeout=10)
    if rc != 0:
        # Already loaded? Check dmesg for existing peek_mmio: output and
        # parse it — the kmod's data is still in the ring buffer.
        existing, _, _ = run(["dmesg"])
        if "peek_mmio: === end of dump ===" in existing:
            return {
                "status": "already loaded this boot — parsing existing dmesg",
                "ko_path": ko,
                "sections": parse_kmod_dmesg(),
            }
        return {
            "status": "insmod failed",
            "stderr": err.strip(),
            "ko_path": ko,
        }
    time.sleep(0.5)
    return {"status": "loaded", "ko_path": ko, "sections": parse_kmod_dmesg()}


# -----------------------------------------------------------------------
# main
# -----------------------------------------------------------------------

def main():
    serial = (slurp("/proc/device-tree/serial-number") or "").strip().rstrip("\x00")

    doc = {
        "meta": {
            "tool": TOOL_NAME,
            "version": VERSION,
            "captured_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "device_serial": serial or None,
        },
        "system":       collect_system(),
        "soc":          collect_soc(),
        "memory":       collect_memory(),
        "thermal":      collect_thermal(),
        "iio":          collect_iio(),
        "emmc":         collect_emmc(),
        "i2c":          collect_i2c(),
        "drm":          collect_drm(),
        "modules":      collect_modules(),
        "usb":          collect_usb(),
        "net":          collect_net(),
        "gpu_sysfs":    collect_gpu_sysfs(),
        "debugfs":      collect_debugfs_dumps(),
        "block":        collect_block(),
        "proc":         collect_proc(),
        "processes":    collect_processes(),
        "dtb":          walk_dtb(),
        "kmod_mmio":    load_kmod_and_collect(),
    }
    json.dump(doc, sys.stdout, indent=2, default=str, sort_keys=False)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
