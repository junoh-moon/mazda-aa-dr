# BETA 재생 하네스(T6) — 2026-10-05

[설계](ASSIST_BETA_DESIGN_2026-10-05.md)의 T6이다. `tools/replay_beta.cpp`(`make test-replay-beta`)는 차량 journal 형식의 디렉터리를 읽어
**제품 코드 경로**를 그대로 돌린다: `motion_batch` → `navigation::Pipeline`(MODEL+BETA 코어, `init_model(...,true,true,true)`+`enable_beta`),
50 ms 작업자 틱 → `Pipeline::drain` + `runtime::BetaController`(`map_model_publication`, `publish_snapshot`),
POSITION/LOCATION → `adapter::position_enter`/`send_vehicle_data`/`position_leave`. 런타임 접착부(`beta_provenance`, 송신 storage 카운터, hold 이벤트)는 `runtime.cpp`와 같은 inline 함수를 쓴다.

작성한(가짜) 부분: OEM send(항상 0 반환, 48바이트 기록), 시계, 1스레드 순서(관측은 다음 틱에 pop), ModelSession/ModelBus 울타리(항상 통과), LDS 연관(없음).
위치 입력은 어댑터 `position`/`send` 행이 있으면 그것을, 없으면 같은 boot의 collector `position_poll` 행을 쓴다(원본 48바이트는 poll 값으로 합성, 패딩은 고정 패턴).
`shadow_input_reset` 행은 런타임처럼 source epoch를 올려 파이프라인을 reset한다.

## 모드

- **의사 단절**: `--t0`/`--sweep FROM:TO:STEP`과 `--durations`(기본 10,20,30,45,60 s). T0부터 D초 동안 기록된 콜백을 막고 `--cadence-ms`(기본 1000) 간격으로 mode 0 콜백(마지막 fix 동결, 정확도 지움, 99/99)을 보낸다.
  각 창은 T0 시점의 재생 상태를 `fork()`로 복사해 독립 실행하므로 창끼리 상태가 섞이지 않고, 제품 경로의 크래시는 창 실패로 보고된다. 치환된 LOCATION을 같은 시각의 기록 GPS(수신 시각 보간, `--truth-lag-ms`로 이동)와 비교한다.
- **실제 단절**: 기록에 mode 0이 있으면 기록대로 재생한다(`--real-only`는 의사 창 없이 이것만).

출력(JSON `--report`, 송신별 CSV `--csv`, BetaController 행 `--journal`): 송신별 선택·사유, 위치 오차, 휠 속도 대비 속도 오차, GPS 코스 대비 방위 오차(GPS 15 km/h 이상), 보고 정확도 적중률, 최장 ENGAGED, 이탈 사유별 수, 미치환 창의 사유.
`--check`는 다음에서 실패한다: mode≠0 치환, 정확도 >40 m 또는 ≤0, 허용 필드(8–16, 20–23, 32, 36–40, 44–47) 밖 바이트 변경, (의사) GPS 복귀 뒤 치환, OEM send 계약(1회 호출·반환·선택) 위반, 창 크래시, 적중률 <0.95.
`--self-test-corrupt payload|accuracy|mode`는 판정기 자체 시험용이다.

## CI 증거(합성 자료만)

`tests/replay/test_replay_beta.py`가 직선·가감속·90° 곡선·25초 터널(감속, mode 0)·GPS 복귀를 프로그램으로 생성한다(개인 자료 없음). 5개 시험: 의사 단절 sweep `--check` 통과(적중률 ≥0.95, p90 오차 <10 m, 휠 대비 속도 오차 <0.6 m/s, 방위 p90 <3°, 첫 치환은 단절 1초 뒤),
실제 터널에서 치환 속도가 감속을 따르고 복귀 뒤 원본만 송신, GPS 3초 이동 시 적중률 실패 검출, 손상된 페이로드·정확도·mode 검출, 유효 GPS 없는 창 건너뜀.

## 비공개 주행 재생(선택)

`MX5DR_TRIP_DIR=<logs 디렉터리> make test-replay-beta`(구간은 `MX5DR_TRIP_SWEEP=FROM:TO:STEP`, 기본 전체 2초 간격). 보고서는 `build/replay_beta_trip.json`/`.csv`에만 남고 커밋하지 않는다. 좌표·주행 자료는 저장소에 넣지 않는다.

## 한계

GPS는 참값이 아니고 수신 시각은 fix 시각이 아니다. 작업자 스레드 경쟁, 큐 포화, journal 실패, 실제 OEM 송신 주기, 세션 훅, libpatch 공존, ARM 빌드는 재현하지 않는다.
합성·재생 통과는 차량·폰·DHU 증거가 아니며 live ASSIST나 차량 시험을 승인하지 않는다.
