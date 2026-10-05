# BETA 차량 시험 한 번의 절차 — 초안 (2026-10-05)

**이 문서는 초안이며 절차입니다. 차량 방문이나 주행을 요청하는 것이 아닙니다.** BETA 묶음은 아직 릴리즈되지 않았고, 이 절차의 어떤 화면도 실제 CMU나 순정 BusyBox에서 실행해 보지 않았습니다.
아래 화면 인용은 저장소의 호스트 시험(`tests/packaging/test_trial_menu.py`, `test_trial_status.py`)이 합성 기록으로 만든 출력이며, 실제 값(번호, 시간, 개수)은 다릅니다.
릴리즈 전에 순정 BusyBox 에뮬레이션으로 다시 실행해 이 문서의 인용을 갱신해야 합니다. 구조와 하지 말 것은 [2026-10-04 절차](FIELD_PROCEDURE_2026-10-04_KO.md)와 같습니다.

## 이 시험이 하는 일

[문제 정의](PROBLEM_DEFINITION_KO.md)의 G1(터널에서 속도), G2(지하주차장에서 방위)를 실제로 확인하는 첫 시험입니다.
BETA는 SHADOW 기록을 그대로 하면서 순정 위치를 송신 시점에 세 가지로 나눠 다룹니다([설계](../validation/ASSIST_BETA_DESIGN_2026-10-05.md), [결정 갱신](../validation/BETA_DECISIONS_2026-10-05.md)).

- **GPS 끊김(LOST, 순정 mode 0)**: 그 LOCATION의 위경도·정확도·속도·방위를 차량 센서로 계산한 값으로 바꿔 보냅니다(상태 `GPS_LOST` → `ENGAGED`). 보고하는 정확도는 40 m를 넘지 않으며, 넘으면 순정으로 돌아갑니다.
- **부팅 후 fix 없음(NO_FIX, 순정 mode 1/2이고 시각 utc 0)**: 순정이 저장해 둔 옛 위치를 보내는 구간입니다. 위치·방위·정확도는 순정 그대로 두고 **속도만** 바퀴 속도로 덮어씁니다(상태 `NO_FIX` → `SPEED_ENGAGED`). 이 구간에서 방위(G2)는 고칠 수 없습니다.
- **GPS 정상(FIX)과 그 밖**: 순정을 그대로 보냅니다. GPS가 돌아오면 즉시 순정입니다.

의심이 있으면(센서 침묵, 송신 실패, 세션 변화) 스스로 순정으로 물러납니다.

한 번의 주행으로 얻을 것:

1. 단절 구간에서 BETA가 실제로 켜지고(`ENGAGED`) 바꾼 송신이 있는지, 그리고 왜 물러났는지. 시동 직후 fix가 없는 동안 속도 덮어쓰기(`SPEED_ENGAGED`)가 있었는지.
2. 네이버가 단절 구간에서 속도·방위를 따르는지(관찰은 동승자가 있을 때만; 없으면 기록으로 판단합니다).
3. GPS 복귀 때 마지막으로 보낸 DR 위치와 첫 GPS 위치의 거리(회수 후 분석 도구가 계산합니다).

운전 중에는 아무것도 조작하지 않습니다. BETA는 주행 중 끄는 방법이 없고 필요 없습니다. 이상이 있으면 스스로 순정으로 돌아갑니다.

## 하지 말 것

- 시동 OFF/ON으로 CMU 재부팅을 대신하지 마십시오. 재부팅은 메뉴 `5`로만 합니다.
- 설치 후 다시 `1`을 누르지 마십시오(회수 때도).
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
   1 Install BETA for the next boot
   ```

   `mode SHADOW`나 `mode OBSERVE`가 나오면 BETA 묶음이 아닙니다. 설치하지 말고 알려 주십시오. `can't open`이 나오면 `sda1`을 `sdb1`, `sdc1`로 바꿔 다시 입력하십시오.
3. `1`과 Enter. 끝날 때까지 기다리십시오. `Staged BETA for one guarded boot.`와 `Installation finished.`가 나와야 합니다. 없거나 오류가 나오면 `5`를 누르지 말고 사진을 남기십시오.
4. `5`와 Enter. `reboot_command_exit=0`이 나오면 시동 버튼을 누르지 말고 CMU 화면이 꺼졌다 켜질 때까지 기다리십시오.
5. 화면이 돌아오면 셸을 다시 열고 2번의 한 줄을 입력한 뒤 `2`와 Enter. 화면 맨 아래 판정 블록으로만 판단합니다. 정상이면 다음과 같은 모양입니다(합성 기록의 호스트 출력).

   ```
   ---- GO / NO-GO ----
   ok   BOOT  new boot
   ok   GUARD committed
   ok   ONCE  consumed
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
   - 그 밖의 `NO-GO` → 출발하지 마십시오. 사진을 남기고 3절의 `3`과 `4`를 하십시오. `NO BOOT same_boot`만은 `5`를 한 번 더 누르고 5분 뒤 `2`로 다시 확인합니다.
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

   이 단계의 `NO-GO`는 출발 판정용이라 회수 때는 무시합니다. 같은 내용이 USB의 `startup-result.txt`에 저장됩니다.
3. `3`과 Enter. `export_exit=0`이면 회수 성공입니다(2026-10-04 절차 S9와 같음). 상세 상태(BETA 줄 포함)가 `trial-result.txt`에 저장됩니다.
4. 셸을 다시 열고 `4`와 Enter로 제거하십시오. `Removed owned one-boot autostart blocks…` 줄이 나와야 합니다.
5. 시동을 꺼도 됩니다. 다음을 비공개로 보관하고 알려 주십시오: `mx5dr-logs-….tar`와 `.sha256`, `trial-result.txt`, `startup-result.txt`, `reboot-request.txt`, 2번의 사진, 동승자 메모가 있으면 그것도.

## 회수 뒤 분석 (PC)

`python3 analyze_logs.py mx5dr-logs-….tar`가 BETA 행을 검사합니다. 위치를 바꾼 송신(choice 3)마다 원본 mode 0, 정확도 0 초과 40 m 이하, 0~7·24~31바이트 원본 유지, `hasAccuracy=1`, 앞선 `ENGAGED`/`GPS_LOST` 상태, GPS 복귀 뒤 치환 없음을 확인합니다.
속도만 바꾼 송신(choice 4)마다 원본 mode 1/2이고 위치 시각 utc 0(NO_FIX), 바뀐 바이트가 32와 36~39뿐, `hasSpeed=1`, 앞선 `NO_FIX`/`SPEED_ENGAGED` 상태, 속도가 송신 전 0.5초 안에 기록된 바퀴 속도 중 하나와 1 mm/s 안에서 같은지(바퀴 기록이 없으면 0~100 m/s 범위만)를 확인합니다. 다른 위치 상태에서 바꾼 송신은 모두 `violation`입니다.
요약에는 `speed_overlay_sends`, 그중 결과가 0이 아닌 수, `NO_FIX`/`SPEED_ENGAGED` 상태별 시간이 나옵니다.
핵심 측정은 다음 줄입니다. GPS 복귀 때 마지막으로 보낸 DR 위치와 첫 GPS 위치의 거리(시간 차를 속도·방위로 맞춘 값 포함)를 그때 보고한 정확도와 비교합니다.

```
BETA GPS return: last DR vs first GPS fix 31.6 m (time-aligned 30.0 m, gap 1.0 s), reported accuracy 12.0 m -> EXCEEDS
```

GPS는 참값이 아니며, 하위 송신 결과 0은 폰이 그 값을 채택했다는 뜻이 아닙니다.

## 이 절차가 검증하지 못한 것

- 이 문서의 모든 화면은 호스트 합성 시험의 출력이며 순정 BusyBox와 실제 CMU에서 실행하지 않았습니다.
- BETA 치환이 실제 Mazda wire와 네이버에서 속도·방위로 반영되는지, 터널에서 순정 mode 0이 얼마나 이어지는지, `libpatch`와 함께 설치된 후킹이 실차에서 터치·HUD에 영향이 없는지는 모릅니다.
- 세션 관측이 켜진(`FENCE observed`) BETA는 런타임 시험이 없습니다.
- 1분, 2분, 50분 같은 숫자는 측정값이 아니라 이 절차가 정한 기준입니다.
- 이 시험은 v1.0 완료나 자격 ASSIST 활성화를 승인하지 않습니다.
