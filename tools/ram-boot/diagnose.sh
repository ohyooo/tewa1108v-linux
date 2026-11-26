#!/bin/sh
# Run once before the interactive Bash. All failure paths return to the shell.
# Exact clock/reset and CPU FAM staging diagnostic, no forced insmod,
# no write to flash, no DMA data-plane traffic, no module removal.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH LC_ALL=C
cd /tewa || exit 1
report(){ printf '%s\n' "$*"; }
DT=/sys/firmware/devicetree/base
report TEWA160_DIAG_BEGIN
uname -a
cat /etc/os-release
arch=$(dpkg --print-architecture 2>/dev/null)
report "TEWA160_USERSPACE_ARCH=$arch"
report "TEWA160_RUNNING_RELEASE=$(uname -r)"
cat /proc/cmdline
if [ -r /sys/firmware/fdt ]; then
    printf 'TEWA160_EFFECTIVE_FDT_SHA256 '; sha256sum /sys/firmware/fdt
else report TEWA160_EFFECTIVE_FDT_RAW_UNAVAILABLE; fi
# Actual live device-tree critical resources and absence of all experimental
# opt-ins are checked before any module probes. Hash the actual property bytes.
if [ "$arch" != armel ] || [ "$(uname -r)" != 7.3.5 ]; then
    report TEWA160_DIAG_BLOCKED_WRONG_ABI
    report TEWA160_DIAG_DONE
    exit 1
fi
if ! sha256sum -c /tewa/module-sha256.txt; then
    report TEWA160_DIAG_BLOCKED_MODULE_HASH
    report TEWA160_DIAG_DONE
    exit 1
fi
if [ ! -d "$DT" ] || ! sha256sum -c /tewa/live-dt-sha256.txt; then
    report TEWA160_DIAG_BLOCKED_LIVE_DT_MISMATCH
    report TEWA160_DIAG_DONE
    exit 1
fi
if ! gates=$(find "$DT" -name 'hsan,experimental-*' -print 2>/dev/null); then
    report TEWA160_DIAG_BLOCKED_EXPERIMENTAL_DT
    report TEWA160_DIAG_DONE
    exit 1
fi
for gate in $gates; do
    case "$gate" in
        "$DT/lsw_dp/hsan,experimental-program-global-registers"|"$DT/lsw_dp/hsan,experimental-program-dp-register-init"|"$DT/lsw_dp/hsan,experimental-arm-fam-oem-registers"|"$DT/lsw_dp/hsan,experimental-program-eqm-bootstrap"|"$DT/lsw_dp/hsan,experimental-program-eqm-per-port"|"$DT/lsw_dp/hsan,experimental-verify-eqm-readback"|"$DT/lsw_dp/hsan,experimental-program-queue-routing"|"$DT/pie@12080000/hsan,experimental-reset-pie-at-boot"|"$DT/lsw_dp/hsan,experimental-program-pie-v300-map"|"$DT/lsw_dp/hsan,experimental-verify-pie-map-data"|"$DT/lsw_dp/hsan,experimental-reset-dp-at-boot"|"$DT/lsw_dp/hsan,experimental-prepare-fam-dma-shadow"|"$DT/lsw_dp/hsan,experimental-enable-dp-clocks"|"$DT/lsw_pfe/hsan,experimental-enable-pfe-clock"|"$DT/lsw_pfe/hsan,experimental-program-global-registers"|"$DT/lsw_pfe/hsan,experimental-program-opp-prelude"|"$DT/pie@12080000/hsan,experimental-program-pie-acp-at-boot"|"$DT/lsw_pfe/hsan,experimental-program-lan1-opp"|"$DT/lsw_pfe/hsan,experimental-program-lan1-pub"|"$DT/lsw_pfe/hsan,experimental-program-ifc-defaults"|"$DT/lsw_pfe/hsan,experimental-ifc-cpu-path"|"$DT/pie@12080000/hsan,experimental-cpu-lan1-diagnostic"|"$DT/pie@12080000/hsan,experimental-dma-enable"|"$DT/gemac@14400000/hsan,experimental-program-oem-gemac-link"|"$DT/pie@12080000/hsan,experimental-program-pie-global-registers"|"$DT/lsw_dp/hsan,experimental-program-ips-queues"|"$DT/lsw_pfe/hsan,experimental-program-ofc-control"|"$DT/lsw_dp/hsan,experimental-enable-ips-before-fam"|"$DT/pie@12080000/hsan,experimental-noncoherent-dma"|"$DT/gemac@14400000/hsan,experimental-read-gemac-oem-counters"|"$DT/lsw_pfe/hsan,experimental-read-pfe-oem-diagnostics"|"$DT/lsw_pfe/hsan,experimental-program-pa-prelude"|"$DT/lsw_pfe/hsan,experimental-program-cpu-opp"|"$DT/lsw_pfe/hsan,experimental-program-mcc-registers"|"$DT/lsw_pfe/hsan,experimental-mirror-lan1-to-cpu"|"$DT/lsw_dp/hsan,experimental-eqm-queue-counters") ;;
        *) report TEWA160_DIAG_BLOCKED_EXPERIMENTAL_DT; report TEWA160_DIAG_DONE; exit 1;;
    esac
done
case "$(cat /proc/cmdline)" in
    *experimental*|*module.sig_enforce=0*|*modprobe.blacklist=*)
        report TEWA160_DIAG_BLOCKED_UNEXPECTED_CMDLINE
        report TEWA160_DIAG_DONE
        exit 1;;
esac
report TEWA160_LIVE_DT_PROFILE_OK
report TEWA160_CURRENT_MODULES_SHA256_OK
# Normal init_module without force. Standard kernel checks apply. Probe uses
# actual device I/O (MDIO controller transactions), not a passive snapshot.
/tewa/driver-test
load_rc=$?
report "TEWA160_LOADER_EXIT=$load_rc"
probe_wait=0
while [ "$probe_wait" -lt 30 ]; do
    if [ -d /sys/class/net/lan1 ] && [ -d /sys/class/watchdog/watchdog0 ] &&
       [ -L /sys/bus/platform/devices/15000000.lsw_dp/driver ] &&
       [ -L /sys/bus/platform/devices/16100000.lsw_pfe/driver ]; then
        break
    fi
    sleep 1
    probe_wait=$((probe_wait+1))
done
report "TEWA_DEVICE_PROBE_WAIT_SECONDS=$probe_wait"
# Built-in Linux dw_wdt is the only new device driver; inspect sysfs without
# opening /dev/watchdog or modifying WDT registers from userspace.
# WATCHDOG_SYSFS controls identity/state/timeout, not class registration.
# The pinned kernel omits it: missing identity must never mean no device.
WD=/sys/class/watchdog/watchdog0
if [ -d "$WD" ] && [ -r "$WD/dev" ]; then
    report TEWA161_WDT_DEVICE_PRESENT=1
    report "TEWA161_WDT_DEV=$(cat "$WD/dev")"
    wd_driver=$(readlink -f "$WD/device/driver" 2>/dev/null)
    report "TEWA161_WDT_DRIVER=$wd_driver"
    case "$wd_driver" in
        */dw_wdt) report TEWA161_WDT_DRIVER_BOUND=1;;
        *) report TEWA161_WDT_DRIVER_BOUND=0;;
    esac
    if [ -r "$WD/identity" ]; then
        identity=$(cat "$WD/identity")
        report "TEWA161_WDT_IDENTITY=$identity"
        report TEWA161_WDT_STATUS_ATTRIBUTES=AVAILABLE
    else
        report TEWA161_WDT_STATUS_ATTRIBUTES=UNAVAILABLE
    fi
    if [ -r /sys/class/watchdog/watchdog0/state ]; then
        wdstate=$(cat /sys/class/watchdog/watchdog0/state 2>/dev/null)
        report "TEWA161_WDT_STATE=$wdstate"
    fi
    if [ -r /sys/class/watchdog/watchdog0/timeout ]; then
        wdtimeout=$(cat /sys/class/watchdog/watchdog0/timeout 2>/dev/null)
        report "TEWA161_WDT_TIMEOUT=$wdtimeout"
    fi
else
    report TEWA161_WDT_DEVICE_PRESENT=0
    report TEWA161_WDT_DRIVER_BOUND=0
    report TEWA161_WDT_PROBE_UNVERIFIED
fi
# Preserve the effective WDT properties and platform binding independently.
# Hashes identify bytes, not hardware readiness; do not open /dev/watchdog.
report TEWA_WDT_DT_PROPERTIES_BEGIN
for key in compatible reg status clocks clock-names resets timeout-sec; do
    property="$DT/watchdog@0x10128000/$key"
    if [ -r "$property" ]; then
        sha256sum "$property"
    else
        report "TEWA_WDT_DT_PROPERTY_ABSENT=$key"
    fi
done
report TEWA_WDT_DT_PROPERTIES_END
for device in /sys/bus/platform/devices/*; do
    [ -L "$device/of_node" ] || continue
    case "$(readlink -f "$device/of_node")" in
        */watchdog@0x10128000)
            report "TEWA_WDT_PLATFORM_DEVICE=$device"
            report "TEWA_WDT_PLATFORM_DRIVER=$(readlink -f "$device/driver" 2>/dev/null)"
            if [ -r "$device/waiting_for_supplier" ]; then
                report "TEWA_WDT_WAITING_FOR_SUPPLIER=$(cat "$device/waiting_for_supplier")"
            fi;;
    esac
done
if [ -r /sys/kernel/debug/devices_deferred ]; then
    report TEWA_WDT_DEFERRED_BEGIN
    cat /sys/kernel/debug/devices_deferred
    report TEWA_WDT_DEFERRED_END
fi
report TEWA160_PROC_MODULES_BEGIN
cat /proc/modules
report TEWA160_PROC_MODULES_END
live=0
for n in hsan_lsw_crg hsan_mdio hsan_lsw_csr hsan_lsw_platform hsan_lsw_dp hsan_lsw_pfe hsan_gemac_lan1 hsan_net_v300; do
    if grep -Eq "^$n [0-9]+ [^ ]+ [^ ]+ Live " /proc/modules; then
        live=$((live+1))
    fi
done
report "TEWA160_LIVE_COUNT=$live"
owner=$(readlink -f /sys/class/net/lan1/device/driver/module 2>/dev/null)
case "$owner" in */hsan_net_v300) owner_ok=1;; *) owner_ok=0;; esac
report "TEWA160_LAN1_OWNER=$owner_ok"
printf 'TEWA160_OWNER_PATH=%s\n' "$owner"
report TEWA160_BOUND_DEVICES_BEGIN
for n in /sys/bus/platform/devices/*/driver/module; do
    [ -L "$n" ] || continue
    m=$(readlink -f "$n")
    case "$m" in */hsan_*) printf '%s -> %s\n' "$n" "$m";; esac
done
report TEWA160_BOUND_DEVICES_END
phy=/sys/bus/mdio_bus/devices/hsan-14600000:03
id=$(cat "$phy/phy_id" 2>/dev/null)
report "TEWA160_PHY3_ID=$id"
report TEWA160_PHY_LOG_BEGIN
dmesg | grep -E 'HSAN MDIO PHY3|HSAN PHY3 BMSR|LAN1 OEM port|Stage79 PIE|datapath|GRO|refusing DMA' | tail -n 50
report TEWA160_PHY_LOG_END
report TEWA160_NETDEV_BEGIN
cat /proc/net/dev
for n in address flags operstate carrier mtu statistics/rx_packets statistics/tx_packets statistics/rx_errors statistics/tx_errors; do
    if [ -e "/sys/class/net/lan1/$n" ]; then
        printf 'lan1 %s: ' "$n"; cat "/sys/class/net/lan1/$n" 2>/dev/null || printf 'unavailable\n'
    fi
done
report TEWA160_NETDEV_END
# No experimental-dma-enable: exact Stage160 ndo_open returns -EACCES before
# queues/DMA. Test that existing rejection once, without any traffic or down/up.
if [ "$load_rc" -eq 0 ] && [ "$live" -eq 8 ] && [ "$owner_ok" -eq 1 ] && [ "$id" = 0x20669a22 ] && { [ ! -e "$DT/pie@12080000/hsan,experimental-dma-enable" ] || [ -e "$DT/pie@12080000/hsan,experimental-cpu-lan1-diagnostic" ]; }; then
    /tewa/gate-test
    gate_rc=$?
    report "TEWA160_GATE_HELPER_EXIT=$gate_rc"
else
    report TEWA160_GATE_NOT_RUN_PREREQUISITES
fi
report TEWA_LSW_CRG_DIAGNOSTICS_BEGIN
cat /sys/module/hsan_lsw_csr/parameters/experimental_allow_lsw_writes
cat /sys/bus/platform/devices/14980000.clk/register_status
if [ -r /sys/kernel/debug/clk/clk_summary ]; then
    grep -E 'lsw_dp_clk0|lsw_pfe_clk0' /sys/kernel/debug/clk/clk_summary
fi
dmesg | grep -E 'hsan-lsw-crg|DP clock|PFE clock|FAM|LSW bank|defer' | tail -n 80
report TEWA_LSW_CRG_DIAGNOSTICS_END
report TEWA160_DIAG_DONE
exit 0
