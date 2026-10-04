# v1.0 베타(ASSIST) 현황 조사와 단일 주행 방안(B)의 포기 — 2026-10-05

`v0.3.12-shadow.5` 한 번 뒤에 곧바로 ASSIST를 켠 베타로 가는 방안(B: 한 번의 주행에 SHADOW 단계와 ASSIST 단계를 담음)을 검토하고, 완성도 기준을 채울 수 없어 **포기**했다. 서로의 결론을 보지 않은
읽기 전용 조사 세 건(M1 송신 경로, M2 데이터 흐름과 wire, M3 세션 후킹 없는 안전성)의 결과이며 M4(주행 자료 재생)는 진행 중이다. 코드는 실행·수정하지 않았다.

## 이미 있는 것

- 송신 쪽 대체 장치가 완성돼 있다: 원본 송신을 정확히 한 번 호출, 반환값과 errno 보존, 원본 래퍼를 복사해 payload만 교체, 모드 전환·콜백 예외·추가 LOCATION·큐 포화에서 즉시 원본 복귀(`src/adapter/adapter.cpp`).
- 선택 조건은 모두 fail-closed(`choose_dr`): OEM mode==0, 출처 세 플래그, 세션 관측, generation/epoch, 후보 ready/qualification/limits, 시각·나이.

## 제품에서 ASSIST가 한 번도 동작할 수 없는 이유

1. `allow_assist=false` 하드코딩(`runtime.cpp`), 설정 파일에 ASSIST 값이 없고(`config.cpp`), assist tick은 `mode==3`, 센서 채널은 `mode==4`에서만 열려 **ASSIST와 센서 입력이 구조적으로 배타적**이다.
2. `provenance()`가 항상 false(`runtime.cpp`), 제품 worker는 AssistWorker 없이 시작(`run_worker(root,channel,0)`). **제품용 `AssistSource`가 없다**(센서/GPS → qualified 입력).
3. 센서 증거에 생산자 시각이 없다(`source_mono_ms`가 전부 0). 자격 있는 증거는 `PRODUCER_TIME` 또는 `SEQUENCE_WITH_BOUND`를 요구한다. 수신 시각을 자격으로 올리지 않는다는 프로젝트 규칙과 충돌한다.
4. **정확도 필드 없음**: `encode_location`이 48바이트를 0으로 채워 `hasAccuracy=0`. 네이버는 정확도 없는 이동 fix를 무시한다(DHU). `DrSnapshot`에 정확도 필드가 없다. 대체가 원본 payload를 복사하지 않고 새로 만든다.
5. 6MT의 후진 증거는 변화 때만 오고(주행 전체 2건) 코어는 250 ms 안의 후진 증거를 요구해 ACTIVE에 도달하지 못한다.
6. 신선도 창(센서 창 + 재정렬 + 틱 = 100~250 ms)이 상한 150 ms를 넘나들어 1 Hz 송신의 상당 부분이 만료될 것으로 추정된다(실측 없음).
7. **`libpatch`와 공존하면 세션 관측이 꺼져 있어(`f97d13f`) `session_reader`가 항상 UNOBSERVED를 돌려주고, `choose_dr`가 항상 EPOCH_MISMATCH, MODEL 계산도 `session_unavailable`로 막힌다.**
   따라서 `shadow.5`를 차에서 돌려도 온라인 MODEL 계산은 돌지 않을 가능성이 높다(원시 센서 기록과 위치 관찰은 남는다; 같은 계산은 오프라인 재생으로 한다). 추정이며 실행 증거는 없다.

## 순정 GPS 단절 신호(2026-10-04 기록에서 관측)

LDS sideband 5,852건: 46–939초 mode 1, utc_s 0(저장 위치, 정확도 4.4/9.7 고정), 939–1453초 mode 1, utc_s>0 증가, HDOP 1.3–2.8, 1453–1459초 **mode 0, 수평·수직 99/99, 위치는 마지막 fix로 고정, 속도 8 km/h(0 아님)**.
mode 2·3은 나오지 않았다. utc_s>0은 fix의 증거가 아니다(끊김 중에도 증가). "GPS 끊김"은 그 호출의 원본 POSITION이 mode 0(그리고 99/99)인 것으로 정의할 수 있다.
치환 LOCATION 규격(M2): `hasAccuracy=1`, 정확도 = ceil(오차 예산(lease 끝)×1000), **예산이 40 m를 넘으면 치환하지 않고 원본 통과**(낮춰 보고·클램프 금지), 속도·방위는 DR 값, timestamp는 무시되므로 원본 유지. 공개 기본값에서 14 m/s의 40 m 창은 12초뿐이다.

## 세션 후킹 없는 ASSIST의 최소 안전 장치(M3)

S1 송신마다 새 mode-0 POSITION 관측, S2 lease ≤150 ms 단일화, S3 `session_storage` 포인터 변화 감시(필요조건일 뿐; OEM이 같은 주소를 재사용), S4 송신 결과가 0이 아니면 즉시 폐기하고 새 mode-0 POSITION + 연속 0 이후에만 재진입,
S5 POSITION 콜백 공백 감시, S6 `session_epoch`를 송신 관측 epoch로 재정의. 상태 기계: OBSERVE → ARMED → GPS_LOST → ENGAGED → WITHDRAWN/FAULT. jciAAPA가 죽으면 `reset_board=yes`로 CMU 전체가 약 5초 안에 재부팅되고 일회성 가드로 다음 부팅은 순정이다.

## 결정

B는 포기한다. 이유: 구현 범위가 크고 여러 항목이 프로젝트의 안전 규칙을 바꾼다(수신 시각 증거, 후진 정책, 세션 근거). 후킹 설치, `libpatch` 공존, ASSIST를 한 번에 처음 켜는 것은 위험이 겹치고, 크래시 비용이 "AA·내비 전체 상실 + 시험 종료"이다.
`shadow.5` 주행을 먼저 한다(차량 미검증인 후킹과 `libpatch` 공존을 확인). 그 자료를 받아 위 항목을 오프라인에서 구현·검증한 뒤 v1.0 베타를 낸다(소유자 합의: 베타가 다음 시험이며 중간 시험은 없다).

## 베타 작업 목록(보고가 정리한 것)

1. 설정에 ASSIST(센서 캡처 유지)와 부트·후킹 성공 뒤에만 `allow_assist`, `assist_block`을 실제 사유로 기록.
2. 제품용 `AssistSource`(관측 큐와 VBS 입력, LDS 앵커) 또는 소유자가 승인한 "베타 자격 기준"(MODEL 계산 결과를 명시 표시된 별도 경로로 전달), `provenance()` 구현.
3. S1~S6와 상태 기계, 모든 전이 journal.
4. `DrSnapshot` 정확도, 원본 payload 복사 후 위치·정확도·속도·방위만 덮어쓰는 인코더, 40 m 초과 시 원본 통과.
5. 후진 증거 정책(latch), 신선도 창과 lease 재설계, 보수적 한계(예: 30 s / 800 m).
6. 재생 하네스(trace의 motion_batch, lds_sideband, POSITION → 파이프라인)와 오프라인 검증 사다리(재생, QEMU 실제 BLM+libpatch, DHU).
7. 소유자의 명시적 승인 기록(ASSIST 관문을 베타 시험에 한해 완화).

## 한계

모두 정적 분석이다. 실제 터널·garage에서 mode 0이 길게 지속되는지, 폰이 ts=0 위치를 채택하는지, `libpatch`가 콜백 표를 제자리에서 수정하는 상호작용, send 시점 snapshot 나이 분포는 확인하지 못했다.
이 기록은 설계 변경이나 live ASSIST 활성화, 차량 시험 승인을 뜻하지 않는다.
