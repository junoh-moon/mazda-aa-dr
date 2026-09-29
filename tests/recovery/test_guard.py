#!/usr/bin/env python3
"""Host gate/editor tests. No target binaries or OEM dumps are shipped/executed."""
import os, pathlib, subprocess, tempfile, unittest, shutil, shlex, resource, hashlib
HERE=pathlib.Path(__file__).resolve().parents[2]
BASE='data_persist/mx5-aa-dr'
STOCK='''#!/bin/sh
SMCFG_NORMALMODE='/jci/sm/sm.conf'
SMCFG_WCPMODE='/jci/sm/sm_WCP.conf'
case "$BOARD" in
2) taskset 0x02 /jci/sm/sm -f $SMCFG_WCPMODE -e /tmp/smevents.txt &
;;
*) taskset 0x02 /jci/sm/sm -f $SMCFG_NORMALMODE -e /tmp/smevents.txt &
;;
esac
'''.replace('2) taskset','2)\n taskset').replace('*) taskset','*)\n taskset')
TOKEN='/data_persist/mx5-aa-dr/libmx5dr.so'
TAP_TOKEN='/data_persist/mx5-aa-dr/libmx5dr-vimtap.so'
CFG='''<sm_config><services><service type="jci_service" name="jciAAPA" path="/jci/aapa/blmjciaapa.so" args="new_hw"><environ_var env_name="LD_PRELOAD" env_value="/data_persist/touch.so"/></service><service type="jci_service" name="jciVBS" path="/jci/vbs/svcjcivbs.so" args=""><environ_var env_name="LD_PRELOAD" env_value="/data_persist/vbs.so"/></service></services></sm_config>\n'''
class Gate(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.build=tempfile.TemporaryDirectory();cls.exe=pathlib.Path(cls.build.name)/'guard'
  subprocess.run(shlex.split(os.environ.get('GUARD_CXX','g++')) + shlex.split(os.environ.get('GUARD_ARCH_FLAGS','')) + ['-std=c++11','-Wall','-Wextra','-Werror','-DMX5DR_GUARD_TESTING',str(HERE/'src/guard/guard.cpp'),str(HERE/'src/runtime/sha256.cpp'),'-o',str(cls.exe)],check=True)
  cls.command=shlex.split(os.environ.get('GUARD_RUNNER',''))+[str(cls.exe)]
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.root=pathlib.Path(self.tmp.name);self.env=dict(os.environ,MX5DR_GUARD_ROOT=str(self.root));(self.root/'.mx5dr-fixture').touch()
  for p in [BASE+'/guard','jci/sm','proc/sys/kernel/random','tmp']:(self.root/p).mkdir(parents=True,exist_ok=True)
  self.put(BASE+'/libmx5dr.so',b'author-fixture-payload');self.put(BASE+'/libmx5dr-vimtap.so',b'author-fixture-tap');self.put(BASE+'/mx5dr.conf',b'mode=OBSERVE\n')
  for p in ['jci/sm/sm.conf','jci/sm/sm_WCP.conf']:self.put(p,CFG.encode())
  trial=CFG.replace('/data_persist/touch.so','/data_persist/mx5-aa-dr/libmx5dr.so:/data_persist/touch.so').encode()
  for p in ['normal.trial','wcp.trial']:self.put(BASE+'/guard/'+p,trial)
  for p in ['normal.source.sha256','wcp.source.sha256']:self.put(BASE+'/guard/'+p,(hashlib.sha256(CFG.encode()).hexdigest()+'\n').encode())
  self.put('proc/sys/kernel/random/boot_id',b'01234567-1234-1234-1234-0123456789ab\n')
 def tearDown(self):self.tmp.cleanup()
 def put(self,p,b):x=self.root/p;x.write_bytes(b);x.chmod(0o600)
 def call(self,*a,env=None):return subprocess.run(self.command+list(a),env=env or self.env,text=True,capture_output=True)
 def arm(self):self.assertEqual(self.call('arm').returncode,0)
 def test_check_does_not_arm_and_rejects_missing_boot_id(self):
  self.assertEqual(self.call('check').returncode,0)
  self.assertFalse((self.root/BASE/'guard/arm').exists())
  (self.root/'proc/sys/kernel/random/boot_id').unlink()
  self.assertNotEqual(self.call('check').returncode,0)
  self.assertFalse((self.root/BASE/'guard/arm').exists())
 def test_one_boot_and_new_boot_baseline(self):
  self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0);self.arm();r=self.call('select','/jci/sm/sm.conf');self.assertEqual(r.returncode,0,r.stderr)
  self.assertIn('/mx5dr-trial-',r.stdout);self.assertIn('touch.so',pathlib.Path(r.stdout.strip()).read_text());self.assertFalse((self.root/BASE/'guard/arm').exists())
  self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
  self.arm();self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
  (self.root/BASE/'guard/arm').unlink();self.put('proc/sys/kernel/random/boot_id',b'11234567-1234-1234-1234-0123456789ab\n');self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_stock_absolute_and_relative_persist_alias(self):
  (self.root/'mnt').mkdir();(self.root/'data_persist').rename(self.root/'mnt/data_persist')
  alias=self.root/'data_persist'
  for target in ['/mnt/data_persist','mnt/data_persist']:
   with self.subTest(target=target):
    alias.symlink_to(target)
    self.arm();r=self.call('select','/jci/sm/sm.conf');self.assertEqual(r.returncode,0,r.stderr)
    self.assertIn(TOKEN,pathlib.Path(r.stdout.strip()).read_text())
    self.assertFalse((self.root/'mnt/data_persist/mx5-aa-dr/guard/arm').exists())
    self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
    (self.root/'mnt/data_persist/mx5-aa-dr/guard/last-boot').unlink();alias.unlink()
 def test_full_stock_alias_chain_and_group_writable_oem_config(self):
  (self.root/'tmp/mnt').mkdir();(self.root/'data_persist').rename(self.root/'tmp/mnt/data_persist')
  (self.root/'data_persist').symlink_to('/mnt/data_persist');(self.root/'mnt').symlink_to('/tmp/mnt')
  for path in ['jci','jci/sm','jci/sm/sm.conf','jci/sm/sm_WCP.conf']:(self.root/path).chmod(0o775)
  self.arm();r=self.call('select','/jci/sm/sm.conf');self.assertEqual(r.returncode,0,r.stderr)
  self.assertIn(TOKEN,pathlib.Path(r.stdout.strip()).read_text())
  self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_persist_alias_does_not_allow_payload_or_target_symlink(self):
  (self.root/'mnt').mkdir();(self.root/'data_persist').rename(self.root/'mnt/data_persist')
  alias=self.root/'data_persist';alias.symlink_to('/mnt/data_persist');self.arm()
  p=self.root/'mnt/data_persist/mx5-aa-dr/libmx5dr.so';p.rename(p.with_suffix('.save'));p.symlink_to(p.with_suffix('.save'))
  self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
  alias.unlink();alias.symlink_to('/tmp/untrusted')
  self.assertNotEqual(self.call('arm').returncode,0)
 def test_wcp_and_input_binding(self):
  self.arm();self.put('jci/sm/sm.conf',(CFG+'<!-- later touch edit -->').encode());self.assertNotEqual(self.call('select','/jci/sm/sm_WCP.conf').returncode,0)
  self.assertNotEqual(self.call('arm').returncode,0)
  self.put('jci/sm/sm.conf',CFG.encode())
  self.arm();r=self.call('select','/jci/sm/sm_WCP.conf');self.assertEqual(r.returncode,0);self.assertIn('args="new_hw"',pathlib.Path(r.stdout.strip()).read_text())
 def test_regenerated_template_binds_new_touch_snapshot(self):
  updated=CFG.replace('touch.so','touch-v2.so')
  self.put('jci/sm/sm.conf',updated.encode())
  self.assertNotEqual(self.call('check').returncode,0)
  self.assertNotEqual(self.call('arm').returncode,0)
  self.put(BASE+'/guard/normal.trial',updated.replace('/data_persist/touch-v2.so',TOKEN+':/data_persist/touch-v2.so').encode())
  self.put(BASE+'/guard/normal.source.sha256',(hashlib.sha256(updated.encode()).hexdigest()+'\n').encode())
  self.arm();r=self.call('select','/jci/sm/sm.conf');self.assertEqual(r.returncode,0,r.stderr)
  self.assertIn('touch-v2.so',pathlib.Path(r.stdout.strip()).read_text())
 def test_source_identity_missing_symlink_or_malformed_declines(self):
  p=self.root/BASE/'guard/normal.source.sha256';saved=p.read_bytes()
  p.unlink();self.assertNotEqual(self.call('arm').returncode,0)
  p.symlink_to(self.root/BASE/'guard/wcp.source.sha256');self.assertNotEqual(self.call('arm').returncode,0)
  p.unlink()
  for value in [saved.rstrip(b'\n'),saved+b'\n',saved.upper()]:
   self.put(BASE+'/guard/normal.source.sha256',value);self.assertNotEqual(self.call('arm').returncode,0)
 def test_payload_change_declines(self):
  self.arm();self.put(BASE+'/libmx5dr.so',b'corruption');self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_tap_change_declines(self):
  self.arm();self.put(BASE+'/libmx5dr-vimtap.so',b'changed tap');self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_missing_symlink_or_unsafe_tap_declines(self):
  p=self.root/BASE/'libmx5dr-vimtap.so';saved=p.with_suffix('.saved')
  self.arm();p.rename(saved);self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
  p.symlink_to(saved);self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
  p.unlink();saved.rename(p);p.chmod(0o666);self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_old_manifest_does_not_authorize_new_guard(self):
  self.arm();p=self.root/BASE/'guard/arm';lines=p.read_text().splitlines();self.assertEqual(lines[0],'mx5dr-one-boot-v2')
  self.put(BASE+'/guard/arm',('\n'.join(['mx5dr-one-boot-v1']+lines[1:-1])+'\n').encode());self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_shadow_two_services_share_consumed_arm(self):
  self.put(BASE+'/mx5dr.conf',b'mode=SHADOW\n')
  for name in ['normal.trial','wcp.trial']:
   p=self.root/BASE/'guard'/name;self.put(BASE+'/guard/'+name,p.read_bytes().replace(b'/data_persist/vbs.so',(TAP_TOKEN+':/data_persist/vbs.so').encode()))
  self.arm();r=self.call('select','/jci/sm/sm_WCP.conf');self.assertEqual(r.returncode,0,r.stderr)
  trial=pathlib.Path(r.stdout.strip()).read_text();self.assertIn(TOKEN+':/data_persist/touch.so',trial);self.assertIn(TAP_TOKEN+':/data_persist/vbs.so',trial)
  self.assertFalse((self.root/BASE/'guard/arm').exists());self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
  self.put('proc/sys/kernel/random/boot_id',b'11234567-1234-1234-1234-0123456789ab\n');self.assertNotEqual(self.call('select','/jci/sm/sm_WCP.conf').returncode,0)
 def test_persistent_vbs_tap_prevents_arm_and_select(self):
  self.arm();self.put('jci/sm/sm_WCP.conf',CFG.replace('/data_persist/vbs.so',TAP_TOKEN+':/data_persist/vbs.so').encode())
  self.assertNotEqual(self.call('arm').returncode,0);self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_symlink_and_permissions_decline(self):
  self.arm();p=self.root/BASE/'guard/normal.trial';p.rename(p.with_suffix('.save'));p.symlink_to(p.with_suffix('.save'));self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
  p.unlink();p.with_suffix('.save').rename(p);p.chmod(0o666);self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_durability_failure_never_publishes(self):
  for stage in ['trial-dir', 'consume-dir', 'last-boot']:
   with self.subTest(stage=stage):
    (self.root/BASE/'guard/last-boot').unlink(missing_ok=True)
    self.arm();r=self.call('select','/jci/sm/sm.conf',env=dict(self.env,MX5DR_GUARD_FAIL_FSYNC=stage));self.assertNotEqual(r.returncode,0);self.assertEqual(r.stdout,'')
    if stage != 'trial-dir':
     self.assertFalse((self.root/BASE/'guard/arm').exists())
     self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_failed_arm_publish_revokes_authorization(self):
  r=self.call('arm',env=dict(self.env,MX5DR_GUARD_FAIL_FSYNC='arm'))
  self.assertNotEqual(r.returncode,0)
  self.assertFalse((self.root/BASE/'guard/arm').exists())
  self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_failed_arm_rollback_durability_is_explicit(self):
  r=self.call('arm',env=dict(self.env,MX5DR_GUARD_FAIL_FSYNC='arm,arm-cancel'))
  self.assertEqual(r.returncode,3)
  self.assertIn('unable to confirm durable disarm',r.stderr)
  self.assertFalse((self.root/BASE/'guard/arm').exists())
  self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_concurrent_selection_at_most_one(self):
  self.arm();a=[subprocess.Popen(self.command+['select','/jci/sm/sm.conf'],env=self.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE) for _ in range(8)]
  for p in a:p.communicate()
  self.assertEqual(sum(p.returncode==0 for p in a),1)
 def test_missing_template_and_persistent_token_decline(self):
  self.arm();(self.root/BASE/'guard/wcp.trial').unlink();self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
 def test_consumed_arm_before_constructor_and_late_abort(self):
  # Only authored host ELF runs. This does not model target SM retry policy.
  src=self.root/'failure.c';dso=self.root/'failure.so';exe=self.root/'launcher'
  src.write_text('#include <stdlib.h>\n#include <unistd.h>\n__attribute__((constructor)) static void init(void){if(getenv("EARLY_FAILURE"))abort();}\n')
  subprocess.run(['cc','-shared','-fPIC',str(src),'-o',str(dso)],check=True)
  src.write_text('#include <stdlib.h>\n#include <unistd.h>\nint main(void){usleep(10000);abort();}\n')
  subprocess.run(['cc',str(src),'-o',str(exe)],check=True)
  for early in [True,False]:
   (self.root/BASE/'guard/last-boot').unlink(missing_ok=True)
   self.arm();self.assertEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
   env=dict(os.environ,LD_PRELOAD=str(dso))
   if early:env['EARLY_FAILURE']='1'
   else:env.pop('EARLY_FAILURE',None)
   r=subprocess.run([str(exe)],env=env,preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
   self.assertEqual(r.returncode,-6)
   self.put('proc/sys/kernel/random/boot_id',b'21234567-1234-1234-1234-0123456789ab\n')
   self.assertNotEqual(self.call('select','/jci/sm/sm.conf').returncode,0)
class ServiceEditor(unittest.TestCase):
 def edit(self,s,action,service=None,token=None):
  args=['awk','-v','action='+action,'-v','token='+(token or TOKEN)]
  if service is not None:args+=['-v','target_service='+service]
  return subprocess.run(args+['-f',str(HERE/'packaging/edit_service.awk')],input=s,text=True,capture_output=True)
 def test_explicit_vbs_and_default_aapa_are_independent(self):
  aa=self.edit(CFG,'add');self.assertEqual(aa.returncode,0,aa.stderr)
  both=self.edit(aa.stdout,'add','jciVBS',TAP_TOKEN);self.assertEqual(both.returncode,0,both.stderr)
  self.assertIn(TOKEN+':/data_persist/touch.so',both.stdout);self.assertIn(TAP_TOKEN+':/data_persist/vbs.so',both.stdout)
  self.assertEqual(self.edit(both.stdout,'add','jciVBS',TAP_TOKEN).stdout,both.stdout)
  self.assertEqual(self.edit(both.stdout,'remove','jciVBS',TAP_TOKEN).stdout,aa.stdout)
  self.assertEqual(self.edit(self.edit(both.stdout,'remove').stdout,'remove','jciVBS',TAP_TOKEN).stdout,CFG)
 def test_pinned_vbs_identity_and_ambiguity(self):
  for source in [CFG.replace('/jci/vbs/svcjcivbs.so','/jci/vbs/other.so'),CFG.replace('name="jciVBS"','name="missing"'),CFG+CFG]:
   self.assertNotEqual(self.edit(source,'add','jciVBS',TAP_TOKEN).returncode,0)
  self.assertNotEqual(self.edit(CFG,'add','other',TAP_TOKEN).returncode,0)
 def test_remove_only_exact_vbs_token(self):
  other=TAP_TOKEN+'.backup:/data_persist/vbs.so'
  source=CFG.replace('/data_persist/vbs.so',TAP_TOKEN+':'+other)
  result=self.edit(source,'remove','jciVBS',TAP_TOKEN);self.assertEqual(result.returncode,0,result.stderr)
  self.assertEqual(result.stdout,CFG.replace('/data_persist/vbs.so',other))
class Editor(unittest.TestCase):
 def edit(self,s,action):return subprocess.run(['awk','-v','action='+action,'-f',str(HERE/'packaging/edit_autostart.awk')],input=s,text=True,capture_output=True)
 def test_roundtrip_and_idempotent(self):
  a=self.edit(STOCK,'add');self.assertEqual(a.returncode,0,a.stderr);self.assertEqual(a.stdout.count('BEGIN'),2);self.assertEqual(self.edit(a.stdout,'add').stdout,a.stdout);self.assertEqual(self.edit(a.stdout,'remove').stdout,STOCK)
  subprocess.run(['sh','-n'],input=a.stdout,text=True,check=True)
 def test_ambiguous_or_wrong_anchor_refused(self):
  for s in [self.edit(STOCK,'add').stdout.replace('END SMCFG_WCPMODE','END SMCFG_NORMALMODE'),STOCK+STOCK,STOCK.replace('taskset 0x02','taskset 0x01'),STOCK.replace('\n','\r\n')]:self.assertNotEqual(self.edit(s,'add').returncode,0)
 def test_later_unrelated_edit_preserved(self):
  a=self.edit(STOCK,'add').stdout+'# later user edit\n';self.assertEqual(self.edit(a,'remove').stdout,STOCK+'# later user edit\n')
 def test_branch_execution_and_fallback(self):
  with tempfile.TemporaryDirectory() as d:
   root=pathlib.Path(d);helper=root/'guard';calls=root/'calls';launches=root/'launches'
   edited=self.edit(STOCK,'add').stdout.replace('/data_persist/mx5-aa-dr/guard/mx5dr-guard',str(helper)).replace('/data_persist/mx5-aa-dr/tools/start_collector.sh',str(root/'no-collector'))
   # Authored taskset stand-in records config; never executes SM.
   script='taskset() { printf "%s\\n" "$4" >> "$LAUNCHES"; }\n'+edited+'\nwait\n'
   for board,base in [('2','/jci/sm/sm_WCP.conf'),('0','/jci/sm/sm.conf')]:
    for state in ['missing','failed','success','malformed']:
     with self.subTest(board=board,state=state):
      helper.unlink(missing_ok=True);calls.unlink(missing_ok=True);launches.unlink(missing_ok=True)
      if state!='missing':
       output='/tmp/mx5dr-trial-ABC123/sm.conf' if state=='success' else 'garbage'
       helper.write_text('#!/bin/sh\nprintf "%s\\n" "$2" >> "$CALLS"\nprintf "%s\\n" "'+output+'"\nexit '+('2' if state=='failed' else '0')+'\n');helper.chmod(0o700)
      r=subprocess.run(['sh'],input=script,text=True,capture_output=True,env=dict(os.environ,BOARD=board,CALLS=str(calls),LAUNCHES=str(launches)))
      self.assertEqual(r.returncode,0,r.stderr)
      self.assertEqual(launches.read_text().splitlines(),[('/tmp/mx5dr-trial-ABC123/sm.conf' if state=='success' else base)])
      self.assertEqual(calls.read_text().splitlines() if calls.exists() else [],[] if state=='missing' else [base])
if __name__=='__main__':unittest.main()
