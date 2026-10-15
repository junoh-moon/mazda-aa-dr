# 휴대폰 IMU 채널 1단계(기록 전용), CMU 쪽 구현 (2026-10-14)

대상: 1세대 Mazda Connect **NA 74.00.324A**, SHADOW/BETA 설치. **차량·폰·동글·DHU는 사용하지 않았습니다.** 이 기록은 차량 방문, 페어링, 주행, 발행, 자격 ASSIST를 승인하지 않습니다. 폰 앱(`phone/imu-sender/`)은 다른 구현자의 몫이며 이 변경에 들어 있지 않습니다. 펌웨어 사실의 출처는 [Fable 타당성 기록](PHONE_IMU_FEASIBILITY_FABLE_2026-10-13.md)과 [Opus 타당성 기록](PHONE_IMU_FEASIBILITY_2026-10-13.md)이며, 이 작업에서 새로 확인한 것은 DATA로 표시합니다.

**1단계의 불변식: 제품이 보내거나 결정하는 것은 하나도 바뀌지 않습니다.** DR은 계속 차량 0x078 yaw를 씁니다. 폰 자이로는 collector가 별도 파일(`logs/pimu.N.jsonl`)에 기록만 하고 runtime·worker는 읽지 않습니다. 바뀌는 순정 동작은 trial 부팅에서 `bds`가 우리 설정 사본으로 serialPort 서비스 하나를 더 광고한다는 것 하나이며, 이것이 아래 8절의 AGENTS.md 예외가 필요한 이유입니다.

## 1. 공유 규격 (원문 그대로)

```
SHARED SPEC 'PHONE IMU CHANNEL v1' (both implementers MUST follow it byte for byte; any change must be agreed through the lead). Goal of stage 1: RECORD-ONLY. The phone sends its gyro, the CMU records it next to the car's own yaw; NOTHING the product sends or decides changes (the dead reckoning keeps using 0x078). Stage 2 (later, not now) may switch the DR input.
TRANSPORT: classic Bluetooth RFCOMM. The CMU (stock bds daemon, BlueGo stack) offers a serial-port service with the 128-bit UUID fae57779-63aa-4424-bf71-66ce5951b0be and the service name 'MX5DR IMU'; the PHONE is the RFCOMM client (createRfcommSocketToServiceRecord(UUID), secure; the CMU and the phone are already bonded for hands-free). Byte order little-endian everywhere. CRC = CRC-32 (IEEE, zlib crc32) over all preceding bytes of the frame.
PHONE -> CMU, DATA frame 'MX', 32 bytes, sent every 20 ms (50 Hz): off 0 magic 0x4D 0x58 ('MX'); 2 version = 1; 3 flags (bit0 bias_valid: the OS bias estimate is valid; bit1 stationary: the phone's own stationarity hint; bit2 gravity_valid; bit3 frame_reset: first frame after (re)connection or after a sensor restart; others 0); 4 seq u32 (increments by 1 per data frame, wraps); 8 t_end_ns i64 = phone SystemClock.elapsedRealtimeNanos-based SensorEvent.timestamp of the LAST gyro sample in the window; 16 dtheta_urad i32 = rotation about the gravity axis integrated over the window, in microradians, CLOCKWISE POSITIVE as seen from above (heading increasing), i.e. sum over samples of (omega . g_down_unit) * dt with omega from TYPE_GYROSCOPE (calibrated, rad/s, phone axes) and g_down_unit = -gravity/|gravity| from TYPE_GRAVITY (Android's TYPE_GRAVITY points UP: a flat phone reads +9.81 on z; the unit vector pointing DOWN is its negation; latest value); 20 window_us u32 = t_end - t_start of the window in microseconds (t_start = timestamp of the sample preceding the first sample in the window); 24 bias_urad_s i32 = the OS gyro bias estimate (TYPE_GYROSCOPE_UNCALIBRATED values[3..5]) projected on g_down_unit, microradians per second (0 if bias_valid is 0); 28 crc32 u32 over bytes 0..27. A window with no new gyro sample is not sent (seq does not advance).
TIME SYNC: the CMU sends a PING frame 'MP' every 5 s and the phone answers immediately with a PONG; both 28 bytes: off 0 magic 0x4D 0x50 ('MP'); 2 version = 1; 3 type (0 = ping from the CMU, 1 = pong from the phone); 4 seq u32 (the pong copies the ping's seq); 8 t_cmu_ns i64 (CMU CLOCK_MONOTONIC ns at send; the pong copies it unchanged); 16 t_phone_ns i64 (0 in a ping; in the pong the phone's elapsedRealtimeNanos at reception); 24 crc32 u32 over bytes 0..23. The CMU estimates offset = t_phone - (t_cmu_send + rtt/2) and journals rtt and offset.
STREAM RULES: the receiver resynchronises on the magic bytes and CRC (drop bad frames and count them); unknown versions are counted and dropped; the phone never sends anything else; the CMU never sends anything except PING; either side closes and reconnects on 10 s of silence (phone: reconnect with backoff 2 s .. 30 s while the CMU device is connected).
GOLDEN VECTORS: the CMU implementer creates tests/phone_imu/golden_frames.json (a list of {hex, fields}) with at least 10 data frames (including negative dtheta, wrap of seq, flags combinations) and 4 ping/pong frames, plus 3 corrupt frames; the phone implementer must decode/encode the same file in its tests. Until the CMU implementer's file exists the phone implementer writes its own copy at phone/imu-sender/tests/golden_frames.json with the same schema; the lead reconciles them (they must be byte-identical in the end).
```

**규격 추가(2026-10-15, 독립 검토 2건 반영, 리드 승인 대상):** "The phone sends no DATA frame before the first PING of the session; while waiting for it the phone keeps the socket open at most 15 s, then closes (this counts as a failure for its circuit breaker)." 폰 쪽 회로 차단기와 CMU 쪽 부팅 내 정지(3a절)가 함께 들어갑니다. 공유 규격 파일의 문구 갱신은 리드의 몫입니다.

**규격 정정(2026-10-14, 리드 통보):** 처음 문구는 `g_down_unit = gravity/|gravity|`였으나 Android `TYPE_GRAVITY`는 위쪽을 가리키므로 위 원문(현재 공유 규격)은 `-gravity/|gravity|`입니다. 폰 앱은 처음부터 이 부호로 구현했습니다. CMU 수신기는 dθ를 해석하지 않고 기록만 하므로 CMU 코드에는 영향이 없습니다.

골든 벡터는 **하나의 정본** `tests/phone_imu/golden_frames.json`(36개: 데이터 20, ping/pong 9, 손상 7)이고 생성기도 하나(`tests/phone_imu/make_golden.py`, 결정적, `struct`+`zlib.crc32`로 C++·Java와 독립)입니다. 형식은 `{"name","kind","hex","fields"}` 목록이며 kind는 data/ping/pong/corrupt, 손상 항목의 fields는 `{"expect": "crc_error"|"unknown_version"|"bad_type"}`입니다. 폰 구현자의 21개 항목은 바이트 그대로 들어 있고 CMU 쪽 15개를 더했습니다. 폰 쪽 사본과 생성기는 지웠고 `phone/imu-sender/test.sh`가 정본을 `--check`로 재생성 대조한 뒤 JVM 시험과 독립 Python 디코더(`e2e_check.py golden`)에 같은 파일을 넘깁니다. CMU 디코더도 같은 36개를 해석하며, 형식 1의 MP 프레임 중 type이 0/1이 아닌 것은 `bad_type`으로 세고 버리도록 맞췄습니다.

## 2. 구현 요약

| 부분 | 파일 | 내용 |
|---|---|---|
| Bluetooth 서비스 선언 | `packaging/edit_bds.awk`, `common.sh`, `install.sh`, `bt.sha256` | 메뉴 1(SHADOW/BETA)이 순정 `/jci/bds/BdsConfiguration.xml`에서 `BASE/bt/BdsConfiguration.xml`(+`.sha256`)을 만들고, 가드 trial 템플릿(`normal.trial`, `wcp.trial`)의 `bds` args에만 ` -configFilePath /data_persist/mx5-aa-dr/bt/BdsConfiguration.xml`을 붙입니다. 순정 `sm.conf`, `sm_WCP.conf`, `/jci/bds`는 바꾸지 않습니다. |
| 가드 결정 | `src/guard/guard.cpp` | 선택마다 스테이징 사본에서 그 인수를 지우거나 둡니다(아래 4절). 결정은 경로 공개 전에 `guard/bt-last`에 남고, 제품 선택 자체를 거절하는 일은 없습니다. |
| 수신기 | `src/collector/phone_imu.h`, `phone_imu_rx.h`, `collector.cpp` | collector의 스레드 하나. bds 감시, PTY 경로 획득, 항상 읽기, 프레임 해석, PING, 1 Hz 집계, `pimu.N.jsonl`. |
| 메뉴 | `packaging/trial`, `trial_status.sh` | 메뉴 11(bt-off 켜기/끄기), 메뉴 2의 `bt_marker`, `bt_fail`, `bt_config`, `bds_args` 줄과 화면 요약. |
| 분석 | `tools/analyze_logs.py`(`phone_imu` 절), `tools/pimu_eval.py` | pimu 파일이 있을 때만 절이 생깁니다(이전 로그의 보고서는 그대로). |

### 2.1 추가한 serialPort 항목과 속성별 이유

`<serialPort id="15" name="MX5DR IMU" noOfInstances="1" critical="false" enabled="true" uuidServer="FAE5777963AA4424BF7166CE5951B0BE" uuidClient="FAE5777963AA4424BF7166CE5951B0BE" writeDelay="3"/>` 한 줄을 `</serviceConfiguration>` 바로 앞, 마지막 순정 serialPort와 같은 들여쓰기로 넣습니다.

- `id="15"`: 순정 id는 1~12와 14입니다. 13은 BCA 설정의 "serial service 1"과 겹칠 수 있어 피했습니다(FACT: `blm_bca-system.xml`의 serialService1UUID는 BDS 설정에 없음; INFERENCE: 예약 가능성). 15는 Fable이 원본 `bds`로 파싱을 확인한 값입니다.
- `name="MX5DR IMU"`: 규격의 서비스 이름. BCA는 이름으로 8종만 분류하므로 이 이름은 분류되지 않습니다(FACT, `BCA_fillServiceInfo`). 순정 소비자와 엮이지 않게 하려는 것입니다.
- `uuidServer`/`uuidClient`: 규격 UUID를 순정 항목과 같은 형식(하이픈 없는 32자리 대문자)으로 둡니다. 순정 항목의 문자열은 표준 UUID 순서입니다(DATA: AHA 항목 `143DD5D0…11E0…`은 버전 1 UUID의 표준 배열, Phone Gateway는 ASCII "JCI PhoneGateway"). Pandora·Stitcher·AHA·Android VDT처럼 서버와 클라이언트를 같게 둡니다.
- `noOfInstances="1"`: 폰 한 대. 인스턴스마다 RFCOMM 채널과 PTY를 쓰므로 순정 주석의 권고(1~3)대로 최소로 둡니다.
- `critical="false"`: 다른 비필수 serialPort와 같습니다. 이 서비스 실패가 필수 서비스 판단에 들어가지 않게 합니다.
- `enabled="true"`: 꺼진 서비스는 광고되지 않습니다.
- `writeDelay="3"`: 모든 순정 serialPort와 같은 값(이벤트마다 3 ms 대기, 상한 약 319 이벤트/s; 50 Hz는 그 16 %)입니다.
- `securityLevel`은 순정 serialPort에 없으므로 넣지 않습니다.

편집기는 폐쇄형입니다. CRLF, serviceConfiguration 안의 주석이나 모르는 요소, 블록 누락·중복, 여러 줄 태그, id 15나 우리 이름·UUID가 이미 있음, 중복 id, serialPort 20개 이상(bds 상한), 순정 속성 집합이 없는 serialPort를 모두 거절합니다. 설치기는 출력이 입력보다 정확히 한 줄 많고 그 줄을 빼면 입력과 바이트가 같은지 다시 확인합니다. args 편집기는 `type="process"`, `path="/jci/bds/bds"`인 `bds` 서비스가 정확히 하나이고 args가 `-interface dbus` 또는 `-interface dbus -wcp`일 때만 붙이고, 다른 모양은 거절합니다. 순정 파일 두 개(`BdsConfiguration.xml` `33a2450c…`, `bds` `3afd74c7…`)의 해시가 `packaging/bt.sha256`과 다르면 서비스만 준비하지 않고 설치는 계속합니다.

## 3. 수신기 위치와 설계 결정

- **collector에 둡니다.** 패키지에서 D-Bus를 쓰는 유일한 프로세스이고 trial 부팅에서만 뜹니다. jciVBS 안의 worker에 넣으면 순정 프로세스 안에서 D-Bus·PTY I/O가 생기고, 1단계에서 제품 경로를 건드리지 않는다는 조건과 맞지 않습니다.
- **권한(DATA)**: 차량 내보내기의 `/proc/mounts`는 `devpts … mode=600`이고, SM 서비스는 uid 0으로 돕니다(내보내기의 `L_jciAAPA` 등 status). 따라서 `bds`가 연 PTY slave는 root 전용이고, `service` 계정으로 내려간 collector는 열 수 없습니다. collector는 `PR_SET_KEEPCAPS`로 `CAP_DAC_OVERRIDE` **하나만 permitted**에 남기고, 수신 스레드가 slave를 `open()`하는 순간에만 effective로 올렸다 내립니다. 주 스레드는 수신 스레드를 만든 뒤 자기 능력을 모두 버립니다. 연 장치는 문자 장치, major 136~143, 소유자 uid 0이어야 합니다.
- **경로**: BCA 신호 `ConnectionStatusResp(u,u,u,u,(ay))`(FACT: `svcjcibca.so` introspection)를 `com.jci.bca` 이름의 현재 소유자에게서 온 것만 받고, serviceId 15만 씁니다. 신호가 안 오는 경우(introspection의 `always_broadcast=false`) 연결되지 않은 동안 `QueryConnectStatus(15) → (iiii(ay))`를 5 s에서 시작해 두 배씩 60 s까지 늘려 부르고, 우리 서비스의 신호가 오면 5 s로 되돌립니다(순정 BCA로 가는 메서드 호출을 줄이려는 것: 신호가 주 경로이고 질의는 놓친 신호의 보완). **연결 상태 값도 봅니다**: `connStatus`가 CONNECTED일 때만 열고, CONNECTING은 상태 유지, 그 밖의 값은 우리 독자를 닫습니다. 값의 뜻은 `svcjcibca.so`의 고정 폭 이름표 순서(IDLE, CONNECTABLE, CONNECTING, CONNECTED, ABORTED, DISCONNECTING, DISCONNECTED, FAILED, TIMEOUT → CONNECTED=3)에서 얻은 **INFERENCE**입니다. 틀리면 서비스를 열지 않는 쪽으로 실패하며(기록 전용이라 안전), 모든 값이 `pimu_link` 행에 남아 주차 확인에서 드러납니다. 오래된 경로를 가진 IDLE 응답(`connStatus=0`)은 열지 않습니다. 다른 serviceId의 PTY는 순정 소비자의 데이터를 빼앗게 되므로 절대 열지 않고 개수만 셉니다. 경로는 `/dev/pts/<1~4자리>`만 받습니다. bds 프로세스는 argv[0]의 이름이 `bds`(SM은 `/jci/bds/bds`를 절대 경로로 실행; 다른 디렉터리 표기도 받음)이고 `-configFilePath` 바로 뒤가 우리 경로인 것으로 찾습니다. bds가 `system()`으로 cp/mkdir을 띄우는 순간의 자식(exec 전 같은 cmdline)을 bds로 오인하지 않도록, 같은 pid가 1 s 간격 두 번의 탐색에서 보여야 bds로 인정하고, 여럿이면 시작 시각(`/proc/<pid>/stat` 22번째)이 가장 이른 것을 고릅니다.
- **항상 읽기**: slave를 raw(`cfmakeraw`)로 바꾸고(원래 `lflag`를 행에 남김), `poll` 50 ms 주기로 한 번에 최대 64 KiB씩 읽습니다. 초당 16 KiB(정상 1.6 kB/s의 10배)를 넘는 바이트는 읽어서 버리고 셉니다. 행 쓰기는 collector 주 루프가 하므로 플래시 지연이 읽기를 멈추지 않습니다. 10 s 무신호면 같은 PTY를 **새 fd로 먼저 연 뒤** 옛 fd를 닫습니다(규격의 "닫고 다시 연결"; CMU는 폰에 다시 걸 수 없고 순정 BCA 호출도 하지 않으므로 이 해석을 택했습니다; 독자가 없는 순간을 만들지 않습니다). 마스터가 닫히면(EOF/EIO/HUP) 끊고 새 경로를 기다립니다. CMU가 보내는 것은 PING(5 s)뿐입니다.
- **시각**: 모든 행 시각은 CLOCK_MONOTONIC ms(`m`)이며 worker의 yaw 행과 같은 커널 시계입니다. 1 s 묶음은 CMU 수신 시각 기준입니다. pong마다 rtt와 offset = t_phone − (t_send + rtt/2)를 기록합니다.
- **행과 예산(beta.14 방식)**: `pimu_boot`, `pimu`(10 s마다 한 행에 1 s 묶음 10개 `[n, Σdθ µrad, Σwindow ms, seq gap, flags OR]`와 seq 처음/끝, 마지막 bias, CRC 오류, 버전 오류, 건너뛴 바이트, 버린 바이트, frame_reset 수, 마지막 t_end), `pimu_sync`, `pimu_link`, `bt_watch`, `pimu_drop`. 행 종류별 토큰 버킷(pimu 10 s/버스트 6, sync 5 s/2, link 10 s/6, bt 60 s/4, drop 60 s), 모든 숫자에 상한을 둡니다. **최악의 경우 377 KB/h**(`budget_bph()`, 단위 시험에서 400 KB/h 미만을 단언, 가장 긴 pimu 행 519 B)입니다. 파일은 1 MiB × 2이며 설치기 여유 공간 계산에 넣었습니다.

### 3a. 부팅 안의 회로 차단 (v2, 두 독립 검토의 차단 사항)

문제: bds가 우리 RFCOMM 서비스가 **쓰일 때만**(SDP/RFCOMM 단계, QEMU로 닿지 않음) 죽으면 SM은 같은 인수로 bds를 다시 띄우고(`retry_count=30`, `reset_board=yes`), 폰은 ACL_CONNECTED에 바로 다시 붙으며 연결되면 백오프가 2 s로 돌아가 주행 중 보드 리셋까지 반복될 수 있었습니다. `logs/bt-fail`과 가드의 `reset_after_bt`는 **다음** 부팅에만 듣습니다. 고친 점:

- **CMU(`phone_imu_rx.h` quiesce)**: 우리 설정으로 뜬 bds가 사라지거나(재시작, 인수 변경, 소멸) 다른 pid로 바뀌는 것을 처음 본 순간부터 **이 부팅 동안 PTY를 다시 열지 않고 PING도 보내지 않으며** 질의도 하지 않습니다(`bt_watch quiesce` 행, `pimu_link disconnect quiesce`). `bt-fail`도 그대로 씁니다(이름 바꾸기 뒤 logs 디렉터리 fsync 추가).
- **폰(`ReconnectPolicy` 회로 차단기, `Session`)**: 실패 = 연결 실패, PING 없이 끝난 세션, 연결 뒤 30 s 안에 끝났거나 ACL이 끊긴 세션. 백오프는 PING을 60 s 이상 받은 세션 뒤에만 2 s로 돌아갑니다. 연속 3회 실패면 60분 동안 시도하지 않고, 두 번째 차단이면 앱을 다시 열 때까지 멈춥니다(알림과 진단 기록에 표시). 차단 중이거나 실패가 남아 있으면 ACL_CONNECTED로 바로 다시 붙지 않습니다. 세션은 첫 PING 전에는 DATA를 보내지 않고 15 s 안에 PING이 없으면 닫습니다.
- **상한(DATA, `CoreTests.crashLoop`의 이산 사건 모의)**: 연결마다 bds가 죽고 SM 재시작과 ACL 복귀에 8 s가 걸리며 CMU가 첫 재시작 뒤 조용해지는 경우, 한 CMU 부팅 24시간 동안 폰이 일으키는 bds 크래시는 **앱 시작당 6번**(차단 2회 × 3실패)이고 그 뒤 잠깁니다. bds의 `retry_count=30`보다 작습니다. 대조로, CMU가 조용해지지 않고 매번 PING을 40 s 받은 뒤 죽는 경우에는 폰 혼자로는 상한이 생기지 않음을 같은 시험이 보여 줍니다(그래서 양쪽이 모두 필요합니다). CMU 쪽은 호스트 PTY·가짜 BCA·가짜 /proc으로 "연결 → bds 재시작 → 폰 재연결 3회"를 돌려, 재시작 뒤 새 PTY를 한 번도 열지 않고 PING 0바이트임을 확인했습니다.
- **남는 틈**: bds가 설정을 읽은 직후(서비스 사용 전) 죽는 경우는 collector가 알아채기 전에 SM 재시작이 반복될 수 있고, 다음 부팅부터는 가드의 리셋 규칙과 미확인 부팅 규칙(3회)에 기댑니다. 그 단계는 QEMU 파싱 검사(일반·WCP 인수)가 덮는 부분입니다.

## 4. 대체 경로(fallback chain)

1. **설치 시점**: 순정 해시가 다르면 서비스를 준비하지 않습니다. 편집기가 모르는 모양을 만나면 설치를 거절합니다(autostart 변경 전).
2. **매 선택(가드)**: 다음 중 하나라도 있으면 스테이징 사본에서 인수를 지워 **순정 bds 인수**로 띄웁니다: `bt-off`(메뉴 11, 어떤 종류든 존재하면), `guard/bt-fail`, `logs/bt-fail`(logs를 열 수 없어도 있다고 봄), XML과 `.sha256` 불일치(형식 오류, 그룹·기타 쓰기 권한 포함), 템플릿에 인수가 둘 이상. `bt-last` 기록이 실패하면 인수를 지웁니다.
3. **같은 부팅(collector)**: 우리 인수로 뜬 bds가 60 s 안에 `/tmp/bds_ready_flag`를 만들지 않거나, 다른 pid로 다시 뜨거나(SM 재시작), 인수가 바뀌거나, 30 s 넘게 사라지면 `logs/bt-fail`(사유 `not_ready`/`restart`/`args_changed`/`vanished`)을 씁니다. bds가 사라진 것을 처음 본 순간부터 이 부팅의 수신을 멈춥니다(3a절). 폰은 자기 회로 차단기로 재연결을 멈춥니다.
4. **다음 부팅(가드)**: 앞 시도가 이 서비스를 썼고 그 부팅 뒤 SM 리셋 보고(`/data/*.out`)가 바뀌었으면 `guard/bt-fail`(`reset_after_bt`)을 씁니다. 제품 자체는 기존 규칙대로 계속됩니다.
5. **기존 규칙**: 미확인 부팅 3회, 리셋 2회 연속, 최근 10회 중 리셋 3회면 제품 전체가 순정으로 돌아갑니다(BT 인수도 함께 사라짐).
6. **복구**: 메뉴 1이 `bt-off`, 두 `bt-fail`을 지우고(기록은 `backups/`에 복사) 다시 켭니다. 메뉴 4가 `BASE/bt`를 지우고, 메뉴 6이 패키지 전체를 지웁니다(`bds`의 cmdline에 패키지 경로가 있으면 메뉴 6은 사용 중으로 거절하므로 4 → 5 → 6 순서).

## 5. 오프라인 검증

- **원본 bds 파싱(필수 릴리즈 단계, `tests/packaging/test_bds_parse_qemu.py`)**: 순정 루트의 원본 `bds`를 qemu-arm 8.2.2로 `-interface dbus -configFilePath <생성 XML>`, `-strace`, 인수를 기록하는 `cp`/`mkdir` 대역으로 실행했습니다. 임시 루트는 순정 트리를 심볼릭 링크로만 보이고 `tmp`, `data`, `config`는 시험 전용 디렉터리이며, 쓰기 모드 open이 순정 경로를 가리키지 않는지도 검사합니다. 우리 ARM 프로브(`bds_parse_probe.c`, `open()`만 가로채고 bds 코드는 호출하지 않음)가 라디오 UART를 열 때 내보낸 전역 `BDS_serialPortCount`와 `BDS_SerialPortList`의 id를 읽었습니다. 결과(DATA): `cp -vf <우리 XML> /tmp/BdsConfiguration.xml` 실행, `/tmp/BdsConfiguration.xml` 두 번 열기, 신호 없음, **`radio_open count=8 ids=5,8,9,10,11,12,14,15`**, 종료 1(라디오 없음의 정상 실패). 대조군: 순정 XML `count=7 ids=5,8,9,10,11,12,14`, 닫는 태그가 깨진 XML은 라디오 전에 SIGSEGV(검출됨), 21개 항목은 `count=20`. 순정 BusyBox 1.19.2 awk(qemu-arm)가 호스트 awk와 같은 XML과 trial 편집을 만들고, 두 번째 추가를 거절합니다. 목록 항목의 배치(8바이트, 첫 단어가 id)는 순정 대조군으로 확인한 관찰(DATA)이며 디컴파일하지 않았습니다. 처음 시도한 `BDS_SerialPortConfiguration_Add` 가로채기는 인수 배치를 모르는 전달 때문에 bds 동작을 바꿔(1회 추가 후 종료 0) 버렸습니다.
- **프레임**: 정본 골든 36개를 C++ 디코더가 해석하고 다시 부호화해 같은 hex를 만듭니다. 무작위 쓰레기와 무작위 분할 20회에서 모든 정상 프레임이 그대로 나오고 CRC·버전·type 오류가 계수됩니다. 폰 쪽 JVM 시험과 독립 Python 디코더도 같은 36개를 모두 같은 결과로 해석합니다(`make test-phone-imu-sender`). 단위 시험: CRC-32 검사값, 바이트 단위 재동기, seq 랩 간격, frame_reset, 행 시계, 빈 행 생략, ping/pong offset 계산, 짝 없는 pong 거절, 토큰 버킷, 행 최대 길이와 시간당 예산.
- **수신기(호스트 PTY 쌍, 실제 dbus-daemon, 가짜 BCA)**: 시험용 collector 빌드에 대해 cooked PTY에서 raw 전환, 50 Hz 450프레임, 쓰레기와 가짜 magic, CRC 손상, 분할 쓰기, seq 공백 3, PING/PONG(offset 오차 < 60 ms, 루프백), 100 kB 과부하(쓰기 쪽이 막히지 않음, 초과분 계수), 10 s 무신호 후 같은 경로 재열기, 마스터 닫힘 후 끊김과 신호로 새 PTY 연결, 합계 dθ와 프레임 수가 보낸 값과 정확히 일치. 다른 송신자의 신호와 다른 serviceId 신호는 열지 않음, 잘못된 경로 거절, `not_ready`와 `restart`의 `logs/bt-fail`, 순정 인수 bds에서는 D-Bus 질의 없음, XML 없음·bt-off·bt-fail·OBSERVE에서는 pimu 파일 자체가 생기지 않음.
- **가드**: 지속 정책 9건과 1회 정책 1건(`tests/recovery/test_guard_bt.py`): 유효한 경우 인수 유지, bt-off(파일·디렉터리·링크), logs/bt-fail, logs 디렉터리 없음, XML·digest 이상 6종, guard/bt-fail, BT를 쓴 부팅 뒤 리셋 → `reset_after_bt`(제품은 fail_count 1로 계속), BT를 쓰지 않은 부팅 뒤 리셋 → 기록 없음, 인수 없는 템플릿은 바이트 그대로, 인수 중복은 모두 제거, `bt-last` fsync 실패 시 인수 제거. 기존 가드 시험 37+42건은 그대로 통과.
- **설치기·편집기·메뉴(`tests/packaging/test_phone_imu_install.py`, 호스트 sh/awk와 호스트 BusyBox awk)**: 생성, 멱등, 모르는 모양 13종 거절(19개 serialPort는 허용, 20개는 거절), args 편집기 추가·제거·거절 7종, SHADOW만/OBSERVE 전환 시 제거, 해시 불일치 시 서비스만 생략, 모르는 순정 모양이면 autostart 변경 전 거절, 메뉴 1의 bt-off·bt-fail 해제, 메뉴 4의 제거, 실제 템플릿과 가드 빌드로 켜짐 → bt-off → collector bt-fail → 메뉴 1 복구의 전체 사슬과 메뉴 2 표시, 메뉴 11(40열, 숫자 확인, 디렉터리·링크 표식).
- **분석기·평가기(`tests/tools/test_pimu_eval.py`)**: 알려진 차량 영점 오차와 폰 편향을 넣은 합성 부팅에서 GPS 상실 구간과 60 s 창의 방위 오차를 각각 되찾고, 폰 묶음이 빠진 창은 채우지 않고 "missing"으로 셉니다. pimu 파일이 없는 로그의 보고서에는 `phone_imu` 키가 없습니다. 비공개 내보내기 1개에 평가기를 돌려(차량 쪽만) 111 s 터널의 차량 방위 오차 −23.1°를 얻었고, 이는 [YAW_DEADBAND](YAW_DEADBAND_2026-10-13.md)의 −22.1~−24.7°와 맞습니다(DATA, 숫자만 옮김).
- **불변 증명**: 비공개 17개 부팅을 beta.15 기록과 같은 방식(하나의 G3 저장소로 연쇄, `--real-only --g3 on --g3-rt`)으로 기준 빌드와 이 변경의 `replay_beta`로 재생해 두 `replay_beta` 바이너리가 `cmp`로 같았고, 119개 출력 파일이 G3 저장 스레드의 벽시계 필드(`lat_ms`, `start_ns`, `end_ns`)만 정규화하면 모두 같았습니다(정규화 없이 112개 동일, 7개 `.g3.jsonl`은 그 벽시계 필드만 다름; beta.15 기록과 같은 성질; 폰 앱 통합 뒤 다시 실행해 같은 결과). 이 재생에는 폰 입력이 없고, 변경은 재생이 링크하는 파일(`src/runtime`, `src/navigation`, `src/adapter`, `src/core`, `tools/replay_beta.cpp`)을 하나도 건드리지 않습니다. 수신기는 collector에만 있으며 SHADOW/BETA trial 부팅에서 bds가 우리 인수로 보일 때만 움직입니다.
- **전체 호스트 시험(v2, 검토 반영 뒤 다시 실행)**: `make -k test`(HOST_DBUS 변수, `MX5DR_STOCK_ROOT`는 순정 트리를 작업 공간에 복사한 사본) 종료 0: build-deps 47, motion 209, recovery 37+42+10, loader 1, collector 29(수신기 13건 포함), packaging 399(SKIP 4: 릴리즈 묶음이 필요한 기존 시험; 원본 bds 파싱 6건은 실행됨, 일반·WCP 인수 모두 `count=8 ids=5,8,9,10,11,12,14,15; exit 1`), tools 253(SKIP 2), replay 25(SKIP 2), C 시험 `PASS` 줄 445. `make test-phone-imu-sender` 종료 0(정본 골든 36개, JVM core 423 검사, Java 부호화→독립 Python 해석 134프레임·pong 6, 첫 프레임이 PONG). 파싱 검사는 SKIP이면 릴리즈를 멈춥니다(`docs/RELEASING_KO.md`; 고정 컴파일러는 `CROSS_COMPILE` 또는 `tools/m3-toolchain`).
- **v2 수신기 시험 추가**: IDLE(`connStatus=0`)과 유효한 오래된 경로는 열지 않음, CONNECTING은 대기, DISCONNECTED는 닫음; 질의 간격 5→10→20 s(25 s 동안 3회)와 신호로 5 s 복귀; bds 재시작 뒤 quiesce(새 PTY 3번 모두 열지 않음, PING 0바이트); vanished와 args_changed; 1 s 미만의 bds 모양 프로세스는 bds로 보지 않음, 상대 argv[0] `bds`와 오래된 쪽 선택; /dev/null(major 1, uid 0)과 다른 소유자의 PTY 거절; 10 s 무신호 재열기 동안 PTY 마스터에 HUP 0회(재열기 전 400 ms 시험 지연); 메뉴 2의 `bds_args=with_config/stock/not_running`(가짜 /proc).
- **ARM**: `mx5dr-collector`와 `mx5dr-guard`를 고정 도구체인(GCC 4.9.1, softfp)으로 컴파일·링크했습니다. collector는 GLIBC_2.4만 요구하고 의존성은 기존과 같습니다(libdbus-1, pthread, rt). ARM 전체 파이프라인(`tests/run_arm_all.sh`)과 `cmu_emulation.py`(루트·chroot·binfmt 필요)는 실행하지 않았습니다. ARM collector의 능력(capability) 처리 경로는 root가 필요해 실행하지 못했습니다(미검증).
- **손 변이 v2**: 검토 항목 17개 모두 잡혔습니다: PTY major 확인 제거, 소유자 확인 제거, vanished 규칙 제거, args_changed를 restart로, 재열기 전 옛 독자 닫기, quiesce 제거, connStatus 무시, 고정 5 s 질의, 확인 없이 후보 채택, argv[0]을 `/jci/bds/bds`로 고정, 메뉴 2 with_config 실패; 폰: 차단기 미작동, 차단 중 ACL 즉시 재시도, 모든 PING 세션에서 백오프 초기화, 두 번째 차단 미잠금, 첫 PING 전 DATA, 첫 PING 무한 대기. logs 디렉터리 fsync는 호스트에서 관찰할 수 없어 변이 시험이 없습니다.
- **손 변이(v1, v2 코드에서 다시 실행)**: 13개 모두 잡혔습니다: 가드의 bt-off 무시, 리셋 규칙 제거, logs 확인 실패 시 통과, XML digest 생략; 디코더의 CRC 무시, seq 랩 산술 제거; 수신기의 raw 전환 제거, 과부하 버림 제거, 재시작 감시 제거, 송신자 확인 제거; XML 편집기의 id 15 허용; 설치기의 잘못된 digest; 메뉴 1의 bt-fail 유지.

## 5a. 폰 앱(`phone/imu-sender/`, 폰 구현자 보고의 요약)

- connectedDevice 포그라운드 서비스, 보안 RFCOMM 클라이언트(`createRfcommSocketToServiceRecord(UUID)`), 재연결 백오프 2~30 s, `ACL_CONNECTED`를 받으면 즉시 재시도.
- 센서는 연결된 동안에만 켭니다: 자이로 5000 µs, 중력 10 ms, 보정 전 자이로(편향) 100 ms. PONG은 수신 스레드가 바로 답합니다. 10 s 무신호면 다시 연결합니다. 앱 전용 로그 1 MiB.
- 권한: BLUETOOTH_CONNECT, FOREGROUND_SERVICE, FOREGROUND_SERVICE_CONNECTED_DEVICE, POST_NOTIFICATIONS, WAKE_LOCK, RECEIVE_BOOT_COMPLETED, BLUETOOTH(maxSdk 30). 위치·인터넷 권한 없음.
- 호스트 시험: 폰 구현자 보고 273/0, 손 변이 10개 잡힘. 통합과 v2(회로 차단기, 첫 PING 전 DATA 금지) 뒤 이 저장소에서 `make test-phone-imu-sender`를 다시 실행했습니다(5절).
- v2 APK: 같은 키(`private-data/phone-signing/sender-debug.p12`, 저장소에 넣지 않음)로 서명, sha256 `f058d31f96d319b19e8f781aebacb9ba9f19d9189db66279b036db7965c83740`, 인증서 SHA-256 `5325914ab6e7b89b5649ee75c8a79a0162bf30603fb04525b939f8c2b1efe87c`(stage1 APK와 같음). 기기 설치·실행은 하지 않았습니다.
- 미검증: Samsung이 프로세스를 죽인 뒤 서비스를 다시 띄우는지, 실제 폰·CMU 사이 RFCOMM 전체.

## 6. 위험

- **bds 크래시와 보드 리셋**: 설정 파싱 실패는 SIGSEGV이고 `bds`는 `reset_board="yes"`입니다. 완화: 폐쇄형 편집기, 순정 해시 고정, 매 릴리즈 원본 bds 파싱 검사, 매 선택 digest 확인, collector의 준비·재시작 감시, 리셋 뒤 `guard/bt-fail`, 기존 미확인 부팅 규칙. BlueGo가 추가 서비스를 SDP/RFCOMM에 등록하는 단계는 qemu에서 재현되지 않습니다(라디오 없음).
- **순정 bds 재시작을 우리 탓으로 보는 오판**: 어떤 이유든 같은 부팅에서 bds가 다시 뜨면 BT를 끕니다(보수적; 메뉴 1로 복구).
- **무선 AA 동글·핸즈프리 공존**: 폰은 동글(BT+Wi-Fi)과 CMU(HFP)에 이미 붙어 있습니다(Opus 기록의 OEM 로그). RFCOMM 하나를 더 여는 것이 HFP·AA 재연결과 경쟁하는지는 차량에서만 알 수 있습니다.
- **페어링**: CMU와 폰은 핸즈프리용으로 본딩돼 있다고 들었으나, 보안 0x22의 의미와 새 서비스에 대한 승인 경로(자동 허용 여부)는 미해석입니다. 본딩이 없거나 승인이 필요하면 CMU 화면 확인이 한 번 필요할 수 있습니다.
- **serviceId 대응**: BCA가 이 항목에 신호를 내는지, BCA의 serviceId가 BDS id 15와 같은지 확정하지 못했습니다. 다르면 수신기는 연결하지 않고 `pimu_link` poll 행과 `pimu_drop`의 다른-서비스 신호 수만 남깁니다(다른 서비스 PTY는 절대 열지 않음).
- **raw 전환 전의 짧은 틈**: 연결 직후 우리가 `cfmakeraw`를 적용하기 전에 도착한 바이트는 기본(cooked·ECHO) 설정으로 처리될 수 있습니다. 폰이 첫 PING 전에는 DATA를 보내지 않으므로(3a절) 이 틈에 오는 것은 사실상 없습니다.
- **BCA의 상태 보존**: BCA가 서비스 15의 연결 상태를 부팅 사이에 저장하는지(예: 자동 재연결 대상으로 기억하는지)는 모릅니다.
- **폰 진단 기록의 BT 주소**: 내보낸 폰 기록에 차량 Bluetooth 주소가 들어 있습니다(폰 README에 주의 문구).
- **PTY 쪽**: 우리가 slave를 raw로 바꿉니다. bds의 마스터 쪽 동작은 바뀌지 않지만, bds 자신이 slave 설정을 가정하는지는 모릅니다. 수신기가 멈추면 bds 쓰기가 막힐 수 있습니다(Opus 기록의 PTY 실험); 그래서 읽기 스레드는 다른 일을 하지 않습니다.
- **권한**: collector가 `CAP_DAC_OVERRIDE`를 permitted에 갖고 있습니다(이전에는 능력 없음). open 직전에만 올립니다.
- **로그 예산**: pimu 최악 377 KB/h, 파일 2 MiB 상한.

## 7. 차량에서만 답할 수 있는 것 (주차 확인 1회, 이 기록은 요청하지 않음)

메뉴 1 → 5 → 2에서 `bds_args=with_config`, `bt_fail=absent`, `bt_watch ready`가 보이는가; 동글로 무선 AA를 연결한 상태에서 폰 앱이 UUID로 RFCOMM을 열면 `pimu_link connect`와 `pimu` 행(초당 약 50프레임), `pimu_sync`의 rtt가 생기는가; 그동안 AA와 핸즈프리가 그대로 동작하는가; 페어링 확인이 필요한가. 터널 효과(폰 편향)는 그 뒤의 정상 주행 로그를 `tools/pimu_eval.py`로 봅니다. 2단계(DR 입력 전환)는 이 자료가 나온 뒤 별도 작업입니다.

## 8. AGENTS.md 예외 문구 (제안; AGENTS.md는 고치지 않았습니다)

> - Phone IMU channel, stage 1 RECORD-ONLY (validation/PHONE_IMU_CHANNEL_2026-10-14.md): SHADOW/BETA guard trial copies (normal.trial, wcp.trial) may start the stock bds with `-configFilePath /data_persist/mx5-aa-dr/bt/BdsConfiguration.xml`, a copy of the stock /jci/bds/BdsConfiguration.xml (pinned in packaging/bt.sha256) with exactly one added serialPort entry (id 15, name 'MX5DR IMU', UUID fae57779-63aa-4424-bf71-66ce5951b0be, writeDelay 3). The stock sm.conf, sm_WCP.conf and /jci/bds are never edited. The guard removes the argument on every selection when bt-off (menu 11), guard/bt-fail or logs/bt-fail exists or the XML digest differs; the collector writes logs/bt-fail when that bds is not ready within 60 s or restarts. Every release must pass tests/packaging/test_bds_parse_qemu.py with the original bds (no SKIP). The collector may open only the PTY of that service, keeping CAP_DAC_OVERRIDE effective only for that open, and may send only PING frames; its pimu rows are never read by the product, which keeps using the car's 0x078 yaw. This does not authorize a vehicle visit, a pairing, a drive or stage 2.

## 9. 확정하지 못한 것

BCA 신호의 발생과 serviceId 대응, 보안 0x22와 자동 승인, BlueGo의 SDP/RFCOMM 등록과 채널 여유, TI1283 무선 지연, 동글 공존, S25 자이로 편향, ARM collector의 능력 경로, `cmu_emulation.py` 전체 기동(이 호스트에 루트·binfmt 없음).

작업 노트: `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/pimuc.sr8Mcx/` (재생 출력 `replay/`, 변이 `mut/`, 시험 로그 `maketest.log`).
