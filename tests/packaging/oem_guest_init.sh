#!/bin/sh
# Diagnostic PID 1 for an isolated full-system ARM VM, not a CMU boot script.
# OEM executables/libraries are unchanged. Missing devices remain failures.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/jci/bin
export LD_LIBRARY_PATH=/lib:/usr/lib:/jci/lib
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /dev/shm /dev/mqueue
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /dev/shm
mount -t mqueue mqueue /dev/mqueue
mount -t tmpfs tmpfs /tmp
mkdir -p /tmp/mnt/data /tmp/mnt/data_persist /tmp/var/run/dbus /tmp/oem-check
chmod 1777 /tmp /dev/shm
ifconfig lo 127.0.0.1 up
syslogd -O /tmp/oem-check/syslog.log
echo 'VM_SCOPE=OEM userspace; diagnostic PID1; virtual board; no physical CMU devices'
cat /proc/version
id
mode=baseline
phase=services
for arg in $(cat /proc/cmdline); do
    case "$arg" in mx5mode=*) mode=${arg#*=};; mx5phase=*) phase=${arg#*=};; esac
done
echo "VM_MODE=$mode VM_PHASE=$phase"
touch_preload=
if [ -f /validation/touch.so ]; then
    mkdir -p /data_persist/oem-aa-mod
    cp /validation/touch.so /data_persist/oem-aa-mod/libpatch-blmjciaapa.so
    touch_preload=/data_persist/oem-aa-mod/libpatch-blmjciaapa.so
    echo 'VM_EXISTING_TOUCH=provided private ARM library'
    for config in /jci/sm/sm.conf /jci/sm/sm_WCP.conf; do
        awk -v path="$touch_preload" '{print}
            /<service .*name="jciAAPA"/ {
                print "            <environ_var env_name=\"LD_PRELOAD\" env_value=\"" path "\"/>"
            }' "$config" > "$config.vm-touch" && mv "$config.vm-touch" "$config"
    done
fi
if [ "$(uname -r)" = 3.0.35 ]; then
    echo 'VM_DRIVERS=unmodified stock modules on emulated CMU machine ID'
    for module in kernel/jci/com_jci.ko kernel/jci/com_jci_cpp_nvram.ko \
        kernel/drivers/misc/cmu_io.ko kernel/jci/com.jci.cpp.drivers.Log.ko \
        kernel/jci/com.jci.cpp.drivers.GpioChip.ko kernel/drivers/spi/spidev.ko \
        kernel/drivers/input/evdev.ko \
        kernel/drivers/input/misc/uinput.ko; do
        timeout -t 5 insmod "/lib/modules/3.0.35/$module"
        rc=$?
        echo "VM_MODULE_RESULT=$rc $module"
    done
    ls -l /sys/class/gpio /dev/cmu_io /dev/spidev* /dev/uinput
fi
# These are the original service and HMI bus daemons and configurations.
dbus start
. /etc/profile
if [ -f /tmp/dbus.env ]; then . /tmp/dbus.env; fi
echo "BUS_PIDS=$(cat /tmp/dbus_service.pid /tmp/dbus_hmi.pid 2>/dev/null)"
if [ "$mode" = shadow ]; then
    if ! (cd /validation/usb && sh install.sh); then
        echo 'VM_INSTALL_FAILED'
        exec /bin/sh
    fi
    trial=$(/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf)
    rc=$?
    echo "GUARD_SELECT_RC=$rc TRIAL=$trial"
    [ "$rc" = 0 ] || exec /bin/sh
    sh /data_persist/mx5-aa-dr/tools/start_collector.sh 20
    sleep 1
    for process in /proc/[0-9]*; do
        case "$(readlink "$process/exe" 2>/dev/null)" in
            */mx5dr-collector)
                echo "VM_COLLECTOR_IDENTITY $process"
                grep -E '^(Name|Pid|Uid|Gid|Groups):' "$process/status"
                ;;
        esac
    done
else
    trial=/jci/sm/sm.conf
fi
# The original init starts this binary after the modules above. It can fail
# when the virtual board lacks the physical SPI peer; preserve that result.
(strace -ff -tt -e trace=file,process,network,ipc,ioctl -o /tmp/oem-check/vim.syscalls /jci/vim/vim_app; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/vim.exit) > /tmp/oem-check/vim.log 2>&1 &
echo "VIM_TRACE_WRAPPER_PID=$!"
case "$phase" in
    services)
        # Execute the unchanged full SM configuration. Do not fabricate the
        # external PID files or replace failed services with success stubs.
        (strace -ff -tt -e trace=file,process,network,ipc -o /tmp/oem-check/sm.syscalls taskset 0x02 /jci/sm/sm -f "$trial" -e /tmp/smevents.txt; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/sm.exit) > /tmp/oem-check/sm.log 2>&1 &
        sm_pid=$!
        echo "SM_PID=$sm_pid"
        ;;
    standalone)
        # The OEM launcher documents this mode; original dependencies can fail.
        (strace -ff -tt -e trace=file,process,network,ipc -o /tmp/oem-check/aap-service.syscalls /usr/bin/aap_service; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/aap-service.exit) > /tmp/oem-check/aap-service.log 2>&1 &
        echo "AAP_SERVICE_TRACE_WRAPPER_PID=$!"
        aa_preload=$touch_preload
        vbs_preload=
        if [ "$mode" = shadow ]; then
            aa_preload=/data_persist/mx5-aa-dr/libmx5dr.so${touch_preload:+:$touch_preload}
            vbs_preload=/data_persist/mx5-aa-dr/libmx5dr-vimtap.so
        fi
        (strace -ff -tt -e trace=file,process,network,ipc -o /tmp/oem-check/settings.syscalls /jci/sm/sm_svclauncher -s settings /jci/settings/svc-com-jci-cpp-settings.so 0 -a --uri=server:// --proxy=tcpip://; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/settings.exit) > /tmp/oem-check/settings.log 2>&1 &
        echo "SETTINGS_TRACE_WRAPPER_PID=$!"
        sleep 2
        (strace -ff -tt -e trace=file,process,network,ipc -o /tmp/oem-check/vbs.syscalls /bin/sh -c 'export LD_PRELOAD="$1" LD_DEBUG=libs; exec /jci/sm/sm_svclauncher -s jciVBS /jci/vbs/svcjcivbs.so 0 -a' vm "$vbs_preload"; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/vbs.exit) > /tmp/oem-check/vbs.log 2>&1 &
        vbs_pid=$!
        echo "VBS_LAUNCHER_PID=$vbs_pid"
        (strace -ff -tt -e trace=file,process,network,ipc -o /tmp/oem-check/aa.syscalls /bin/sh -c 'export LD_PRELOAD="$1" LD_DEBUG=libs; exec /jci/sm/sm_svclauncher -s jciAAPA /jci/aapa/blmjciaapa.so 0 -a' vm "$aa_preload"; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/aa.exit) > /tmp/oem-check/aa.log 2>&1 &
        aa_pid=$!
        echo "AA_LAUNCHER_PID=$aa_pid"
        ;;
    initprobe)
        strace -ff -tt -o /tmp/oem-check/init.syscalls /sbin/init_target > /tmp/oem-check/init.log 2>&1 &
        sm_pid=$!
        echo "INIT_PROBE_PID=$sm_pid (diagnostic child, not PID1)"
        ;;
    *) echo 'UNKNOWN_VM_PHASE';;
esac
sleep 25
echo 'VM_PROCESS_SNAPSHOT_BEGIN'
ps -ef
echo 'VM_PROCESS_SNAPSHOT_END'
for process in /proc/[0-9]*; do
    executable=$(readlink "$process/exe" 2>/dev/null)
    case "$executable" in /jci/sm/sm|/jci/sm/sm_svclauncher|/jci/vim/vim_app|/usr/bin/aap_service|*/mx5dr-collector) ;; *) continue;; esac
    pid=${process#/proc/}
    if [ -r "$process/maps" ]; then
        echo "VM_MAPS_BEGIN $pid"
        cat "/proc/$pid/maps"
        echo "VM_MAPS_END $pid"
    fi
done
timeout -t 5 /jci/sm/smctl -g 2>&1
for file in /tmp/oem-check/* /tmp/smevents.txt /data_persist/mx5-aa-dr/logs/*.jsonl; do
    [ -f "$file" ] || continue
    echo "VM_LOG_BEGIN $file"
    case "$file" in
        *.syscalls.*) grep -E 'execve|exit_group|SIG[A-Z]+|mx5dr|Watchdog|cmu_io|spidev|/dev/shm' "$file"; tail -n 40 "$file";;
        *) cat "$file";;
    esac
    echo "VM_LOG_END $file"
done
echo 'VM_INSPECTION_SHELL_READY'
exec /bin/sh
