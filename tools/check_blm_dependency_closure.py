#!/usr/bin/env python3
"""Read-only ELF metadata check. Never loads or executes firmware code.
Uses host readelf; outputs authored JSON metadata, not OEM bytes.
"""
import argparse,collections,csv,gzip,hashlib,json,posixpath,re,struct,subprocess,zipfile,zlib
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--base',type=Path,required=True);p.add_argument('--target',type=Path,required=True);p.add_argument('--output',type=Path,default=Path('blm_dependency_closure.json'));a=p.parse_args()
root=a.base/'rootfs'; target='/jci/aapa/blmjciaapa.so'
def norm(s):return posixpath.normpath('/'+s.lstrip('/'))
links={norm(x['path']):x['target'] for x in json.loads((a.base/'rootfs-symlink-manifest.json').read_text())}
z=zipfile.ZipFile(a.base/'decrypted.zip');copies={}
for line in gzip.decompress(z.read('resources/files.ini.gz')).decode().splitlines():
    if '=' not in line:continue
    row=next(csv.reader([line.split('=',1)[1].strip()],skipinitialspace=True))
    if row and row[0]=='Copy':copies[norm(row[2])]={'member':row[1],'crc':int(row[4],0)}
materialized=[]
def resolve(path):
    path=norm(path)
    for _ in range(64):
        parts=path.strip('/').split('/');changed=False
        for n in range(1,len(parts)+1):
            prefix='/'+('/'.join(parts[:n]))
            if prefix in links:
                dest=links[prefix];base='/' if dest.startswith('/') else posixpath.dirname(prefix)
                path=norm(posixpath.join(base,dest,*parts[n:]));changed=True;break
        if not changed:return path
    raise ValueError('link cycle '+path)
def physical(path):
    path=resolve(path)
    if path==target:return a.target
    if path in copies:
        f=a.base/'overlay-for-dependency-check'/path.lstrip('/')
        if not f.exists():
            c=copies[path]; data=gzip.decompress(z.read(c['member']+'.gz'));assert zlib.crc32(data)==c['crc']
            f.parent.mkdir(parents=True,exist_ok=True);f.write_bytes(data);materialized.append(path)
        return f
    f=root/path.lstrip('/')
    return f if f.is_file() else None
search=['/jci/lib','/jci/opera/3rdpartylibs/freetype','/usr/lib/imx-mm/audio-codec','/usr/lib/imx-mm/parser','/data_persist/dev/lib','/lib','/usr/lib']
cache={}
def readelf(f,opt):return subprocess.check_output(['readelf',opt,'-W',str(f)],text=True,stderr=subprocess.DEVNULL)
def elf(path):
    if path in cache:return cache[path]
    f=physical(path);dy=readelf(f,'-d');ve=readelf(f,'-V');sy=readelf(f,'--dyn-syms');he=readelf(f,'-h')
    rec={'path':path,'sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'needed':re.findall(r'\(NEEDED\).*?\[(.*?)\]',dy),'rpath':re.findall(r'\((?:RUNPATH|RPATH)\).*?\[(.*?)\]',dy),'symbols':[],'version_needs':{},'version_defs':[],'elf_class':re.search(r'Class:\s+(\S+)',he).group(1),'machine':re.search(r'Machine:\s+([^\n]+)',he).group(1)}
    for line in sy.splitlines():
        m=re.match(r'\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)',line)
        if not m:continue
        value,size,typ,bind,vis,ndx,name=m.groups(); base,sep,ver=name.partition('@');default=ver.startswith('@');ver=ver.lstrip('@') or None
        rec['symbols'].append({'name':base,'version':ver,'default':default,'bind':bind,'vis':vis,'ndx':ndx,'type':typ})
    section=None;need=None
    for line in ve.splitlines():
        if line.startswith('Version needs section'):section='need'
        elif line.startswith('Version definition section'):section='def'
        elif line.startswith('Version symbols section'):section='sym'
        if section=='need':
            m=re.search(r'File: (\S+)',line)
            if m:need=m.group(1);rec['version_needs'][need]=[]
            m=re.search(r'Name: (\S+)',line)
            if m:rec['version_needs'][need].append(m.group(1))
        elif section=='def':
            m=re.search(r'Name: (\S+)',line)
            if m:rec['version_defs'].append(m.group(1))
    cache[path]=rec;return rec
def locate(name,owner):
    if '/' in name:
        paths=[norm(name)]
    else:
        rp=[]
        for entry in owner['rpath']:
            rp.extend(x.replace('${ORIGIN}',posixpath.dirname(owner['path'])).replace('$ORIGIN',posixpath.dirname(owner['path'])) for x in entry.split(':'))
        paths=[norm(posixpath.join(d,name)) for d in search+rp]
    candidates=[]
    for path in paths:
        if physical(path):
            actual=resolve(path)
            if actual not in candidates:candidates.append(actual)
    return candidates
queue=collections.deque([target]);seen=set();edges=[];missing=[]
while queue:
    path=queue.popleft()
    if path in seen:continue
    seen.add(path);obj=elf(path)
    for name in obj['needed']:
        candidates=locate(name,obj)
        edges.append({'from':path,'needed':name,'candidates':candidates})
        if candidates:queue.append(candidates[0])
        else:missing.append({'from':path,'needed':name})
exports=collections.defaultdict(list)
for path in seen:
    for s in elf(path)['symbols']:
        if s['ndx']!='UND' and s['bind'] in ['GLOBAL','WEAK','UNIQUE'] and s['vis'] in ['DEFAULT','PROTECTED']:exports[s['name']].append((path,s))
checks=[];weak=0
for path in sorted(seen):
    for s in elf(path)['symbols']:
        if s['ndx']!='UND':continue
        if s['bind']=='WEAK':weak+=1;continue
        if s['bind'] not in ['GLOBAL','UNIQUE']:continue
        matches=[]
        for provider,e in exports[s['name']]:
            if (s['version'] and s['version']==e['version']) or (not s['version'] and (not e['version'] or e['default'])):matches.append(provider)
        checks.append({'from':path,'symbol':s['name'],'version':s['version'],'providers':sorted(set(matches))})
version_checks=[]
for path in sorted(seen):
    obj=elf(path)
    for name,versions in obj['version_needs'].items():
        candidates=locate(name,obj);provider=candidates[0] if candidates else None
        for version in versions:version_checks.append({'from':path,'needed':name,'version':version,'provider':provider,'present':bool(provider and version in elf(provider)['version_defs'])})
summary={'objects':len(seen),'target_direct_needed':len(elf(target)['needed']),'dependency_edges':len(edges),'missing_libraries':len(missing),'strong_undefined_records':len(checks),'unmatched_strong_records':sum(not c['providers'] for c in checks),'target_strong_undefined_records':sum(c['from']==target for c in checks),'target_unmatched_strong_records':sum(c['from']==target and not c['providers'] for c in checks),'version_checks':len(version_checks),'version_failures':sum(not c['present'] for c in version_checks),'weak_undefined_records_excluded':weak,'resources_copy_entries':len(copies),'resources_files_materialized':materialized}
out={'summary':summary,'target':str(a.target),'target_sha256':elf(target)['sha256'],'search_order_assumption':search,'limitations':['Metadata-only candidate closure, not a dynamic loader simulation.','Existing/inherited LD_LIBRARY_PATH, ld.so.cache, preloads, main executable exports and previously loaded global objects are not modeled.','No dlopen constructors, relocation application, symbol interposition, hardware ABI or timing/runtime behavior was tested.','Any RPATH/RUNPATH is reported; nuanced glibc inherited RPATH precedence is not simulated.','Weak undefined symbols are not fatal and are excluded. Strong unresolved closure imports may come from main executable or other runtime global objects.'],'objects':[{k:v for k,v in elf(x).items() if k!='symbols'} for x in sorted(seen)],'edges':edges,'missing_libraries':missing,'strong_symbol_checks':checks,'version_checks':version_checks}
# Supplemental stock candidates are deliberately not added to the closure.
extra=elf('/jci/lib/libjcicommon_util.so')
extra_exports={s['name'] for s in extra['symbols'] if s['ndx']!='UND' and s['bind'] in ['GLOBAL','WEAK'] and s['vis'] in ['DEFAULT','PROTECTED'] and not s['version']}
remaining=[c for c in checks if not c['providers']]
out['outside_closure_candidate']={'path':extra['path'],'sha256':extra['sha256'],'matches':[c['symbol'] for c in remaining if c['symbol'] in extra_exports],'in_closure':extra['path'] in seen,'runtime_loaded': 'not established'}
out['target_matches_restored_rootfs']=hashlib.sha256((root/target.lstrip('/')).read_bytes()).hexdigest()==elf(target)['sha256']
out['duplicate_candidate_hashes']=[{'edge':e,'hashes':{c:hashlib.sha256(physical(c).read_bytes()).hexdigest() for c in e['candidates']}} for e in edges if len(e['candidates'])>1]
libc=physical('/lib/libc.so.6').read_bytes()
banner=re.search(rb'GNU C Library[^\x00\n]+',libc)
out['glibc_banner']=banner.group().decode() if banner else None
# Decode only the three ARM MOVNE-immediate / STRNE output stores in dbus_get_version.
dbus=physical('/usr/lib/libdbus-1.so.3');raw=dbus.read_bytes()
symbols=readelf(dbus,'--dyn-syms');m=re.search(r'\d+:\s+([0-9a-f]+)\s+(\d+)\s+FUNC\s+GLOBAL\s+DEFAULT\s+\S+\s+dbus_get_version(?:\s|$)',symbols)
va=int(m.group(1),16);size=int(m.group(2));phoff=struct.unpack_from('<I',raw,28)[0];phentsize,phnum=struct.unpack_from('<HH',raw,42)
for i in range(phnum):
    typ,offset,vaddr,paddr,filesz,memsz,flags,align=struct.unpack_from('<8I',raw,phoff+i*phentsize)
    if typ==1 and vaddr<=va and va+size<=vaddr+filesz:off=offset+va-vaddr;break
words=struct.unpack_from('<10I',raw,off);version=[]
for i in range(3):
    cmp,mov,store=words[i*3:i*3+3]
    assert cmp==0xe3500000+(i<<16)
    assert mov & 0xffffff00 == 0x13a03000 # MOVNE r3, unrotated imm8
    assert store==0x15803000+(i<<16)
    version.append(mov & 255)
assert words[9]==0xe12fff1e
out['dbus_get_version_static']={'path':'/usr/lib/libdbus-1.so.3.7.2','symbol_address':hex(va),'version':'.'.join(map(str,version)),'method':'ARM function conditionally stores immediate 1, 6, 4 through non-null output pointers; no execution'}
# Separate launcher-rooted candidate scope, never silently merged into BLM-only results.
launcher='/jci/sm/sm_svclauncher';lq=collections.deque([launcher]);lseen=set();ledges=[];lmissing=[]
while lq:
    path=lq.popleft()
    if path in lseen:continue
    lseen.add(path);obj=elf(path)
    for name in obj['needed']:
        candidates=locate(name,obj);ledges.append({'from':path,'needed':name,'candidates':candidates})
        if candidates:lq.append(candidates[0])
        else:lmissing.append({'from':path,'needed':name})
scope=seen|lseen;sexports=collections.defaultdict(list)
for path in scope:
    for sym in elf(path)['symbols']:
        if sym['ndx']!='UND' and sym['bind'] in ['GLOBAL','WEAK','UNIQUE'] and sym['vis'] in ['DEFAULT','PROTECTED']:sexports[sym['name']].append((path,sym))
schecks=[]
for path in sorted(scope):
    for sym in elf(path)['symbols']:
        if sym['ndx']!='UND' or sym['bind'] not in ['GLOBAL','UNIQUE']:continue
        matches=[provider for provider,e in sexports[sym['name']] if (sym['version'] and sym['version']==e['version']) or (not sym['version'] and (not e['version'] or e['default']))]
        schecks.append({'from':path,'symbol':sym['name'],'version':sym['version'],'providers':sorted(set(matches))})
lversions=[]
for path in sorted(lseen):
    for name,versions in elf(path)['version_needs'].items():
        candidates=locate(name,elf(path));provider=candidates[0] if candidates else None
        for version in versions:lversions.append({'from':path,'needed':name,'version':version,'provider':provider,'present':bool(provider and version in elf(provider)['version_defs'])})
out['launcher_candidate_scope']={'launcher':launcher,'launcher_direct_needed':elf(launcher)['needed'],'launcher_closure_objects':len(lseen),'launcher_closure_paths':sorted(lseen),'launcher_dependency_edges':ledges,'launcher_missing_libraries':lmissing,'launcher_version_checks':lversions,'combined_scope_objects':len(scope),'combined_strong_import_records':len(schecks),'combined_unmatched_strong_records':[c for c in schecks if not c['providers']],'previously_unmatched_blm_closure_records':[c for c in schecks if any(c['from']==old['from'] and c['symbol']==old['symbol'] for old in remaining)],'runtime_scope_status':'Static candidate main-executable dependency scope, not observed loaded objects, dlopen flags or preload/interposition test'}
a.output.write_text(json.dumps(out,indent=2));print(json.dumps(summary,indent=2));print('Launcher combined unmatched:',len(out['launcher_candidate_scope']['combined_unmatched_strong_records']))
