#!/bin/sh
# Diagnostic PID 1 for oem_session_probe.cpp; never install on a vehicle.
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
mkdir -p /tmp/mnt/data /tmp/mnt/data_persist /tmp/var/run/dbus
chmod 1777 /tmp /dev/shm
ifconfig lo 127.0.0.1 up
syslogd -O /tmp/session-syslog.log
echo 'VM_SCOPE=original session APIs; diagnostic PID1; no phone or physical CMU devices'
case " $(cat /proc/cmdline) " in
    *' mx5mode=baseline mx5phase=session '*) ;;
    *) echo 'VM_SESSION_PROBE_RC=64'; exec /bin/sh;;
esac
for module in kernel/jci/com_jci.ko kernel/jci/com_jci_cpp_nvram.ko kernel/drivers/misc/cmu_io.ko kernel/jci/com.jci.cpp.drivers.Log.ko; do
    timeout -t 5 insmod "/lib/modules/3.0.35/$module"
    echo "VM_MODULE_RC=$? $module"
done
dbus start
. /etc/profile
[ ! -f /tmp/dbus.env ] || . /tmp/dbus.env
/usr/bin/aap_service > /tmp/session-service.log 2>&1 &
service_pid=$!
sleep 3
echo "VM_SESSION_SERVICE_PID=$service_pid EXE=$(readlink /proc/$service_pid/exe)"
/validation/session-probe --isolated-oem-vm
echo "VM_SESSION_PROBE_RC=$?"
sleep 1
echo VM_SESSION_SERVICE_LOG_BEGIN
cat /tmp/session-service.log
echo VM_SESSION_SERVICE_LOG_END
echo VM_SESSION_SYSLOG_BEGIN
cat /tmp/session-syslog.log
echo VM_SESSION_SYSLOG_END
echo VM_SESSION_DONE
exec /bin/sh
