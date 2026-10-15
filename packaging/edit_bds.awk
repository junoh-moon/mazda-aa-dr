# Narrow, fail-closed editors for the phone IMU Bluetooth service
# (validation/PHONE_IMU_CHANNEL_2026-10-14.md). Not a general XML parser.
# POSIX awk; also run with the stock BusyBox 1.19.2 awk in the release check.
#
# what=xml: input is the stock /jci/bds/BdsConfiguration.xml. Output is the
#   same bytes plus exactly ONE line before </serviceConfiguration>:
#   <serialPort id="15" name="MX5DR IMU" noOfInstances="1" critical="false"
#    enabled="true" uuidServer=<UUID> uuidClient=<UUID> writeDelay="3"/>
#   Refuses: CRLF, comments or unknown elements inside serviceConfiguration,
#   a missing or repeated block, a used id 15, our name or UUID already
#   present, duplicate ids, 20 or more serialPort entries already (bds keeps
#   at most 20), or a serialPort line without the stock attribute set.
# what=args action=add|remove xml=<path>: input is a Service Manager
#   configuration. Only the args value of the single process service "bds"
#   (path /jci/bds/bds) changes: add appends " -configFilePath <path>" to the
#   stock "-interface dbus" or "-interface dbus -wcp"; remove strips exactly
#   that suffix (absent: unchanged). Anything else is refused.
function die(s) { print "edit_bds: " s > "/dev/stderr"; bad=1; exit 2 }
function attr(tag,key, p,s) {
    p="[ \t]" key "=\"[^\"]*\""
    if (!match(tag,p)) return ""
    s=substr(tag,RSTART,RLENGTH); sub(/^[^=]*="/,"",s); sub(/"$/,"",s); return s
}
function count_attr(tag,key, n,t) {
    n=0; t=tag
    while (match(t,"[ \t]" key "=")) { n++; t=substr(t,RSTART+RLENGTH) }
    return n
}
{ line[++n]=$0; if (index($0,"\r")) die("CRLF configuration unsupported") }
END {
    if (bad) exit 2
    if (what=="xml") xml_edit()
    else if (what=="args") args_edit()
    else die("bad what")
}
function xml_edit(   i,op,cl,serial,ids,id,name,el,indent,uuid,entry,tag) {
    uuid="FAE5777963AA4424BF7166CE5951B0BE"
    op=0; cl=0
    for (i=1;i<=n;i++) {
        if (line[i] ~ /^[ \t]*<serviceConfiguration>[ \t]*$/) { if (op) die("repeated serviceConfiguration"); op=i }
        else if (line[i] ~ /^[ \t]*<\/serviceConfiguration>[ \t]*$/) { if (cl) die("repeated serviceConfiguration end"); cl=i }
        else if (index(line[i],"serviceConfiguration")) die("unexpected serviceConfiguration text")
        if (index(toupper(line[i]),uuid)) die("phone IMU UUID already present")
        if (index(line[i],"MX5DR")) die("MX5DR text already present")
    }
    if (!op || !cl || cl<=op+1) die("missing serviceConfiguration block")
    serial=0; indent=""
    for (i=op+1;i<cl;i++) {
        tag=line[i]
        if (tag ~ /^[ \t]*$/) continue
        if (tag !~ /^[ \t]*<[A-Za-z]+[ \t][^<>]*\/>[ \t]*$/) die("unsupported line in serviceConfiguration")
        el=tag; sub(/^[ \t]*</,"",el); sub(/[ \t].*$/,"",el)
        if (el !~ /^(callClient|audioSink|networkClient|pimClient|serialPort|messageClient|fileServer)$/) die("unknown service element " el)
        if (count_attr(tag,"id")!=1) die("service without a single id")
        id=attr(tag,"id")
        if (id !~ /^[0-9]+$/ || length(id)>3) die("non-numeric service id")
        if ((id+0) in ids) die("duplicate service id " id)
        ids[id+0]=1
        if (el=="serialPort") {
            serial++
            if (count_attr(tag,"name")!=1 || count_attr(tag,"uuidServer")!=1 || count_attr(tag,"uuidClient")!=1 ||
                count_attr(tag,"noOfInstances")!=1 || count_attr(tag,"writeDelay")!=1 || count_attr(tag,"enabled")!=1)
                die("serialPort without the stock attribute set")
            indent=tag; sub(/<.*$/,"",indent)
        }
    }
    if (15 in ids) die("service id 15 already used")
    if (!serial) die("no stock serialPort entry")
    if (serial>=20) die("serialPort limit reached")
    entry=indent "<serialPort id=\"15\" name=\"MX5DR IMU\" noOfInstances=\"1\" critical=\"false\" enabled=\"true\" uuidServer=\"" uuid "\" uuidClient=\"" uuid "\" writeDelay=\"3\"/>"
    for (i=1;i<=n;i++) { if (i==cl) print entry; print line[i] }
}
function args_edit(   i,found,tag,args,want,suffix,start,len,before,after) {
    if (action!="add" && action!="remove") die("bad action")
    if (xml=="" || xml ~ /[^A-Za-z0-9_.\/-]/ || substr(xml,1,1)!="/") die("unsupported configuration path")
    suffix=" -configFilePath " xml
    found=0
    for (i=1;i<=n;i++) {
        if (line[i] ~ /<!--/ && index(line[i],"name=\"bds\"")) die("bds service inside a comment line")
        if (!match(line[i],/<service[ \t][^>]*name="bds"[^>]*>/)) {
            if (index(line[i],"name=\"bds\"")) die("unsupported bds service declaration")
            continue
        }
        found++; where=i; start=RSTART; len=RLENGTH; tag=substr(line[i],RSTART,RLENGTH)
    }
    if (found!=1) die("expected exactly one bds service")
    if (attr(tag,"type")!="process" || attr(tag,"path")!="/jci/bds/bds" || count_attr(tag,"args")!=1) die("unexpected bds identity")
    args=attr(tag,"args")
    if (args=="-interface dbus" || args=="-interface dbus -wcp") {
        if (action=="add") want=args suffix
        else want=args
    } else if (args=="-interface dbus" suffix || args=="-interface dbus -wcp" suffix) {
        if (action=="add") die("bds configuration path already present")
        want=substr(args,1,length(args)-length(suffix))
    } else die("unexpected bds arguments")
    match(tag,/[ \t]args="[^"]*"/)
    tag=substr(tag,1,RSTART-1) " args=\"" want "\"" substr(tag,RSTART+RLENGTH)
    line[where]=substr(line[where],1,start-1) tag substr(line[where],start+len)
    for (i=1;i<=n;i++) print line[i]
}
