# 차량 시험 한 번의 절차 — 2026-10-04

**이 문서는 절차입니다. 차량 방문이나 주행을 요청하는 것이 아닙니다.** 사용자가 일정을 정하면 이 순서를 그대로 따르십시오.
화면 인용은 모두 이 묶음의 설치 파일을 **순정 BusyBox 1.19.2 환경에서 실제로 실행한 출력**입니다
([검증 기록](../validation/STOCK_BUSYBOX_MENU_2026-10-04.md)). 단, 그 실행은 에뮬레이션이라 **실제 CMU에서 나오는 값(번호, 시간)은 다를 수 있습니다.**
아래 표에서 "그대로"라고 한 문자열만 판정에 쓰고, 번호·경로·저장 용량 숫자는 판정에 쓰지 않습니다.

## 이 시험이 하는 일

기록만 합니다. 순정 위치에 아무것도 주입하지 않고 ASSIST는 꺼져 있습니다. 목적은 이 차에서 휠·yaw·후진 센서와 GPS 끊김 구간(지하주차장)의
실제 기록을 한 번에 얻어 집에서 재생으로 DR을 개선하는 것입니다. 운전 중에는 아무것도 조작하지 않습니다.

## 하지 말 것

- **시동 OFF/ON으로 CMU 재부팅을 대신하지 마십시오.** 재부팅은 메뉴 `5`로만 합니다.
- 설치 후 다시 `1`을 누르지 마십시오(회수 때도).
- USB와 AA 동글을 동시에 연결하지 마십시오(포트가 하나입니다).
- 시험 중 시동을 끄지 마십시오. 끄면 그 부팅의 기록이 끝납니다.
- 운전 중 USB를 바꾸거나 명령을 입력하지 마십시오.

## 0. 집에서 준비

1. 설치 ZIP `mx5dr-usb.zip`을 받아 SHA-256을 확인하십시오. 정확히 아래와 같아야 합니다.

   ```
   bbdd0ab9a51b61d0e3df94fb497bc5579d6ea32bbfda1c8d341c68c49109bdc9
   ```

   다르면 그 ZIP을 쓰지 말고 알려 주십시오.
2. ZIP을 풀어 **내용물 전체**를 깨끗한 FAT32 USB 최상위에 복사하십시오. USB를 열었을 때 `trial`, `install.sh`, `mp3`, `js`가 바로 보여야 합니다.
3. 이 USB 하나로 설치·확인·회수를 모두 합니다.

## 1. 주차 상태에서 설치와 재부팅

조건: 환기되는 장소, 주차브레이크, 6MT 중립, **엔진이 실제로 가동 중**(타코미터가 0보다 높음). 이 절이 끝날 때까지 엔진과 USB를 유지합니다.

**S1.** USB를 꽂고, **기존에 성공하신 방법**으로 키보드 입력이 되는 셸을 여십시오(포함된 곡 재생, 약 15초). 이 단계는 제가 에뮬레이션하지 못했습니다.

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

**S5. 재부팅이 됐는지는 화면이 아니라 다음 확인으로만 판정합니다.** 화면이 돌아오면 S1처럼 셸을 열고 S2의 한 줄을 입력한 뒤 **`2`와 Enter**를 누르십시오.
첫 줄이 이 값이면 새 부팅입니다.

- `reboot_check=new_boot_observed` → 재부팅됨. S6으로 가십시오.
- `reboot_check=same_boot` → **재부팅되지 않았습니다.** `5`를 한 번 더 누르고 5분 기다린 뒤 다시 `2`로 확인하십시오(5분은 이 절차가 정한 기준입니다). 그래도 `same_boot`면 중단하고 S9의 `3`, `4`를 하고 알려 주십시오.
- `reboot_check=unavailable` → 요청 기록을 읽지 못한 것입니다. 중단하고 사진을 남기고 알려 주십시오.
- 5분이 지나도 화면이 바뀌지 않으면 셸을 열어 `2`를 확인하십시오. 확인 결과가 위 셋 중 무엇인지가 판정입니다.

**S6. 시험 시작 판정.** `2`의 화면에서 다음이 **모두** 있어야 합니다(순정 BusyBox 실행에서 실제로 나온 줄입니다).

| # | 화면에 있어야 하는 문자열(그대로) |
| --- | --- |
| 1 | `reboot_check=new_boot_observed` |
| 2 | `startup_state=guard_committed_after_new_boot` |
| 3 | `one_boot=consumed_this_boot` |
| 4 | `config_mode=SHADOW` |
| 5 | `runtime_disable_next_start=absent` |
| 6 | `retained_bytes=` 뒤의 숫자가 0보다 큼 |
| 7 | `collector_poll_recent=observed` |
| 8 | `service_jciAAPA=running`, `service_jciLDS=running`, `service_jciVBS=running` 세 줄이 **각각** `package_preload=yes`를 포함 |

서비스 줄의 형식은 실제 실행에서 이렇게 나왔습니다.

```
service_jciAAPA=running pid=812 uid=0 stack_soft_bytes=131072 package_preload=yes
service_jciLDS=running pid=813 uid=0 stack_soft_bytes=131072 package_preload=yes
status_exit=0
Startup check saved: startup-result.txt
```

`jciVBS` 줄도 같은 형식이며 차에서는 `package_preload=yes`여야 합니다. 이 묶음은 시험 설정에서 `jciVBS`에 VBS 탭 preload를 거는데, 그 탭이 센서의 원천이기 때문입니다.
(에뮬레이션의 `jciVBS` 줄이 `package_preload=no`로 나온 것은 제가 그 시험용 `/proc`에 탭을 넣지 않아서이며 설계가 아닙니다.)
`pid` 숫자, `uid`, `status_exit`, 그 밖의 `observed` 줄은 판정에 쓰지 않습니다. 정차 중이라 센서가 아직 없거나 AA가 없어서 `status_exit=1`이 나올 수 있습니다.
8번은 이 묶음에 새로 넣은 확인이라 **실제 CMU에서 처음 읽는 값**입니다. 에뮬레이션의 값은 제가 만든 `/proc` 자료로 나온 것입니다.

- 1~8이 모두 맞으면 **출발(GO)**.
- 서비스 줄이 `not_running`이거나 `retained_bytes`가 0이거나 `collector_poll_recent`만 없으면 **60초 기다린 뒤 `2`를 한 번 더** 확인하십시오.
- 그래도 1~8 중 하나라도 맞지 않으면 **출발하지 마십시오(NO-GO).** S9의 `3`, `4`를 하고 USB를 가져와 알려 주십시오. 다시 `1`을 누르지 마십시오.
- `startup_state=`가 위와 다른 값(예: `invalid_arm_marker`)이면 같은 NO-GO입니다. 설치·재무장·수동 수정을 시도하지 마십시오.

**S7.** GO면 `0`과 Enter로 메뉴를 끝내고, USB를 빼고 **AA 동글을 연결하십시오.** 시동은 계속 켜 둡니다. AA가 평소처럼 연결되는지 주차 상태에서 확인하십시오.

## 2. 주행 (조작 없음)

시동을 끄지 않고 이어서 갑니다. 아래를 한 번의 주행 안에서 채우십시오.

- **재부팅이 끝난 시점부터 회수까지 90분 이내**(상태 화면이 안내하는 로그 회전은 약 105~115분입니다. 90분은 이 절차가 정한 여유입니다). 주행 시간은 30분 이상.
- 정지 3회 이상(신호 대기 가능).
- 좌회전 3회 이상, 우회전 3회 이상, GPS가 잘 잡히는 곳에서 직선 5분 이상.
- **지하주차장에 들어가 GPS가 끊긴 상태로 30초 이상 있다가(주행이든 정차든) 다시 지상으로 나와 2분 이상 주행.** 들어가기 직전과 나온 직후에 GPS가 잡혀 있어야 오차를 잴 수 있습니다.
- 도착해서 **후진 주차 1회.**
- 폰과 지도는 평소처럼 쓰십시오. 이 시험은 지도에 아무것도 주입하지 않습니다.

## 3. 주차 후 회수

시동을 끄기 전에 합니다. 엔진은 계속 켜 둡니다.

**S8.** AA 동글을 빼고 **같은 USB**를 꽂으십시오. S1, S2와 같이 셸을 열고 한 줄을 입력하십시오.

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
- 5분, 60초, 90분 같은 숫자는 에뮬레이션 측정이 아니라 이 절차가 정한 기준입니다.
- 이 시험은 v1.0 완료나 ASSIST 활성화를 승인하지 않습니다.
