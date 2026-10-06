# Narrow additive/removal editor for stock normal/WCP SM launch anchors.
# Marked blocks are standalone subshell-helper calls, never sourced code.
function fail(s){print "edit_autostart: " s > "/dev/stderr";bad=1;exit 2}
function block(v){
 print "# MX5DR ONE-BOOT BEGIN " v
 print "MX5DR_TRIAL=''"
 print "if [ -x /data_persist/mx5-aa-dr/guard/mx5dr-guard ]; then"
 # The SM launch waits for this call. The stock BusyBox 1.19.2 timeout applet
 # bounds it (the guard also bounds itself); any failure keeps the baseline.
 print "    if [ -x /usr/bin/timeout ]; then"
 print "        MX5DR_TRIAL=$(/usr/bin/timeout -t 15 -s KILL /data_persist/mx5-aa-dr/guard/mx5dr-guard select \"$" v "\") || MX5DR_TRIAL=''"
 print "    else"
 print "        MX5DR_TRIAL=$(/data_persist/mx5-aa-dr/guard/mx5dr-guard select \"$" v "\") || MX5DR_TRIAL=''"
 print "    fi"
 print "fi"
 print "case \"$MX5DR_TRIAL\" in"
 print "    /tmp/mx5dr-trial-??????/sm.conf)"
 print "        " v "=$MX5DR_TRIAL"
 print "        if [ -r /data_persist/mx5-aa-dr/tools/start_collector.sh ]; then"
 print "            /bin/sh /data_persist/mx5-aa-dr/tools/start_collector.sh 28800 || :"
 print "        fi"
 # Persistent BETA: the guard confirms this boot 90 s later, in the
 # background, if the SM still runs with this trial. Never delays the SM.
 print "        if [ -f /data_persist/mx5-aa-dr/guard/persist ]; then"
 print "            ( trap '' HUP; /bin/sleep 90; exec /data_persist/mx5-aa-dr/guard/mx5dr-guard confirm ) </dev/null >/dev/null 2>&1 &"
 print "        fi"
 print "        ;;"
 print "esac"
 print "# MX5DR ONE-BOOT END " v
}
{
 if($0 ~ /\r/)fail("CRLF unsupported")
 if($0 ~ /^# MX5DR ONE-BOOT BEGIN SMCFG_(NORMALMODE|WCPMODE)$/){if(skip)fail("nested block");skip=1;marker=$NF;if(seen[marker]++)fail("duplicate owned block");blocks++;next}
 if($0 ~ /^# MX5DR ONE-BOOT END SMCFG_(NORMALMODE|WCPMODE)$/){if(!skip||$NF!=marker)fail("unmatched end");skip=0;next}
 if(skip)next
 if(index($0,"MX5DR ONE-BOOT"))fail("unrecognized marker")
 lines[++n]=$0
 if($0 ~ /^[ \t]*taskset 0x02 \/jci\/sm\/sm -f \$SMCFG_NORMALMODE -e \/tmp\/smevents.txt &[ \t]*$/){normal++;which[n]="SMCFG_NORMALMODE"}
 if($0 ~ /^[ \t]*taskset 0x02 \/jci\/sm\/sm -f \$SMCFG_WCPMODE -e \/tmp\/smevents.txt &[ \t]*$/){wcp++;which[n]="SMCFG_WCPMODE"}
}
END{
 if(bad)exit 2
 if(skip||normal!=1||wcp!=1||(blocks!=0&&blocks!=2))fail("expected unique normal/WCP anchors and paired owned blocks")
 if(action!="add"&&action!="remove")fail("unknown action")
 for(i=1;i<=n;i++){if(action=="add"&&which[i]!="")block(which[i]);print lines[i]}
}
