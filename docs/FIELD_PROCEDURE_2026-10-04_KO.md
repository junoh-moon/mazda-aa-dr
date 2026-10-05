# 차량 시험 한 번의 절차 — 2026-10-04

**이 문서는 절차입니다. 차량 방문이나 주행을 요청하는 것이 아닙니다.** 사용자가 일정을 정하면 이 순서를 그대로 따르십시오.
화면 인용은 모두 이 묶음의 설치 파일을 **순정 BusyBox 1.19.2 환경에서 실제로 실행한 출력**입니다
([검증 기록](../validation/STOCK_BUSYBOX_MENU_2026-10-04.md)). 단, 그 실행은 에뮬레이션이라 **실제 CMU에서 나오는 값(번호, 시간)은 다를 수 있습니다.**
아래 표에서 "그대로"라고 한 문자열만 판정에 쓰고, 번호·경로·저장 용량 숫자는 판정에 쓰지 않습니다.

## 이 시험이 하는 일

**대상 릴리즈는 `v0.3.12-shadow.5`입니다.** 기록만 합니다. 순정 위치에 아무것도 주입하지 않고 ASSIST는 꺼져 있습니다. 이미 확보한 이동 센서 기록(휠·yaw 약 13,900건, 최고 60 km/h)은 더 필요 없고,
**한 번의 주행으로 다음 세 가지를 얻는 것**이 목적입니다.

1. **재부팅이 재발하지 않는지.** 지난 시험은 부팅 약 24분에 CMU가 스스로 재부팅됐고, 그 원인의 유력 후보(collector의 SMDB 접근)를 `shadow.4`에서 제거했습니다. 재부팅 완료부터 회수까지 **30분 이상** 버티는지가 검증입니다.
2. **GPS 단절 구간의 센서 기록.** 지난번에는 진입 직전에 재부팅돼서 약 5초뿐이었습니다. 지하주차장을 들어갔다 나오는 구간이 DR 정확도를 재는 핵심입니다.
3. **AA 위치 관찰.** `shadow.5`부터 AA 위치 후킹이 사용자의 `libpatch`와 함께 설치됩니다. Mazda가 AA에 무엇을 보내는지(\`position\` 행)를 처음으로 기록합니다.

운전 중에는 아무것도 조작하지 않습니다.

## 하지 말 것

- **시동 OFF/ON으로 CMU 재부팅을 대신하지 마십시오.** 재부팅은 메뉴 `5`로만 합니다.
- 설치 후 다시 `1`을 누르지 마십시오(회수 때도).
- USB와 AA 동글을 동시에 연결하지 마십시오(포트가 하나입니다).
- 시험 중 시동을 끄지 마십시오. 끄면 그 부팅의 기록이 끝납니다.
- 운전 중 USB를 바꾸거나 명령을 입력하지 마십시오.

## 0. 집에서 준비

1. GitHub 릴리즈의 설치 ZIP과 `.sha256`을 받아 SHA-256을 확인하십시오. 릴리즈 노트에 적힌 값과 같아야 합니다. 다르면 그 ZIP을 쓰지 말고 알려 주십시오.
2. ZIP을 풀어 **내용물 전체**를 깨끗한 FAT32 USB 최상위에 복사하십시오. USB를 열었을 때 `trial`, `install.sh`, `mp3`, `js`가 바로 보여야 합니다. `mp3`와 `js`는 셸 진입에 쓰는 파일이며 이전 릴리즈(v0.3.9)와 같은 방식입니다.
3. 이 USB 하나로 설치·확인·회수를 모두 합니다.

## 1. 주차 상태에서 설치와 재부팅

조건: 환기되는 장소, 주차브레이크, 6MT 중립, **엔진이 실제로 가동 중**(타코미터가 0보다 높음). 이 절이 끝날 때까지 엔진과 USB를 유지합니다.

**S1.** USB를 꽂고 **이전 릴리즈에서 쓰신 방식 그대로** 셸을 여십시오. Entertainment → USB에서 포함된 곡을 재생하고 약 15초 기다리면 키보드 입력이 되는 셸이 열립니다. 다른 USB가 먼저 연결되어 있으면 다음 곡도 재생하십시오. 이 단계는 제가 에뮬레이션하지 못했고, 이전 릴리즈에서 사용자가 검증한 방식에 의존합니다.

**S2.** 다음 한 줄을 입력하고 Enter를 누르십시오(Shift가 필요한 글자가 없습니다).

```
sh /tmp/mnt/sda1/trial
```

- 화면에 `Parked USB trial menu`와 번호 목록, 마지막 줄 `Number, then Enter:`가 나오면 정상입니다.
- `can't open '/tmp/mnt/sda1/trial'`이 나오면 USB 이름이 다른 것입니다. `sda1`을 `sdb1`로, 안 되면 `sdc1`로 바꿔 다시 입력하십시오.
- `Run trial from the mounted USB root; keep that USB connected.`가 나오면 USB 연결을 확인하고 이 줄을 다시 입력하십시오.

**S3.** `1`과 Enter를 누르십시오. 끝날 때까지 기다리고 USB를 빼지 마십시오. 아래 줄들이 **그대로** 나와야 합니다(맨 위 숫자 줄은 달라도 됩니다).

```
storage_available_kib=… required_kib=… remaining_log_kib=… reserve_kib=…
Staged SHADOW for one guarded boot. Persistent service configs retain existing touch only. No processes restarted.
Install steps finished. A vehicle ignition cycle alone does not prove a new CMU Linux boot.
…
Installation finished. Choose 5 to request a CMU reboot; ignition off/on is not a reboot check.
```

그리고 메뉴가 다시 나옵니다. **`Installation finished`가 없거나 오류 줄이 나오면 `5`를 누르지 말고** 화면을 사진으로 남긴 뒤 알려 주십시오.
`Persistent … preload found; run uninstall.sh first` 같은 오류가 나오면 이전 설치가 남아 있는 것입니다(이 문구는 설치기 소스에서 읽은 것이며 에뮬레이션에서 실행해 보지 않았습니다). 사진을 남기고 **`4`와 Enter로 제거한 뒤 `1`을 한 번만** 다시 하십시오.
이전 버전(v0.3.9)이 설치되어 있어도 같은 결과가 나오는 것을 확인했습니다(v0.3.9-shadow.2·shadow.3 위 덮어쓰기, 설치 직후 상태와 시험 부팅 이후 상태 모두).

**S4.** 같은 메뉴에서 `5`와 Enter를 누르십시오. 다음 네 줄이 **그대로** 나옵니다.

```
reboot_receipt=reboot-request.txt
reboot_request=normal
reboot_completion=unconfirmed
reboot_command_exit=0
```

`reboot_completion=unconfirmed`는 **정상입니다.** 요청만 보냈다는 뜻입니다. 이제 **시동 버튼을 누르지 말고** CMU 화면이 꺼졌다가 다시 켜질 때까지 기다리십시오.
`reboot_command_exit=0`이 아니면 사진을 남기고 `5`를 한 번만 다시 누르십시오. 또 실패하면 중단하고 알려 주십시오.

**S5.** 화면이 돌아오면 같은 방식으로 셸을 열고 S2의 한 줄을 입력한 뒤 **`2`와 Enter**를 누르십시오. 재부팅이 됐는지도, 시험이 시작됐는지도 **화면 맨 아래의 판정 블록 하나로만** 판단합니다.
상세 값은 USB의 `startup-result.txt`에 전부 저장되므로 화면에서 긴 줄을 읽을 필요가 없습니다.

```
---- GO / NO-GO ----
ok   BOOT  new boot
ok   GUARD committed
ok   ONCE  consumed
ok   MODE  SHADOW
ok   STOP  not disabled
ok   DATA  1275 bytes
ok   POLL  collector polling
ok   AAPA  preload yes
ok   LDS   preload yes
ok   VBS   preload yes
GO
```

위는 순정 BusyBox 실행의 실제 메뉴 2 출력(합성 `/proc`)에 판정 함수를 적용한 결과입니다. 이 시험에서 VBS 줄만 제가 `preload yes`로 바꿔 넣었습니다. 숫자(`1275`)는 차에서는 다릅니다.
각 줄은 40자 이내이고 `ok`, `NO`, `wait` 중 하나로 시작합니다. 메뉴 2가 끝나면 긴 메뉴 안내를 다시 찍지 않고 짧은 한 줄(`0 Exit (1-5 as listed before)`)만 나오므로 판정 블록이 화면 아래에 남습니다.

**S6. 마지막 줄로 판정합니다.**

- `GO` → 출발합니다. S7로 가십시오.
- `WAIT 60 s, then run 2 again` → `wait`인 줄이 아직 시작 중인 항목입니다(서비스가 `not running`, `DATA 0 bytes`, `POLL`이 아직 없음, 새 부팅 직후의 `GUARD not consumed yet`). **60초 기다린 뒤 `2`를 한 번 더** 누르십시오. 두 번째에도 `WAIT`나 `NO-GO`면 출발하지 마십시오(아래 NO-GO와 같음).
- `NO-GO` → **출발하지 마십시오.** `NO`인 줄과 그 값을 사진으로 남기고 S9의 `3`과 `4`를 하십시오. 다시 `1`을 누르지 마십시오. 단 하나의 예외가 있습니다.
  - `NO   BOOT  same_boot` → 재부팅이 되지 않은 것입니다. `5`를 한 번 더 누르고 5분 기다린 뒤 `2`로 다시 확인하십시오(5분은 이 절차가 정한 기준입니다). 그래도 `same_boot`면 위의 NO-GO와 같이 중단하십시오.
  - `NO   BOOT  unavailable`이면 요청 기록을 읽지 못한 것이니 중단하고 알려 주십시오.

판정 블록의 줄과 기준은 아래와 같습니다. `AAPA`, `LDS`, `VBS`는 이 묶음에 새로 넣은 확인이라 **실제 CMU에서 처음 읽는 값**입니다.

| 줄 | ok의 조건 |
| --- | --- |
| BOOT | `reboot_check=new_boot_observed` |
| GUARD | `startup_state=guard_committed_after_new_boot` |
| ONCE | `one_boot=consumed_this_boot` |
| MODE | `config_mode=SHADOW` |
| STOP | `runtime_disable_next_start=absent` |
| DATA | `retained_bytes`가 0보다 큼 |
| POLL | `collector_poll_recent=observed` |
| AAPA, LDS, VBS | 각 서비스가 `running`이고 `package_preload=yes` |

이 블록은 화면의 상세 출력(`status_exit`, 그 밖의 `observed` 줄)을 대신 판단하지 않습니다. 정차 중에는 센서가 없거나 AA가 없어서 상세 출력의 `status_exit=1`이 나올 수 있고 그것은 판정에 쓰지 않습니다.
`NO   GUARD invalid_arm_marker`처럼 `startup_state`가 위와 다른 값이면 설치·재무장·수동 수정을 시도하지 마십시오.

**S7.** GO면 먼저 **AA 위치 후킹이 설치됐는지**를 확인하십시오. 다음 한 줄을 입력합니다(Shift가 필요한 글자가 없습니다).

```
grep hooks= /tmp/mnt/sda1/startup-result.txt
```

- `hooks=observed` → 후킹 설치됨. 그대로 진행합니다.
- `hooks=unavailable` → 후킹이 설치되지 않은 것입니다. **주행은 해도 됩니다**(센서 기록과 재부팅 검증, GPS 단절 기록은 얻습니다). 다만 AA 위치는 기록되지 않으니 이 화면을 사진으로 남겨 주십시오.

이어서 `0`과 Enter로 메뉴를 끝내고, USB를 빼고 **AA 동글을 연결하십시오.** 시동은 계속 켜 둡니다. 주차 상태에서 **기존 AA 기능이 평소처럼 동작하는지** 확인하십시오(연결, 터치, km/L 표시 등).
이번 릴리즈는 AA 위치 후킹이 처음으로 실제로 설치되므로, **AA가 연결되지 않거나 터치·km/L이 평소와 다르면 출발하지 말고** S9의 `3`과 `4`로 회수·제거한 뒤 알려 주십시오.

## 2. 주행 (조작 없음)

시동을 끄지 않고 이어서 갑니다. 이동 센서 자료는 이미 충분하므로 아래 **네 가지만** 채우면 됩니다.

- **하늘이 열린 곳에서 시작하십시오.** 지난번에는 처음 15분 동안 GPS 시각이 유효하지 않았습니다(첫 유효가 부팅 후 939초). 지붕이 있는 곳에서 재부팅과 판정을 마쳤기 때문으로 보입니다. 재부팅이 끝난 뒤 **하늘이 열린 곳에서 몇 분 서 있다가 출발**하고, 지상 도로를 **5분 이상** 달려 GPS를 잡은 뒤에 지하주차장으로 가십시오.
- **지하주차장 진입 → 안에서 30초 이상(주행이든 정차든, 회전이 있으면 더 좋음) → 지상 복귀**를 **2~3회** 하십시오. 복귀 후 매번 **지상에서 2분 이상** 주행해서 GPS가 다시 잡히게 하십시오(나온 직후 GPS 확보가 오차를 재는 기준입니다).
- **재부팅이 끝난 시점부터 회수까지 30분 이상, 50분 이내**. 지난번 재부팅이 부팅 약 24분에 났으므로 그보다 오래 버티는지가 수정 검증입니다. 50분은 로그 회전에 대한 이 절차의 여유입니다. 2026-10-05 주행에서 잰 SHADOW 기록량은 평균 약 27.7 KB/s, AA 연결 중 약 35.8 KB/s(대부분 송신 행)라서 120 MiB 기록은 약 58분 뒤부터 가장 오래된 부분이 순환됩니다([주행 기록](../validation/TRIP_SHADOW5_2026-10-05.md)). 이전에 적었던 105~115분은 AA 없이 잰 값이라 맞지 않았습니다.
- 시동을 끄지 마십시오.

폰과 지도는 평소처럼 쓰십시오. 이 시험은 지도에 아무것도 주입하지 않습니다. 정지, 좌우회전, 후진 주차 같은 항목은 이번에는 필요 없습니다.

**중단 기준**: 주행 중 AA 연결이 끊기거나 지도 위치가 멈추는 증상이 **새로** 생기거나, CMU가 다시 재부팅되면 더 진행하지 말고 안전한 곳에 주차한 뒤 S8부터 회수하십시오. 재부팅이 났다면 회수 때 SM이 남긴 보고서도 같이 가져옵니다(메뉴 3이 자동으로 수집합니다).

## 3. 주차 후 회수

시동을 끄기 전에 합니다. 엔진은 계속 켜 둡니다.

**S8.** AA 동글을 빼고 **같은 USB**를 꽂으십시오. S1과 같은 방식으로 셸을 열고 S2의 한 줄을 입력하십시오.

**S9.** `3`과 Enter를 누르십시오. **최대 15초 기다리고 USB를 빼지 마십시오.** 정상 화면의 끝부분은 다음과 같습니다(실제 실행 출력).

```
finish_scope=current_boot
Collector stop requested. Confirm collector_stop after the current poll; no PID was killed.
…
Current-boot capture freeze acknowledged and collector PID marker absent. Export logs now, before another boot.
finish_exit=0
export_scope=all_retained_boots
/tmp/mnt/sda1/mx5dr-logs-….tar
diagnostic_partial=0 (missing/truncated inputs are listed inside collection.txt)
Archive includes the whole installation, actual startup/SM files and bounded current-boot diagnostics. Keep it private.
Live writer may have rotated during export; partial final JSONL records are possible. Originals retained.
export_exit=0
Result saved: /tmp/mnt/sda1/trial-result.txt
Capture finish acknowledged and available files exported. This menu now exits. …
```

판정은 `export_exit=`가 합니다.

- `export_exit=0` → **회수 성공.** `finish_exit=0`이든 아래의 `finish_exit=1`이든 같습니다.
- 다음 두 줄이 나오고 `finish_exit=1`이어도 **정상으로 취급합니다**(AA 쪽 종료 확인이 없었다는 뜻이며 내보내기는 계속됩니다). 다시 실행하지 마십시오.

  ```
  mx5dr: Freeze not confirmed within deadline; do not assume complete logs. No process killed. Preserve/export available logs and report this result.
  finish_exit=1
  ```

  이 경우 `diagnostic_partial=` 줄 이하와 `export_exit=0`, `Result saved: …`가 이어서 나옵니다. 끝의 `Capture finish acknowledged…` 줄만 없습니다.
- `export_exit=`가 0이 아니거나 `Result saved`가 없으면 사진을 남기고 **`3`을 한 번만 다시** 실행하십시오. 또 실패하면 `4`를 하지 말고 알려 주십시오. 원본은 CMU에 남아 있어 나중에 `3`으로 다시 회수할 수 있습니다.

**S10.** 메뉴가 끝나면 S2의 한 줄을 다시 입력하고 **`4`와 Enter**로 제거하십시오. 다음 줄이 **그대로** 나와야 합니다.

```
Removed owned one-boot autostart blocks and mx5dr preload tokens; OFF config staged. No restart/kill. Library/logs/backups retained for mapped-code lifetime and diagnosis.
```

순정 BusyBox 실행에서 이 제거 뒤 `sm.conf`, `sm_WCP.conf`, `autostart`가 순정과 바이트 단위로 같았습니다. 이 줄이 나오지 않으면 복귀를 완료했다고 보지 말고 사진을 남기고 알려 주십시오.
이 절차는 제거 뒤의 추가 재부팅을 요구하지 않습니다. 일회성 가드는 시험 부팅에서 이미 소비됐습니다.

**S11.** 이제 시동을 꺼도 됩니다. USB를 가져와 다음을 비공개로 보관하고 알려 주십시오(차량 정보가 들어 있습니다).

- `mx5dr-logs-….tar`와 같은 이름의 `.sha256`
- `trial-result.txt`, `startup-result.txt`, `reboot-request.txt`

## 이 절차가 검증하지 못한 것

- 셸을 여는 단계(S1)와 실제 USB·FAT, 실제 재부팅, 실제 SM·AA·VBS·LDS 프로세스, 실제 센서 기록은 에뮬레이션에 없습니다. 위 화면의 센서·서비스 관련 줄은 제가 만든 `/proc`와 합성 기록에서 나온 값입니다.
- service 계정의 파일 소유권(`proot`가 흉내 낼 수 없어 검사하지 못함)과 실제 소요 시간.
- 5분, 60초, 50분 같은 숫자는 에뮬레이션 측정이 아니라 이 절차가 정한 기준입니다.
- 이 시험은 v1.0 완료나 ASSIST 활성화를 승인하지 않습니다.
