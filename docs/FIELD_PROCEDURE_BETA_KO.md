# BETA 설치·사용 절차 (2026-10-05, 2026-10-06 상시 실행으로 갱신)

**2026-10-06 갱신: BETA는 한 번만 켜지는 시험이 아니라 설치해 두면 매 부팅 스스로 켜지는 제품입니다.** 설치는 한 번(메뉴 `1`), 첫 시작은 메뉴 `5`이고 그 뒤에는 아무것도 누르지 않아도 됩니다. 안전은 제거가 아니라 자동 차단이 맡습니다. 아래 "상시 실행과 자동 차단"을 먼저 읽으십시오([설계](../validation/PERSISTENT_GUARD_DESIGN_2026-10-06.md)).

**이 문서는 절차입니다. 차량 방문이나 주행을 요청하는 것이 아닙니다.** 화면 인용은 순정 BusyBox 1.19.2 에뮬레이션(`validation/STOCK_BUSYBOX_BETA_2026-10-05.md`)에서 제품이 쓴 BETA 기록으로 만든 출력이며 실제 CMU에서 실행한 것이 아닙니다. 실제 값(번호, 시간, 개수)은 다릅니다.
구조와 하지 말 것은 [2026-10-04 절차](FIELD_PROCEDURE_2026-10-04_KO.md)와 같습니다. 이 절차 이전의 `shadow.5` 주행에서 후킹 설치와 약 15분의 무리셋이 확인됐습니다([기록](../validation/TRIP_SHADOW5_2026-10-05.md)).

## 이 시험이 하는 일

[문제 정의](PROBLEM_DEFINITION_KO.md)의 G1(터널에서 속도), G2(지하주차장에서 방위)를 실제로 확인하는 첫 시험입니다.
BETA는 SHADOW 기록을 그대로 하면서 순정 위치를 송신 시점에 세 가지로 나눠 다룹니다([설계](../validation/ASSIST_BETA_DESIGN_2026-10-05.md), [결정 갱신](../validation/BETA_DECISIONS_2026-10-05.md)).

- **GPS 끊김(LOST, 순정 mode 0)**: 그 LOCATION의 위경도·정확도·속도·방위를 차량 센서로 계산한 값으로 바꿔 보냅니다(상태 `GPS_LOST` → `ENGAGED`). **터널 모드(v1.0.0-beta.6부터)**: GPS가 돌아올 때까지 끊김 구간 안에서는 시간·거리 제한 없이(수치 상한 6시간·1000 km) 계속 바꿔 보냅니다. 단, 끊기는 순간 마지막 앵커가 60초보다 오래됐거나 실제 예산이 40 m를 넘었으면(예: 앵커 속도 상한 60 km/h를 넘는 고속 주행 후) 그 끊김은 순정으로 둡니다. 정차 중에는 속도 0에 마지막 방위를 유지해 보냅니다. 보고하는 정확도는 `min(실제 예산, 40 m)`이며, 실제 예산이 40 m를 넘은 뒤에는 보고 위치가 실제와 수백 m 어긋날 수 있습니다(실제 예산은 기록의 `accuracy_honest_m`). 센서 끊김, 후진 의심, 세션·저장소 변경, 연결 끊김은 여전히 순정으로 돌아갑니다.
- **부팅 후 fix 없음(NO_FIX, 순정 mode 1/2이고 시각 utc 0)**: 순정이 저장해 둔 옛 위치를 보내는 구간입니다. 위치·방위·정확도는 순정 그대로 두고 **속도만** 바퀴 속도로 덮어씁니다(상태 `NO_FIX` → `SPEED_ENGAGED`). 이 구간에서 방위(G2)는 고칠 수 없습니다.
- **GPS 정상(FIX)과 그 밖**: 순정을 그대로 보냅니다. GPS가 돌아오면 즉시 순정입니다.

의심이 있으면(센서 침묵, 송신 실패, 세션 변화) 스스로 순정으로 물러납니다.

한 번의 주행으로 얻을 것:

1. 단절 구간에서 BETA가 실제로 켜지고(`ENGAGED`) 바꾼 송신이 있는지, 그리고 왜 물러났는지. 시동 직후 fix가 없는 동안 속도 덮어쓰기(`SPEED_ENGAGED`)가 있었는지.
2. 네이버가 단절 구간에서 속도·방위를 따르는지(관찰은 동승자가 있을 때만; 없으면 기록으로 판단합니다).
3. GPS 복귀 때 마지막으로 보낸 DR 위치와 첫 GPS 위치의 거리(회수 후 분석 도구가 계산합니다).

운전 중에는 아무것도 조작하지 않습니다. BETA는 주행 중 끄는 방법이 없고 필요 없습니다. 이상이 있으면 스스로 순정으로 돌아갑니다.

## 상시 실행과 자동 차단

- **매 부팅 실행**: 메뉴 `1`로 한 번 설치하면 CMU가 새로 부팅할 때마다 가드가 BETA를 다시 켭니다. 순정 설정 파일(`sm.conf`, `sm_WCP.conf`)에는 아무것도 쓰지 않고, 매 부팅 `/tmp`에 그 부팅용 설정을 새로 만듭니다. 설치한 그 부팅에서는 켜지지 않으므로 처음에 한 번 `5`로 재부팅합니다.
- **자동 차단**: 순정 SM은 CMU를 스스로 재부팅하기 직전에 `/data`에 보고서(`dmesg.out` 등)를 남깁니다. 가드는 매 부팅 이 보고서가 지난 BETA 부팅 이후 바뀌었는지 봅니다(시계를 쓰지 않습니다). **BETA 부팅 두 번이 연달아 CMU 리셋으로 끝나면** 그다음 부팅부터 BETA를 켜지 않고 순정만 실행합니다. 연달아가 아니어도 최근 10번 중 3번이 리셋이면 같습니다. 런타임이 스스로 꺼짐 표식(`disable-next-start`)을 남겨도 같습니다. 기록과 보고서는 지우지 않습니다.
- **확인(2026-10-06 추가)**: BETA로 시작한 부팅은 약 90초 뒤 가드가 SM이 그 설정으로 아직 돌고 있는지 스스로 확인합니다(`this boot confirmed yes`). 보고서도 확인도 없는 부팅(SM이 설정을 거부, 전원 차단, 90초 전에 시동을 끔)이 **세 번 연달아** 나오면 차단합니다. 메뉴 `1` 직후의 **첫** BETA 부팅은 반드시 확인돼야 하며, 아니면 바로 차단합니다(`PERSIST tripped (probation)`). 그래서 `5`로 재부팅한 뒤 메뉴 `2`가 `ok   CONF  confirmed`를 보여 줄 때까지(약 2분) 시동을 유지하십시오. 2분이 안 되는 짧은 운행이 세 번 연달아 이어져도 차단됩니다(안전한 쪽입니다. `1`로 다시 켭니다).
- **멈춤 방지**: 가드가 멈춰도 순정 BusyBox `timeout`이 15초 안에 끝내고 순정으로 시작합니다.
- **차단은 저절로 풀리지 않습니다.** 메뉴 `2`가 `PERSIST tripped (…)`와 다음 두 줄을 보여 줍니다.

  ```
  NO-GO: tripped, stock runs, BETA off.
  Find the cause, then run menu 1 again.
  ```

  `3`으로 기록을 회수해 알려 주십시오. 원인을 이해한 뒤에만 `1`로 다시 켭니다(차단 횟수가 0으로 돌아갑니다).
- **이 빌드의 한계**: 런타임이 "120초 안정" 표식(`healthy`)을 아직 쓰지 않아 그 규칙은 꺼져 있습니다(`attempts since healthy … (rule off)`). 확인(90초) **뒤에** 보고서 없이 일어나는 재부팅(전원 차단, 커널 패닉)과, 리셋 없이 AA·터치·위치만 이상한 경우는 가드가 알 수 없습니다. 이상하면 메뉴 `4`로 제거하십시오.
- **차는 알려 주지 않습니다. 몇 번 운행할 때마다 주차 상태에서 메뉴 `2`를 확인하십시오.** 순정으로 시작한 부팅이면 `NO   BOOT  stock this boot (이유)`가, 다른 도구가 `sm.conf`를 고친 경우 `PERSIST enabled but bindings changed`와 할 일(`run menu 1`)이 나옵니다.
- **상태 보기**: 언제든 주차 상태에서 메뉴 `2`. 판정 블록 위쪽 다섯 줄이 상시 실행 상태입니다.

  ```
  PERSIST enabled (every boot)
  fail count 0 of 2, unconfirmed 0 of 3
  recent boots CCC
  attempts since healthy 3 (rule off)
  healthy previous boot no
  this boot selected yes
  this boot confirmed yes
  ```

  `fail count 1 of 2`는 직전 BETA 부팅이 리셋으로 끝났다는 뜻입니다. 이번 부팅도 리셋으로 끝나면 다음 부팅부터 차단됩니다. `unconfirmed 2 of 3`은 확인 없는 부팅이 두 번 이어졌다는 뜻입니다. `recent boots`는 최근 부팅 결과입니다(C 확인, U 미확인, R 리셋). `this boot confirmed pending`이면 90초가 아직 지나지 않았습니다. 60초 뒤 `2`를 다시 누르십시오.
- **다시 켤 때의 기록 보존**: 메뉴 `1`은 이전 상태 파일을 `backups/persist-evidence/번호`에 복사하고(최근 3개) 어떤 이유로 차단돼 있었는지 보여 줍니다. 차단 중이었다면 먼저 `3`으로 회수하라고 안내합니다.
- **제거**: 메뉴 `4`. 자동 실행 블록과 상시 실행 설정을 지우고 순정으로 돌아갑니다(기록은 남김). 다시 쓰려면 `1`.
- **완전 삭제(2026-10-07)**: 설치한 적 없던 상태가 필요하면 `4` 뒤 `5`로 재부팅하고 메뉴 `6`(`6 Delete everything this package left on the CMU (run 4, then 5, first)`), 안내 뒤 확인으로 `6`을 한 번 더 입력합니다(2026-10-08). 로그·백업까지 모두 지우므로 필요하면 먼저 `3`으로 회수합니다. 3절 4번을 보십시오.

## 하지 말 것

- 시동 OFF/ON으로 CMU 재부팅을 대신하지 마십시오. 재부팅은 메뉴 `5`로만 합니다.
- 차단(`PERSIST tripped`)이 아닐 때 다시 `1`을 누르지 마십시오. 누르면 실패 횟수가 0으로 돌아가고 그 부팅에서는 BETA가 꺼집니다(다시 `5`가 필요합니다).
- USB와 AA 동글을 동시에 연결하지 마십시오(포트가 하나입니다).
- 시험 중 시동을 끄지 마십시오. 끄면 그 부팅의 기록과 BETA가 끝납니다.
- 운전 중 USB를 바꾸거나, 명령을 입력하거나, 폰을 보며 비교하지 마십시오.

## 0. 집에서 준비

1. 릴리즈의 설치 ZIP과 `.sha256`을 받아 SHA-256을 확인하십시오. 릴리즈 노트의 값과 같아야 합니다.
2. ZIP 내용물 전체를 깨끗한 FAT32 USB 최상위에 복사하십시오. `trial`, `install.sh`, `mp3`, `js`, `bundle-default-mode`가 바로 보여야 합니다.
3. 이 ZIP은 `--default-mode BETA`로 만든 것이어야 합니다. 메뉴 첫 화면이 그것을 알려 줍니다(S2). 다른 모드를 고르기 위해 추가로 입력할 것은 없습니다.

## 1. 주차 상태에서 설치와 재부팅

조건: 환기되는 장소, 주차브레이크, 6MT 중립, **엔진이 실제로 가동 중**. 이 절이 끝날 때까지 엔진과 USB를 유지합니다.

1. USB를 꽂고 이전 릴리즈와 같은 방식으로 셸을 여십시오(Entertainment → USB에서 곡 재생 후 약 15초).
2. 다음 한 줄을 입력하고 Enter를 누르십시오(Shift가 필요한 글자와 밑줄이 없습니다).

   ```
   sh /tmp/mnt/sda1/trial
   ```

   메뉴 맨 위에 다음 두 줄이 **그대로** 나와야 합니다.

   ```
   This USB installs mode BETA: BETA = SHADOW capture + dead-reckoned LOCATION only while GPS is lost
   1 Install or re-enable BETA, every boot
   ```

   `mode SHADOW`나 `mode OBSERVE`가 나오면 BETA 묶음이 아닙니다. 설치하지 말고 알려 주십시오. `can't open`이 나오면 `sda1`을 `sdb1`, `sdc1`로 바꿔 다시 입력하십시오.
3. `1`과 Enter. 끝날 때까지 기다리십시오. `Installed BETA persistent: the guard starts it on every CMU boot.`와 `Installation finished.`가 나와야 합니다. 없거나 오류가 나오면 `5`를 누르지 말고 사진을 남기십시오. 이 부팅에서 `2`를 누르면 `NO BOOT installed; choose 5`가 정상입니다.
4. `5`와 Enter. `reboot_command_exit=0`이 나오면 시동 버튼을 누르지 말고 CMU 화면이 꺼졌다 켜질 때까지 기다리십시오.
5. 화면이 돌아오면 셸을 다시 열고 2번의 한 줄을 입력한 뒤 `2`와 Enter. 화면 맨 아래 판정 블록으로만 판단합니다. 정상이면 다음과 같은 모양입니다(합성 기록의 호스트 출력).

   ```
   ---- GO / NO-GO ----
   PERSIST enabled (every boot)
   fail count 0 of 2, unconfirmed 0 of 3
   recent boots none
   attempts since healthy 1 (rule off)
   healthy previous boot none
   this boot selected yes
   this boot confirmed yes
   ok   BOOT  product this boot
   ok   CONF  confirmed
   ok   GUARD committed
   ok   PERS  enabled
   ok   MODE  BETA
   ok   STOP  not disabled
   ok   DATA  1275 bytes
   ok   POLL  polling 1s ago
   ok   HLTH  runtime 0s ago
   ok   AAPA  preload yes
   ok   LDS   preload yes
   ok   VBS   preload yes
   ok   BETA  armed
   ok   HOOK  installed
   ok   FENCE declined
   BETA: engaged 0 times, replaced 0 sends,
    speed overlay sends 0, nonzero 0,
    hold 0, last state ARMED (enabled),
    scope current_boot
   GO
   ```

6. 마지막 줄과 BETA 세 줄로 판정합니다.

   | 줄 | ok의 조건 | 아니면 |
   | --- | --- | --- |
   | POLL, HLTH | 마지막 collector 폴링과 런타임 health가 몇 초 전인지 함께 표시 | `wait … last poll 12s ago`: 60초 뒤 `2`를 다시 누르십시오 |
   | MODE | `config_mode=BETA`이고 이 USB의 모드와 같음 | `NO MODE SHADOW not BETA` 등: 출발하지 말고 3절 회수·제거 |
   | BETA | 이 부팅의 첫 결정이 `armed` | `NO BETA disabled:…`: BETA가 꺼진 이유입니다. 사진을 남기십시오 |
   | HOOK | AA 위치 후킹 설치(`install=ok`) | `NO HOOK not_installed`: BETA가 동작할 수 없습니다 |
   | FENCE | `declined`(libpatch 공존) 또는 `observed` | `NO FENCE …`: 세션 경계가 없습니다 |

   - `GO` → 7번으로 갑니다.
   - `WAIT 60 s, then run 2 again` → 60초 기다린 뒤 `2`를 한 번 더 누르십시오. `wait BETA no state yet`도 여기에 해당합니다.
   - `NO-GO`이고 `NO`가 **BETA, HOOK, FENCE 줄에만** 있으면: 기록(SHADOW)은 동작합니다. 주행하면 2026-10-04 절차와 같은 자료는 얻지만 BETA 시험은 되지 않습니다. 사진을 남기고, 주행 여부는 소유자가 정하십시오.
   - 그 밖의 `NO-GO` → 출발하지 마십시오. 사진을 남기고 3절의 `3`과 `4`를 하십시오. `NO BOOT installed; choose 5`만은 `5`를 한 번 더 누르고 5분 뒤 `2`로 다시 확인합니다. `PERSIST tripped`는 위 "상시 실행과 자동 차단"을 따르십시오.
7. `0`과 Enter로 메뉴를 끝내고, USB를 빼고 AA 동글을 연결하십시오. 주차 상태에서 AA 연결, 터치, km/L 표시가 평소와 같은지 확인하십시오. 다르면 출발하지 말고 회수·제거하십시오.

## 2. 주행 (조작 없음)

- 하늘이 열린 곳에서 몇 분 서 있다가 출발하고, 지상 도로를 5분 이상 달려 GPS를 잡으십시오.
- **지하주차장 진입 → 안에서 1분 이상(회전이 있으면 더 좋음) → 지상 복귀**를 2~3회, 가능하면 **터널** 통과를 1회 이상 하십시오. 복귀 뒤 매번 지상에서 2분 이상 달려 GPS를 다시 잡으십시오. GPS 복귀 직후의 위치가 BETA 정확도를 재는 기준입니다.
- 재부팅부터 회수까지 30분 이상, **50분 이내**. 기록량이 AA 연결 중 약 35.8 KB/s(2026-10-05 SHADOW 주행 측정)이고 BETA 행과 송신 행의 `class` 필드, AA GEAR 송신(type 8, 4바이트)의 `payload_hex`가 1.3 KB/s 이하를 더해 약 37 KB/s 이하로 추정되므로, 120 MiB 기록은 약 56분 뒤부터 가장 오래된 부분이 순환됩니다(추정, 차량 측정 아님). 시동을 끄지 마십시오.
- 동승자가 있으면 단절 구간에서 네이버의 속도 표시가 차 속도를 따르는지, 방위가 진행 방향과 맞는지, "GPS 끊김" 표시가 나오는지, 복귀 때 위치가 튀는지를 메모해 주십시오. 혼자라면 보지 마십시오. 기록으로 판단합니다.

**중단 기준**: AA 연결이 끊기거나, 지도 위치가 멈추거나 크게 튀는 증상이 **새로** 생기거나, CMU가 재부팅되면 더 진행하지 말고 안전한 곳에 주차한 뒤 3절로 회수하십시오.

## 3. 주차 후 회수

시동을 끄기 전에 합니다. 엔진은 계속 켜 둡니다.

1. AA 동글을 빼고 같은 USB를 꽂은 뒤 셸을 열고 1절 2번의 한 줄을 입력하십시오.
2. **먼저 `2`와 Enter.** 판정 블록 아래쪽의 BETA 줄이 이번 주행의 요약입니다. 사진을 남기십시오. 예(합성 기록의 호스트 출력):

   ```
   BETA: engaged 2 times,
    replaced 31 sends,
    speed overlay sends 523, nonzero 0,
    hold 0,
    last state ARMED (gps_returned),
    scope current_boot
   ```

   - `engaged N times`: BETA가 켜진 GPS 끊김(mode 0) 구간 수. 지하·터널 진입 횟수와 비슷해야 합니다. 0이면 단절 중에도 켜지지 않은 것입니다(원인은 기록에 남습니다). 예산은 끊긴 시점이 아니라 마지막으로 **받아들인 앵커**부터 흐르므로, 느린 주차장 진입에서는 0이 나올 수 있습니다.
   - `replaced M sends`: 위치까지 바꿔 보낸 LOCATION 수(choice 3, GPS 끊김 구간만). 단절 1초마다 대략 1건입니다.
   - `speed overlay sends S, nonzero Z`: 시동 직후 fix가 없는(NO_FIX) 동안 속도만 바꿔 보낸 LOCATION 수(choice 4)와 그중 하위 송신 결과가 0이 아닌 수. fix 없이 달린 1초마다 대략 1건이고, 서 있거나 이미 같은 속도면 덮어쓰지 않습니다. Z는 0이 기대값입니다.
   - `hold K`: 바꾼 송신(위치 또는 속도)이 실패해 BETA가 물러난 횟수. 0이 기대값입니다.
   - `last state`: 마지막 상태와 이유. 지상에서 회수하면 보통 `ARMED (gps_returned)`입니다. `NO_FIX`나 `SPEED_ENGAGED`이면 회수 때까지 첫 fix가 없었던 것입니다(2026-10-05 주행에서는 첫 fix가 주차 뒤 727초에 나왔습니다).
   - `scope previous_boot`가 나오면 시험 부팅이 이미 끝난 뒤(시동을 껐거나 CMU가 재부팅됨)라는 뜻입니다. 숫자는 그 부팅의 기록입니다.

   **이 단계의 짧은 판정(`GO`든 `NO-GO`든)은 출발 전 시작 확인용입니다. 주행이 끝난 뒤의 `GO`를 주행 결과로 읽지 마십시오.** 주행 결과는 위 BETA 줄만 봅니다. 같은 내용이 USB의 `startup-result.txt`에 저장됩니다.
3. `3`과 Enter. `export_exit=0`이면 회수 성공입니다(2026-10-04 절차 S9와 같음). 상세 상태(BETA 줄, 상시 실행 상태 포함)가 `trial-result.txt`에 저장됩니다. `3`의 기록 정지는 이번 부팅에만 해당하며 다음 부팅에서 BETA가 스스로 다시 켜집니다.
4. **제거는 필요하지 않습니다.** BETA를 더 쓰지 않으려면 셸을 다시 열고 `4`와 Enter. `Removed owned one-boot autostart blocks, the persistent enablement…` 줄이 나와야 합니다.
   흔적까지 지우려면(설치한 적 없던 상태) 이어서 메뉴를 다시 열어 `5`로 CMU를 재부팅하고, 화면이 돌아오면 셸과 메뉴를 다시 열어 `6`과 Enter를 누르십시오. 수집 로그까지 지운다는 안내가 나오면 `6`과 Enter를 한 번 더 누르십시오(다른 입력은 취소, 2026-10-08). 화면 끝에 다음과 같은 결과가 나옵니다(복제 루트 출력, 숫자는 다릅니다).

   ```
   ---- DELETE RESULT ----
   Deleted 67 files, 39942433 bytes
   Package directory absent
   OEM files: identical to pre-install
   (all 4 recorded states agree)
   Saved: purge-result.txt
   ```

   터치 모드 등 다른 도구가 설치 뒤 `sm.conf`를 바꿨다면 `OEM files differ from pre-install:`과 `sm.conf (kept as it is)`가 나옵니다. 오류가 아니며 되돌리지 않습니다. 설치 사이에 다른 도구가 파일을 바꿔 설치 전 기록끼리 다르면 `Pre-install records disagree.`와 현재 파일이 맞는 기록 이름(없으면 `none`)이 나옵니다. `Refused, nothing deleted: …`이면 아무것도 지우지 않았습니다. 한 줄 사유대로 `4` 또는 `5`를 먼저 하고 다시 `6`을 누르십시오. 같은 내용과 지운 파일 목록이 USB의 `purge-result.txt`에 저장됩니다.
5. 시동을 꺼도 됩니다. 다음을 비공개로 보관하고 알려 주십시오: `mx5dr-logs-….tar`와 `.sha256`, `trial-result.txt`, `startup-result.txt`, `reboot-request.txt`, 2번의 사진, 동승자 메모가 있으면 그것도.

## 요 센서 영점 자료 (2026-10-09, 로그 전용)

이 변경을 포함한 빌드부터 persistent 로그가 요(yaw) 센서 영점 질문에 쓸 작은 행을 함께 남깁니다: 정차마다 `yaw_stop` 1행(네 바퀴 모두 0인 1초 이상 정차, 5초 안에 이어진 정차는 합침), GPS 끊김·복귀마다 `yaw_edge`, 요 신호가 1초 넘게 끊기거나 4095 표지가 오면 `yaw_reinit`, 그리고 10초 요약(`log_digest`)의 1초 단위 요 합과 GPS 침로. 소유자가 할 일은 **없습니다**. 주행 중 조작도 새 메뉴도 없으며, 주차 후 지금처럼 3절의 `3`(메뉴 3)으로 회수하면 같은 `mx5dr-logs-….tar`에 들어갑니다. 기록량은 합성 주행에서 초당 약 21 B(상시 정체 주행 약 36 B) 늘어납니다. BETA 판단, 송신, 위치 출력은 바뀌지 않습니다. 이미 설치된 v1.0.0-beta.6에는 이 행이 없습니다. 근거와 한계는 validation/YAW_DATA_COLLECTION_2026-10-09.md에 있습니다.

## 가속도·브레이크·회전수 원시 자료 (2026-10-10, 로그 전용)

이 변경을 포함한 빌드부터 VIM tap이 이미 받고 있던 메시지 가운데 제품이 쓰지 않던 값(0x116의 종가속도·브레이크 압력·품질 비트, 0x169의 횡가속도, 0x15B의 차속·엔진 회전수)을 별도 진단 소켓으로 복사하고, persistent 로그에 20초마다 `chan_digest` 1행(채널별 개수·최소·최대·평균, 정차 중 평균, 차속·회전수 마지막 값과 변화 횟수)을 남깁니다. 단위·부호·영점은 모르므로 원시 정수만 기록합니다. 소유자가 할 일은 **없습니다**. 주행 중 조작도 새 메뉴도 없으며 3절의 `3`(메뉴 3) 회수 파일에 함께 들어갑니다. 기록량은 합성 주행에서 초당 약 10 B 늘어납니다. 모션 이벤트, BETA 판단, 송신, 위치 출력은 바뀌지 않습니다. 0x169·0x15B가 실제 차량에서 이 콜백에 도착하는지는 아직 확인하지 않았으며, 회수한 로그의 `chan_digest` `n` 값이 그 답이 됩니다. 평소에는 끌 필요가 없습니다. 끄거나 다시 켜려면 주차 중 USB `trial` 메뉴에서 `7`을 고르고(현재 상태 표시), 한 번 더 `7`을 입력한 뒤 `5`로 재부팅합니다. CMU 시작 때 한 번만 읽기 때문에 재부팅 전까지는 이전 상태로 동작합니다. 메뉴 `2` 상태 보고서의 `vim_side_channel_marker=present/absent`와 `vim_side_channel=` 값으로 확인할 수 있고, 메뉴 `1`(재설치)은 기본값(켜짐)으로 돌립니다. 설정 파일(`mx5dr.conf`)은 고치지 마십시오. 고치면 guard가 제품 전체를 거절합니다. 이미 설치된 v1.0.0-beta.6에는 이 행이 없습니다. 근거와 한계는 validation/VIM_CHANNEL_CAPTURE_2026-10-10.md에 있습니다.

## 회수 뒤 분석 (PC)

`python3 analyze_logs.py mx5dr-logs-….tar`가 BETA 행을 검사합니다. 위치를 바꾼 송신(choice 3)마다 원본 mode 0, 정확도 0 초과 40 m 이하(터널 모드에서는 40 m로 고정된 값이며 GPS 복귀 점프는 기록의 `accuracy_honest_m`과 비교합니다), 0~7·24~31바이트 원본 유지, `hasAccuracy=1`, 앞선 `ENGAGED`/`GPS_LOST` 상태, GPS 복귀 뒤 치환 없음을 확인합니다.
속도만 바꾼 송신(choice 4)마다 원본 mode 1/2이고 위치 시각 utc 0(NO_FIX), 바뀐 바이트가 32와 36~39뿐, `hasSpeed=1`, 앞선 `NO_FIX`/`SPEED_ENGAGED` 상태, 속도가 송신 전 0.5초 안에 기록된 바퀴 속도 중 하나와 1 mm/s 안에서 같은지(바퀴 기록이 없으면 0~100 m/s 범위만)를 확인합니다. 다른 위치 상태에서 바꾼 송신은 모두 `violation`입니다.
요약에는 `speed_overlay_sends`, 그중 결과가 0이 아닌 수, `NO_FIX`/`SPEED_ENGAGED` 상태별 시간이 나옵니다.
핵심 측정은 다음 줄입니다. GPS 복귀 때 마지막으로 보낸 DR 위치와 첫 GPS 위치의 거리(시간 차를 속도·방위로 맞춘 값 포함)를 그때 보고한 정확도와 비교합니다.

```
BETA GPS return: last DR vs first GPS fix 31.6 m (time-aligned 30.0 m, gap 1.0 s), reported accuracy 12.0 m -> EXCEEDS
```

GPS는 참값이 아니며, 하위 송신 결과 0은 폰이 그 값을 채택했다는 뜻이 아닙니다.
요 영점 자료가 있으면 같은 출력 끝에 `Yaw zero data (diagnostic; …)` 절이 나옵니다: 정차 표(평균, 직전 정차 대비 변화, 부팅 뒤 시각), 10초 요약으로 맞춘 주행 중 영점(GPS 지연 0초와 1.3초), GPS 끊김·복귀 전후 비교, 재초기화 사건. 진단 자료이며 센서 검증이 아닙니다.

## 이 절차가 검증하지 못한 것

- 이 문서의 화면은 순정 BusyBox 에뮬레이션에서 제품이 쓴 기록으로 만든 것이며 실제 CMU에서 실행하지 않았습니다.
- DHU 시험 5([기록](../validation/DHU_NAVER_EXP5_2026-10-05/README.md))는 저장 구간의 속도 덮어쓰기 형태를 네이버가 끊김 없이 받아들임을 보였지만 단일 회차·10초 표본이며 Mazda 형식, 무선 동글, 실제 차속은 시험하지 않았습니다.
- 지하주차장 방위(G2)는 이 구현으로 달성되지 않습니다. 예산이 앵커에서 흐르므로 직진 50 km/h에서 14초, 30 km/h에서 21초에 40 m에 닿고, 2026-10-04의 실제 주차장 진입 단절 재생에서는 치환이 0건이었습니다.
- BETA 치환이 실제 Mazda wire와 네이버에서 속도·방위로 반영되는지, 터널에서 순정 mode 0이 얼마나 이어지는지, `libpatch`와 함께 설치된 후킹이 실차에서 터치·HUD에 영향이 없는지는 모릅니다.
- 세션 관측이 켜진(`FENCE observed`) BETA는 런타임 시험이 없습니다.
- 1분, 2분, 50분 같은 숫자는 측정값이 아니라 이 절차가 정한 기준입니다.
- 이 시험은 v1.0 완료나 자격 ASSIST 활성화를 승인하지 않습니다.
