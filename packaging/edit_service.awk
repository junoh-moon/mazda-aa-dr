# Narrow, fail-closed editor; not a general XML parser. POSIX awk.
# Only the explicitly selected service and its single LD_PRELOAD value change.
function die(s) { print "edit_service: " s > "/dev/stderr"; bad=1; exit 2 }
function spaces(n, s) { s=""; while(n-- > 0) s=s " "; return s }
function attr(tag,key, p,s) {
    p="[[:space:]]" key "[[:space:]]*=[[:space:]]*\"[^\"]*\""
    if (!match(tag,p)) return ""
    s=substr(tag,RSTART,RLENGTH); sub(/^[^=]*=[[:space:]]*"/,"",s); sub(/"$/,"",s); return s
}
function edit(block, mask,rest,pos,start,len,tag,val,n,a,i,newval,out,count,where,sz,before,after,owned) {
    mask=block
    pos=1
    while ((i=index(substr(mask,pos),"<!--"))>0) {
        start=pos+i-1; len=index(substr(mask,start+4),"-->")
        if (!len) die("unterminated comment")
        len=len+6; mask=substr(mask,1,start-1) spaces(len) substr(mask,start+len); pos=start+len
    }
    rest=mask; pos=1; count=0
    while (match(rest,/<environ_var[[:space:]][^>]*>/)) {
        start=pos+RSTART-1; len=RLENGTH; tag=substr(block,start,len)
        if (attr(tag,"env_name")=="LD_PRELOAD") {
            count++; where=start; sz=len
            if (tag !~ /\/[[:space:]]*>$/) die("LD_PRELOAD must be self-closing")
            val=attr(tag,"env_value")
            if (val ~ /[^A-Za-z0-9_./:+ \t-]/) die("unsupported preload syntax")
        }
        pos=start+len; rest=substr(mask,pos)
    }
    if(count>1) die("multiple LD_PRELOAD entries")
    # Reject alternate quoting or an unparsed preload declaration, not silently add another.
    if(!count && index(mask,"LD_PRELOAD")) die("unsupported preload declaration")
    if(count) {
        n=split(val,a,/[: \t]+/); newval=""; owned=0
        for(i=1;i<=n;i++) {
            if(a[i]==token) owned++
            else if(a[i]!="") newval=newval (newval==""?"":":") a[i]
        }
        if(action=="remove" && !owned) return block
        if(action=="add") newval=token (newval==""?"":":" newval)
        tag=substr(block,where,sz)
        if(newval!="") {
            match(tag,/[[:space:]]env_value[[:space:]]*=[[:space:]]*"[^"]*"/)
            if(!RSTART) die("missing env_value")
            before=substr(tag,1,RSTART-1); after=substr(tag,RSTART+RLENGTH)
            tag=before " env_value=\"" newval "\"" after
            return substr(block,1,where-1) tag substr(block,where+sz)
        }
        before=substr(block,1,where-1); after=substr(block,where+sz)
        # Remove the empty, wholly owned line if it has no other content.
        if(before ~ /\n[ \t]*$/ && after ~ /^[ \t]*\n/) { sub(/[ \t]*$/,"",before); sub(/^[ \t]*\n/,"",after) }
        return before after
    }
    if(action=="remove") return block
    sub(/<\/service>/,"            <environ_var env_name=\"LD_PRELOAD\" env_value=\"" token "\"/>\n        </service>",block)
    # Closing indentation is already present; avoid doubling it for the inserted line.
    sub(/        +            <environ_var/,"            <environ_var",block)
    return block
}
{ all=all $0 "\n" }
END {
    if(bad) exit 2
    if(action!="add" && action!="remove") die("bad action")
    if(target_service=="") target_service="jciAAPA"
    if(target_service=="jciAAPA") target_path="/jci/aapa/blmjciaapa.so"
    else if(target_service=="jciVBS") target_path="/jci/vbs/svcjcivbs.so"
    else die("unsupported target service")
    if(token=="" || token ~ /[^A-Za-z0-9_./+-]/) die("unsupported token")
    if(index(all,"\r")) die("CRLF configuration unsupported")
    mask=all; pos=1
    while ((i=index(substr(mask,pos),"<!--"))>0) {
        start=pos+i-1; len=index(substr(mask,start+4),"-->")
        if(!len) die("unterminated comment")
        len+=6; mask=substr(mask,1,start-1) spaces(len) substr(mask,start+len); pos=start+len
    }
    rest=mask; pos=1; targets=0
    while(match(rest,/<service[[:space:]][^>]*>/)) {
        start=pos+RSTART-1; len=RLENGTH; tag=substr(all,start,len)
        if(attr(tag,"name")==target_service) {
            targets++; first=start
            if(attr(tag,"path")!=target_path || attr(tag,"type")!="jci_service") die("unexpected " target_service " identity")
            end=index(substr(mask,start+len),"</service>")
            if(!end || tag ~ /\/[[:space:]]*>$/) die("missing service closure")
            total=len+end-1+10; target=substr(all,start,total)
        }
        pos=start+len; rest=substr(mask,pos)
    }
    if(targets!=1) die("expected exactly one " target_service " service")
    result=edit(target)
    printf "%s",substr(all,1,first-1) result substr(all,first+total)
}
