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
retry_config=full
for arg in $(cat /proc/cmdline); do
    case "$arg" in mx5mode=*) mode=${arg#*=};; mx5phase=*) phase=${arg#*=};; mx5retry=*) retry_config=${arg#*=};; esac
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
    collector_seconds=20
    [ "$phase" != location ] || collector_seconds=60
    [ "$phase" != location-sm ] || collector_seconds=120
    sh /data_persist/mx5-aa-dr/tools/start_collector.sh "$collector_seconds"
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
# Only the retry diagnostic uses these helpers. No script below is included in
# the USB bundle or modifies an installed CMU service configuration.
retry_aa_pids() {
    for retry_process in /proc/[0-9]*; do
        [ "$(readlink "$retry_process/exe" 2>/dev/null)" = /jci/sm/sm_svclauncher ] || continue
        retry_argv=$(tr '\000' ' ' < "$retry_process/cmdline" 2>/dev/null)
        case "$retry_argv" in *' jciAAPA '*) echo "${retry_process#/proc/}";; esac
    done
}
retry_snapshot() {
    echo "VM_RETRY_SNAPSHOT_BEGIN=$1 UPTIME=$(cat /proc/uptime)"
    for retry_process in /proc/[0-9]*; do
        retry_exe=$(readlink "$retry_process/exe" 2>/dev/null)
        case "$retry_exe" in /jci/sm/sm|/jci/sm/sm_svclauncher) ;; *) continue;; esac
        retry_argv=$(tr '\000' ' ' < "$retry_process/cmdline" 2>/dev/null)
        case "$retry_exe:$retry_argv" in /jci/sm/sm:*|*' jciAAPA '*|*' settings '*) ;; *) continue;; esac
        echo "VM_RETRY_PROCESS=${retry_process#/proc/} ARGV=$retry_argv"
        grep -E '^(Name|Pid|PPid|Uid|Gid):' "$retry_process/status"
        tr '\000' '\n' < "$retry_process/environ" | grep '^LD_PRELOAD='
        grep -E '/jci/aapa/|libmx5dr|libpatch-blmjciaapa' "$retry_process/maps"
    done
    for retry_marker in arm consumed last-boot; do
        if [ -f "/data_persist/mx5-aa-dr/guard/$retry_marker" ]; then
            echo "VM_RETRY_GUARD_$retry_marker=present"
        else
            echo "VM_RETRY_GUARD_$retry_marker=absent"
        fi
    done
    for retry_trace in /data_persist/mx5-aa-dr/logs/trace.[012].jsonl; do
        [ -f "$retry_trace" ] || continue
        echo "VM_RETRY_RUNTIME_RECORDS=$retry_trace"
        grep '"kind":"boot"' "$retry_trace"
        grep '"kind":"health"' "$retry_trace" | tail -n 1
    done
    echo "VM_RETRY_SNAPSHOT_END=$1"
}
retry_ctl() {
    retry_label=$1
    shift
    echo "VM_RETRY_CTL_BEGIN=$retry_label UPTIME=$(cat /proc/uptime)"
    timeout -t 12 /jci/sm/smctl "$@" > "/tmp/oem-check/retry-$retry_label.log" 2>&1
    retry_rc=$?
    cat "/tmp/oem-check/retry-$retry_label.log"
    echo "VM_RETRY_CTL_END=$retry_label RC=$retry_rc UPTIME=$(cat /proc/uptime)"
}
retry_probe() {
    echo "VM_RETRY_SCOPE=config=$retry_config; exact OEM SM/launcher/AA; diagnostic operations only"
    retry_trial=$trial
    case "$retry_config" in
        full) echo 'VM_RETRY_CONFIG=full original service graph plus existing touch and guarded trial tokens';;
        reduced)
            # Keep the original server/global settings and settings/jciAAPA
            # attributes, including retry_count=0 and reset_board=yes. A real
            # settings service keeps this from becoming an all-services-stop
            # experiment when AA is restarted. Omit the other services and the
            # eight dependency edges. This is not a full OEM boot or a proposed
            # production configuration.
            retry_trial=/tmp/oem-check/retry-sm.conf
            # RETRY_CONFIG_AWK_BEGIN
            awk '
                {
                    # Match only live XML markup, retaining original comment
                    # lines inside selected services. Mixed multiline comment
                    # and markup lines are outside this diagnostic format.
                    remaining=$0; code=""; was_comment=comment
                    while (length(remaining)) {
                        if (comment) {
                            end=index(remaining, "-->")
                            if (!end) break
                            remaining=substr(remaining, end+3); comment=0
                        } else {
                            begin=index(remaining, "<!--")
                            if (!begin) { code=code remaining; break }
                            code=code substr(remaining, 1, begin-1)
                            remaining=substr(remaining, begin+4); comment=1
                        }
                    }
                    if ((was_comment || comment) && code !~ /^[[:space:]]*$/) exit 2
                }
                code ~ /^[[:space:]]*<sm_config([[:space:]][^<>]*)?>[[:space:]]*$/ {
                    if (root_open || services || closed_root || code ~ /\/>[[:space:]]*$/) exit 2
                    root_open=1; print; next
                }
                code ~ /^[[:space:]]*<\/services>[[:space:]]*$/ {
                    if (!services || active || closed_services) exit 2
                    closed_services=1; next
                }
                code ~ /^[[:space:]]*<\/sm_config>[[:space:]]*$/ {
                    if (!root_open || !closed_services || closed_root) exit 2
                    closed_root=1; next
                }
                !services {
                    if (code ~ /<services /) {
                        if (!root_open) exit 2
                        services=1
                    }
                    print; next
                }
                closed_services { if (code !~ /^[[:space:]]*$/) exit 2; next }
                code ~ /^[[:space:]]*<service .*name="/ {
                    if (active) exit 2
                    name=code; sub(/^.* name="/, "", name); sub(/".*$/, "", name)
                    active=(name == "jciAAPA" || name == "settings")
                    if (active) {
                        if (name == "jciAAPA") aa++; else settings++
                        print
                    }
                    if (code ~ /\/>[[:space:]]*$/) active=0
                    next
                }
                active {
                    if (code ~ /^[[:space:]]*<dependency /) removed++
                    else print
                    if (code ~ /<\/service>/) active=0
                }
                END {
                    if (comment || active || !root_open || !services || !closed_services || !closed_root) exit 2
                    if (aa != 1 || settings != 1 || removed != 8) exit 2
                    print "    </services>\n</sm_config>"
                }
            ' "$trial" > "$retry_trial" || {
                rm -f "$retry_trial"
                echo 'VM_RETRY_CONFIG_FAILED'
                return 1
            }
            # RETRY_CONFIG_AWK_END
            echo 'VM_RETRY_CONFIG=reduced original settings and jciAAPA; eight dependency edges omitted; retry/reset/timeouts unchanged'
            # The real AA backend remains fallible and is not counted as
            # satisfying the omitted aap_service SM dependency.
            (strace -ff -tt -e trace=file,process,network,ipc -o /tmp/oem-check/aap-service.syscalls /usr/bin/aap_service; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/aap-service.exit) > /tmp/oem-check/aap-service.log 2>&1 &
            sleep 2
            ;;
        *) echo 'VM_RETRY_UNKNOWN_CONFIG'; return;;
    esac
    (strace -ff -tt -e trace=file,process,network,ipc -o /tmp/oem-check/sm.syscalls taskset 0x02 /jci/sm/sm -f "$retry_trial" -e /tmp/smevents.txt; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/sm.exit) > /tmp/oem-check/sm.log 2>&1 &
    echo "VM_RETRY_SM_TRACE_WRAPPER_PID=$!"
    sleep 8
    retry_ctl help --help
    retry_snapshot initial
    retry_ctl launch --launch --name jciAAPA --wait
    sleep 8
    retry_snapshot after-launch
    retry_ctl get-initial --get
    if [ -z "$(retry_aa_pids)" ]; then
        echo 'VM_RETRY_NO_AA_PROCESS=launch did not yield a live AA launcher; retry contract remains untested'
        return
    fi
    retry_ctl restart --restart --name jciAAPA --wait
    sleep 8
    retry_snapshot after-restart
    retry_ctl get-restarted --get
    # An externally requested SIGKILL models unexpected delayed child death;
    # it is not evidence that the preload itself crashed.
    sleep 15
    retry_victims=$(retry_aa_pids)
    if [ -z "$retry_victims" ]; then
        echo 'VM_RETRY_DELAYED_KILL_SKIPPED=no live AA launcher after restart; automatic crash policy not exercised'
        return
    fi
    echo "VM_RETRY_DELAYED_KILL_PIDS=$retry_victims UPTIME=$(cat /proc/uptime)"
    for retry_victim in $retry_victims; do kill -KILL "$retry_victim"; done
    # Observe automatic SM behavior before issuing a new start request. The
    # cumulative 65-second sleep spans the original 30-second ping timeout and
    # sigkill_wait_before_reboot interval; snapshots record actual guest time.
    retry_elapsed=0
    for retry_delay in 2 8 20 35; do
        sleep "$retry_delay"
        retry_elapsed=$((retry_elapsed + retry_delay))
        retry_snapshot "after-delayed-kill-$retry_elapsed"
        retry_ctl "get-after-kill-$retry_elapsed" --get
    done
    retry_ctl relaunch --launch --name jciAAPA --wait
    sleep 8
    retry_snapshot after-client-relaunch
    retry_ctl get-relaunched --get
    if [ "$mode" = shadow ]; then
        /data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf > /tmp/oem-check/retry-guard-select.log 2>&1
        echo "VM_RETRY_GUARD_SECOND_SELECT_RC=$?"
        cat /tmp/oem-check/retry-guard-select.log
    fi
    echo 'VM_RETRY_PROBE_FINISHED=observations only; inspect SM state, child identity and preload'
}
location_query() {
    location_label=$1
    shift
    echo "VM_LOCATION_QUERY_BEGIN=$location_label UID=$(id -u) UPTIME=$(cat /proc/uptime)"
    DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus_service_socket \
        timeout -t 3 /usr/bin/dbus-send --session --print-reply --reply-timeout=1500 "$@"
    location_rc=$?
    echo "VM_LOCATION_QUERY_END=$location_label RC=$location_rc UPTIME=$(cat /proc/uptime)"
}
location_snapshot() {
    location_stage=$1
    location_query "$location_stage.owner" --dest=org.freedesktop.DBus \
        /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner string:com.jci.lds.data
    location_query "$location_stage.position" --dest=com.jci.lds.data \
        /com/jci/lds/data com.jci.lds.data.GetPosition
    location_query "$location_stage.selected-gps" --dest=com.jci.lds.control \
        /com/jci/lds/control com.jci.lds.control.GetSelectedGPS_sync
    location_query "$location_stage.read-status" --dest=com.jci.lds.control \
        /com/jci/lds/control com.jci.lds.control.GetReadStatus_sync
}
location_sm_snapshot() {
    location_stage=$1
    location_snapshot "$location_stage" >> /tmp/oem-check/location-dbus.log 2>&1
    {
        echo "VM_LOCATION_SM_SNAPSHOT_BEGIN=$location_stage UPTIME=$(cat /proc/uptime)"
        timeout -t 5 /jci/sm/smctl -g
        echo "VM_LOCATION_SM_GET_RC=$?"
        # Read only original daemon-created PID files. A file alone is not
        # proof of readiness; record the actual process identity as well.
        for location_pid_file in /tmp/dbus_service.pid /tmp/dbus_hmi.pid /tmp/vim_app.pid; do
            if [ ! -f "$location_pid_file" ]; then
                echo "VM_LOCATION_EXTERNAL_PID_FILE=$location_pid_file absent"
                continue
            fi
            location_pid=$(cat "$location_pid_file")
            case "$location_pid" in
                ''|*[!0-9]*) echo "VM_LOCATION_EXTERNAL_PID_FILE=$location_pid_file invalid"; continue;;
            esac
            echo "VM_LOCATION_EXTERNAL_PID_FILE=$location_pid_file PID=$location_pid EXE=$(readlink "/proc/$location_pid/exe" 2>/dev/null)"
        done
        for location_process in /proc/[0-9]*; do
            location_exe=$(readlink "$location_process/exe" 2>/dev/null)
            case "$location_exe" in /jci/sm/sm|/jci/sm/sm_svclauncher|/jci/vim/vim_app|/usr/bin/aap_service) ;; *) continue;; esac
            location_argv=$(tr '\000' ' ' < "$location_process/cmdline" 2>/dev/null)
            echo "VM_LOCATION_PROCESS=${location_process#/proc/} EXE=$location_exe ARGV=$location_argv"
            grep -E '^(Name|Pid|PPid|Uid|Gid):' "$location_process/status"
            tr '\000' '\n' < "$location_process/environ" | grep '^LD_PRELOAD='
            grep -E '/jci/(sm|settings|time|usbmgr|vbs|lds|navi|aapa)/|libmx5dr|libpatch-blmjciaapa' "$location_process/maps"
        done
        echo "VM_LOCATION_SM_SNAPSHOT_END=$location_stage UPTIME=$(cat /proc/uptime)"
    } >> /tmp/oem-check/location-sm-state.log 2>&1
}
location_sm_probe() {
    echo 'VM_LOCATION_SM_SCOPE=19 original services in an explicitly partial graph; not a full OEM boot; no manual service Start or launch'
    location_trial=/tmp/oem-check/location-sm.conf
    # Keep the original global settings, complete service attributes, injected
    # trial environment, and every edge whose target is selected. The actual
    # OEM stage scripts and USB readiness script run unchanged. In particular,
    # usb_drivers can create its flag after module errors; that is not proof of
    # real USB hardware. NNG remains autorun=no with its original SD path.
    # LOCATION_SM_CONFIG_AWK_BEGIN
    awk '
        BEGIN {
            count=split("settings jciUSBMGR jciVBS jciLDS jcinavi jciBLMSettings jciTime aap_service jciAAPA stage_1 stage_2 stage_3 stage_navi usb_drivers vim_app dbus_service dbus_hmi NNG jciBLMTIME", names, " ")
            for (i=1; i<=count; i++) selected[names[i]]=1
        }
        {
            # Ignore markup inside XML comments. Preserve complete comment
            # lines only inside a selected block; refuse mixed multiline
            # comment/markup lines rather than emit an unmatched comment.
            remaining=$0; code=""; was_comment=comment
            while (length(remaining)) {
                if (comment) {
                    end=index(remaining, "-->")
                    if (!end) break
                    remaining=substr(remaining, end+3); comment=0
                } else {
                    begin=index(remaining, "<!--")
                    if (!begin) { code=code remaining; break }
                    code=code substr(remaining, 1, begin-1)
                    remaining=substr(remaining, begin+4); comment=1
                }
            }
            if ((was_comment || comment) && code !~ /^[[:space:]]*$/) exit 2
        }
        code ~ /^[[:space:]]*<sm_config([[:space:]][^<>]*)?>[[:space:]]*$/ {
            if (root_open || services || closed_root || code ~ /\/>[[:space:]]*$/) exit 2
            root_open=1; print; next
        }
        code ~ /^[[:space:]]*<\/services>[[:space:]]*$/ {
            if (!services || active || closed_services) exit 2
            closed_services=1; next
        }
        code ~ /^[[:space:]]*<\/sm_config>[[:space:]]*$/ {
            if (!root_open || !closed_services || closed_root) exit 2
            closed_root=1; next
        }
        !services {
            if (code ~ /<services /) {
                if (!root_open) exit 2
                services=1
            }
            print; next
        }
        closed_services { if (code !~ /^[[:space:]]*$/) exit 2; next }
        code ~ /^[[:space:]]*<service .*name="/ {
            if (active) exit 2
            name=code; sub(/^.* name="/, "", name); sub(/".*$/, "", name)
            active=(name in selected)
            if (active) { seen[name]++; print }
            if (code ~ /\/>[[:space:]]*$/) active=0
            next
        }
        active {
            if (code ~ /^[[:space:]]*<(dependency|connection) /) {
                target=code; sub(/^.* value="/, "", target); sub(/".*$/, "", target)
                if (target in selected) {
                    print
                    if (code ~ /<dependency /) kept_dependency++; else kept_connection++
                } else {
                    if (code ~ /<dependency /) removed_dependency++; else removed_connection++
                }
            } else print
            if (code ~ /<\/service>/) active=0
        }
        END {
            if (comment || active || !root_open || !services || !closed_services || !closed_root) exit 2
            for (name in selected) if (seen[name] != 1) exit 2
            if (kept_dependency != 21 || kept_connection != 2 || removed_dependency != 10 || removed_connection != 1) exit 2
            print "    </services>\n</sm_config>"
        }
    ' "$trial" > "$location_trial" || {
        rm -f "$location_trial"
        echo 'VM_LOCATION_SM_CONFIG_FAILED'
        return 1
    }
    # LOCATION_SM_CONFIG_AWK_END
    echo 'VM_LOCATION_SM_OMITTED_DEPENDENCY=stage_3->jciMMUI; jciBLMSettings->devices,audio_config,dsp_config,system_mazda_my14; aap_service->devicemanager; jciAAPA->devicemanager,audio_manager,jciRM,jciUpdatea'
    echo 'VM_LOCATION_SM_OMITTED_CONNECTION=jciBLMSettings->jciaudiosettings'
    echo 'VM_LOCATION_SM_CONFIG=21 internal dependencies and 2 internal connections retained; original attributes/environment/retry/reset/timeouts preserved'
    location_snapshot before-sm > /tmp/oem-check/location-dbus.log 2>&1
    (strace -ff -tt -e trace=file,process,network,ipc,ioctl -o /tmp/oem-check/sm.syscalls taskset 0x02 /jci/sm/sm -f "$location_trial" -e /tmp/smevents.txt; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/sm.exit) > /tmp/oem-check/sm.log 2>&1 &
    echo "VM_LOCATION_SM_TRACE_WRAPPER_PID=$!"
    sleep 8
    location_sm_snapshot after-sm
    sleep 45
    location_sm_snapshot after-start-timeout
}
case "$phase" in
    retry) retry_probe;;
    location-sm) location_sm_probe;;
    services)
        # Execute the unchanged full SM configuration. Do not fabricate the
        # external PID files or replace failed services with success stubs.
        (strace -ff -tt -e trace=file,process,network,ipc -o /tmp/oem-check/sm.syscalls taskset 0x02 /jci/sm/sm -f "$trial" -e /tmp/smevents.txt; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/sm.exit) > /tmp/oem-check/sm.log 2>&1 &
        sm_pid=$!
        echo "SM_PID=$sm_pid"
        ;;
    standalone|location)
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
        if [ "$phase" = location ]; then
            echo 'VM_LOCATION_SCOPE=standalone OEM LDS/navi; unchanged args and XML; no GPS or CAN input supplied'
            location_snapshot before-providers > /tmp/oem-check/location-dbus.log 2>&1
            (strace -ff -tt -e trace=file,process,network,ipc,ioctl -o /tmp/oem-check/lds.syscalls /jci/sm/sm_svclauncher -s jciLDS /jci/lds/svcjcilds.so 0 -a /jci/lds/lds ConfigFile=/jci/lds/lds.xml; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/lds.exit) > /tmp/oem-check/lds.log 2>&1 &
            echo "VM_LOCATION_LDS_TRACE_WRAPPER_PID=$!"
            sleep 3
            (strace -ff -tt -e trace=file,process,network,ipc,ioctl -o /tmp/oem-check/navi.syscalls /jci/sm/sm_svclauncher -s jcinavi /jci/navi/svcjcinavi.so 0 -a; rc=$?; echo "TRACE_WRAPPER_RC=$rc" > /tmp/oem-check/navi.exit) > /tmp/oem-check/navi.log 2>&1 &
            echo "VM_LOCATION_NAVI_TRACE_WRAPPER_PID=$!"
            sleep 3
            location_snapshot after-providers >> /tmp/oem-check/location-dbus.log 2>&1
        fi
        ;;
    initprobe)
        strace -ff -tt -o /tmp/oem-check/init.syscalls /sbin/init_target > /tmp/oem-check/init.log 2>&1 &
        sm_pid=$!
        echo "INIT_PROBE_PID=$sm_pid (diagnostic child, not PID1)"
        ;;
    *) echo 'UNKNOWN_VM_PHASE';;
esac
sleep 25
if [ "$phase" = location ] || [ "$phase" = location-sm ]; then
    if [ "$phase" = location-sm ]; then
        location_sm_snapshot late
    else
        location_snapshot late >> /tmp/oem-check/location-dbus.log 2>&1
    fi
    if [ "$mode" = shadow ]; then
        sh /data_persist/mx5-aa-dr/tools/stop_collector.sh
        echo "VM_LOCATION_COLLECTOR_STOP_RC=$?"
    fi
fi
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
    if [ "$phase" = retry ]; then
        case "$file" in
            *.syscalls.*)
                # Preserve the exact SM/AA process traces needed by this probe.
                # Printing every unrelated service thread can exhaust the
                # bounded VM deadline before runtime journals are exported.
                grep -qE 'execve\("/jci/sm/sm"|execve\("/jci/sm/sm_svclauncher".*"jciAAPA"' "$file" || continue
                ;;
        esac
    fi
    echo "VM_LOG_BEGIN $file"
    case "$file" in
        # Keep tap initialization evidence even when it is outside the final
        # thread tail. These are file/socket calls, not OEM read-buffer dumps.
        *.syscalls.*) grep -E 'execve|exit_group|SIG[A-Z]+|mx5dr|/data_persist/mx5-aa-dr/|libjcivim_api|svcjcivbs|libjcimod_can|/jci/vim/vim_app|/jci/lds/|/jci/navi/|/dev/ttymxc2|socket\((AF_UNIX|AF_LOCAL|PF_FILE|PF_LOCAL)|Watchdog|cmu_io|spidev|/dev/shm' "$file"; tail -n 40 "$file";;
        *) cat "$file";;
    esac
    echo "VM_LOG_END $file"
done
echo 'VM_INSPECTION_SHELL_READY'
exec /bin/sh
