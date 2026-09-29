# 실제 센서 입력과 SHADOW 항법 — 2026-09-29

이 변경은 `v0.2.0-observe.2` 다음의 **Draft 기능 구현**이다. 배포한 OBSERVE 릴리즈에는 들어 있지 않다. 기존 #10의 오프라인 정책 비교와 별개로, 차량 센서 메시지를 받아 DR 코어에서 위치를 계산하는 실행 경로를 추가한다. 실차에서 실행하거나 네이버 지도가 계산 위치를 수용한 것은 아직 아니다.

## 연결한 경로

`VIM 원본 SPI → 기존 VBS의 VIMC 콜백 → 전용 비차단 datagram → AA worker의 시간 정렬 → 공통 DR 코어 → LOCATION 직렬화 미리보기 → 자동 로그`

`SHADOW`에서 계산한 위치·속도·방향과 48바이트 LOCATION 미리보기를 기록한다. 원래 AA LOCATION 송신은 그대로 한 번 호출한다. `OBSERVE`/`SCRUB`는 VBS 센서 후크를 설치하지 않는다. 검증된 정규화 입력에 대해서는 같은 파이프라인·코어가 기존 `core_bridge`와 AA 어댑터로 이어진다. 실제 센서 입력을 검증된 입력으로 승격하는 경로와 live ASSIST는 열지 않았다.

상세 ABI·파일 해시·정적 분석 경계는 [센서 수신 계약](SENSOR_SOURCE_CONTRACT_KO.md)에 있다.

## 펌웨어에서 확인한 수신 경로

대상은 **NA 74.00.324A**다. 제공된 업데이트에서 누락됐던 `libjcivim_api.so`, `libjcipcapi.so`, `libjcipc.so`, VBS 서비스까지 추가 추출하여 정적으로 분석했다. OEM 실행 파일을 실행하지 않았으며 원본·디스어셈블리 덤프는 공개 저장소에 넣지 않는다.

VIMC에는 여러 클라이언트에 SPI 메시지를 전달하는 API가 있다. 하지만 그 서버 송신은 blocking POSIX message queue를 사용한다. 새 클라이언트가 죽고 큐가 남으면 VIM fanout이 멈출 수 있으므로 **별도 구독자 추가는 채택하지 않았다**. TCP 7035도 단일 클라이언트라 추가 접속하지 않는다.

대신 `jciVBS`가 원래 수행하는 `VIMC_AddClient` 등록을 감싸 기존 콜백을 보존한다. 수신 shim은 원본 메시지를 복사하고 원래 콜백을 같은 인자·포인터로 한 번 호출한다. 신규 VIMC 클라이언트나 큐를 만들지 않으며 차량으로 명령을 보내지 않는다. 적용 대상·원본 라이브러리는 해시와 ABI로 제한한다.

수신 메시지는 ARM32에서 `{u32 id, u32 length, u8 *data}`이며 data는 SPI 원본의 +3 위치부터다. 다른 층인 VDT public enum과 혼동하면 안 된다. 특히 아래 `0x118`은 VIM의 후진등 신호이고, VDT의 같은 번호는 조명 관련 신호다.

| VIM ID | 수신 payload | 해석 |
| --- | --- | --- |
| 0x100 | +1부터 LE u16 휠 속도 4개 | 각 값 `raw × 0.01 − 100` km/h, 평균 후 `/3.6` |
| 0x116 | +1 LE u16 yaw 누적값, +3 count | count로 정수 나눗셈 후 각속도 변환 |
| 0x118 | +1 후진 상태 | 순정 소비자는 0/비0으로 해석. 구현은 실제 수신한 0/1만 모델 입력으로 허용 |

순정 sensor profile의 자이로 변환은 `yaw_clockwise_rad_s = −(mean − bias_raw) × 0.000658615`다. 명목 bias는 2047이다. 순정 코드가 분해능을 rad/count에서 deg/count로 변환하고 음수 역수를 calibration factor로 사용하는 경로, heading에 적분하고 sin/cos로 이동시키는 경로를 확인했다. 후진은 이동 부호만 바꾸며 yaw 부호를 다시 뒤집지 않는다. 이는 순정 프로파일의 해석이지 이 차량에서의 실제 bias·오차 보증이 아니다.

VIM의 TCP 경로는 CMU CLOCK_MONOTONIC 밀리초를 만들어 붙이지만, 이번 IPC 원본 payload에는 그 timestamp가 없다. callback에서 얻는 시각은 **수신 시각**이다. 물리 측정 시각으로 바꿔 부르지 않는다. TCP 경로의 signed 32-bit 초×1000 연산은 약 24.855일에 불연속을 만들지만 이 값을 이번 IPC 입력에서 사용하지 않는다. 파이프라인은 음수·역행·epoch 변경을 별도로 거부한다.

`/tmp/RvrseLmpReq`는 부팅 때 실제 입력 없이 1로 초기화되므로 입력으로 쓰지 않는다. TCP 후진 이벤트는 변화 때만 전송되지만 IPC는 같은 값의 SPI 이벤트도 전달한다. 실제 차량에서 SPI 후진 이벤트가 얼마나 자주 오는지는 미확인이다. 새 후진 이벤트 없이 250ms를 넘으면 모델도 중단한다. 휠/yaw가 계속 온다는 이유로 후진의 유효 기간을 연장하지 않는다.

## 계산과 차단 계약

- 빌린 payload 포인터를 보관하지 않고 내용을 즉시 복사한다. OEM 호출의 인자, 반환값, errno를 보존한다.
- IPC는 고정 64바이트 인코딩이며 포인터나 C++ 구조체를 전송하지 않는다. 커널 송신자 자격정보, 프로세스·epoch·관측 순번과 나이를 검사한다. sequence는 관측 순번이며 물리 producer sequence가 아니다.
- 입력은 고정 크기 큐에서 시간순으로 처리한다. yaw는 이전 수신과 현재 수신 사이의 평균이었다는 **모델 가정**으로, 끝난 구간에만 적용한다. 기본 holdback은 100ms다. 실제 지연이 이를 초과하면 중단하며 값을 미래로 외삽해 빈 구간을 채우지 않는다.
- 모델의 GPS 기준점은 연속된 두 개의 수치적으로 가능한 fix와 실제로 관측한 후진 상태를 요구한다. 수신 시각과 GPS heading을 body heading으로 연결하는 것도 가정으로 기록한다. mode=0에서 계산 위치를 진단 출력하고 GPS 복귀·native mode=3에서는 기존 계산의 권한을 즉시 철회한다.
- 정차 hysteresis, 회전·후진, 센서 단절, 늦은 메시지, 시간 역행, source 재시작, 큐 overflow, GPS 재획득을 공통 코어로 처리한다. 단절 이후에는 새 GPS 기준점을 요구한다.
- 기본 수치 제한은 60초·1,500m·모델 오차 예산 100m다. 정확도 보증이 아니며 장터널·지도 매칭은 범위 밖이다. speed-only DR은 허용하지 않는다.

공통 코어에 `MODEL` domain을 추가했다. MODEL anchor는 `validated`, `heading_valid`, `calibration_verified`를 false로 유지하며 센서 품질도 `MODEL`이다. MODEL snapshot의 `valid`는 항상 false이고 계산 성공은 별도 `model_valid`로 나타낸다. 일반 snapshot API와 `core_bridge`가 MODEL 출력을 거부하므로 외부 qualification flag를 true로 주어도 승격되지 않는다. 미리보기는 같은 LOCATION serializer를 쓰되 ready snapshot을 만들거나 publish하지 않는다.

검증된 입력용 파이프라인은 명시적인 source evidence·기준점·세션 context를 받는다. synthetic 테스트의 검증 조건은 테스트가 공급하며 live 기본값으로 옮기지 않는다. runtime의 `allow_assist=false`와 exact-request provenance 차단은 유지한다.

## 설치·복구 범위

새 VBS preload는 SHADOW 임시 SM 설정에만 추가한다. 영구 SM 설정에는 남기지 않는다. 기동 가드는 AA와 VBS의 실험 DSO를 포함한 manifest를 묶고, SM이 실험 라이브러리를 매핑하기 전에 일회성 권한을 소비·동기화한다. 다음 가드 경유 기동은 baseline이다. 같은 실행 중인 SM 내부 재시도나 저장장치 고장을 복구했다고 주장하지 않는다.

VBS는 차량 데이터를 전달하는 서비스이므로 후크의 실제 시작·지연·coexistence 검증은 필요하다. 이 문서와 Draft PR은 차량 설치 승인이나 새 릴리즈가 아니다. 운전 중 명령 실행은 없으며 설정·기동 확인·로그 회수는 주차 중에 한다.

## 로그와 남은 확인

`motion`은 수신한 raw/count/reverse와 수신 시각을 남긴다. `shadow`는 위치·속도·heading, 계산 결과, 중단 이유, 불확실성 비트, 모델 오차 예산, LOCATION 미리보기를 남긴다. 모든 SHADOW 레코드는 `assist_ready=false`다. 로그 누락이나 audit fault가 있으면 그 세션의 계산을 중단한다. 기존 로그 용량 제한은 보관량 제한이고 플래시 총 기록량 제한은 아니다.

실차에서 확인할 것은 source hook ABI/시작, 원본 callback·AA/touch 공존, 실제 휠/yaw/6MT 후진 cadence, 시간 지연 상한, bias·보정, 실제 위치 오차, 다음 기동 복귀다. live ASSIST에는 여기에 정확한 LDS 요청 provenance, AA timestamp 계약과 폰/네이버 지도 수용 검증이 추가로 필요하다. send 함수의 성공만으로 폰 수용을 판단하지 않는다.

검증 결과와 환경상 실행하지 못한 검사는 별도의 `validation/LIVE_SHADOW_2026-09-29.md`에 기록한다.
