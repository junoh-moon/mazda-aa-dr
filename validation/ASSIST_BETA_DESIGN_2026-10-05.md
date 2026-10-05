# v1.0 베타 최소 설계 결정 — 2026-10-05

[문제 정의](../docs/PROBLEM_DEFINITION_KO.md)의 L1 속도, L2 방위를 위한 최소 제품 설계다. 읽기 전용 조사(코드 줄 근거 포함)에서 나온 결정을 기록한다. 코드는 아직 바뀌지 않았다.
이 기록은 차량 시험이나 live ASSIST 활성화를 승인하지 않는다.

## 결정

1. **입력 경로**: 기존 `AssistWorker`(자격 도메인; `quality==VALID`와 `PRODUCER_TIME`/`SEQUENCE_WITH_BOUND`를 요구) 대신 **MODEL 파이프라인의 결과를 명시적으로 표시된 BETA 도메인**으로 어댑터에 넘긴다.
   MODEL 경로는 이미 실차 입력을 소비하고 오차 예산·속도·진행 방위·정지를 계산하며 지난 주행 재생으로 검증됐다. 자격 도메인의 열거값을 거짓으로 채우지 않는다. `AssistWorker`, SCRUB, 자격 경로, `allow_assist=false`는 그대로 둔다.
2. **송신 치환**: 원본 48바이트를 복사하고 위경도·정확도·속도·방위(8–23, 32–47)만 덮어쓴다. 0–7, 고도(24–31)는 원본을 유지하고 `hasAccuracy=1`, `accuracy_e3=ceil(예산_m×1000)`.
   **위치는 DR 전진**이다(동결하면 정직한 정확도가 v·t라서 14 m/s에서 3초에 40 m를 넘고, 속도≠0·위치 고정은 DHU에서 끊김으로 판정된 조합이다).
3. **정확도와 한계**: [정확도 규칙](ACCURACY_RULE_2026-10-05.md)을 따른다. 베타 코어 설정은 e0 20 m, h0 0.03, k 0.002, sv 0.3과 `error_max_m=40`이며 40 m 초과는 코어 자체의 한계 실패로 처리해 낮춰 보고하는 경로가 구조적으로 없다(재진입은 새 GPS 앵커가 필요).
4. **신선도**: 코어를 바꾸지 않고 frontier 시각(나이 0)에 모델 스냅샷을 조회하고, 브리지가 `정확도 = 예산 + (v+sv)×lease`로 계산하며 `valid_until = frontier + 500 ms`. 좌표는 외삽하지 않는다.
5. **송신 시점 결정**: 그 호출의 원본이 mode 0(필수), fault·hold 없음, 도메인 BETA, 세션 결과가 OBSERVED 또는 UNOBSERVED(설치가 명시적으로 거절한 경우)일 때만, 그리고 generation·epoch 일치, ready, lease 안, 정확도 ≤40 m일 때만 치환하며 그 외는 전부 원본 통과.
6. **세션 경계 대체(H2에 필요한 것만)**: S1 송신마다 mode 0 컨텍스트(기존), S2 500 ms lease와 예산 반영, S4 치환 송신 반환≠0이면 hold(ORIGINAL 송신 반환 0을 관측해야 해제). `session_storage` 포인터 변화는 카운터로 세어 invalidate와 journal만 하고 게이트로 쓰지 않는다. 폰 세션 epoch는 자격 계보용이었고 페이로드 정확성은 차량 센서와 GPS에서 나온다.
7. **후진 증거**: MODEL 도메인의 REVERSE만 래치(`lease_until=UINT64_MAX`), `REVERSE_LATCH_MODEL` 표지 유지, 휠 속도 0에서 풀지 않는다(변화 전용 생산자는 바뀌지 않으면 메시지를 보내지 않는다). 리셋은 source epoch 변경뿐이고 부팅 후 첫 후진 메시지 전에는 미지로 seed하지 않는다.
   래치가 틀려도 wire에 나가는 것은 진행 방위라 전진 중 위치·방위는 맞고, 놓친 후진 진입은 주차 칸 몇 m의 유계 오차다.
8. **상태 기계**: DISABLED → ARMED → GPS_LOST → ENGAGED → ARMED(GPS 복귀) 또는 WITHDRAWN{budget_limit, sensor_silence, send_result_hold, session_storage_changed} → FAULT(고정, OBSERVE). 모든 전이를 `beta_state` 행으로 기록하고 요약은 1 Hz로 제한한다.
9. **설정**: `mode=BETA`(내부 5)를 명시 opt-in으로 추가한다(SHADOW 캡처를 유지하고 BETA를 덧붙임). 기본값은 SHADOW를 유지한다.

## 작업 분해(위험)

T1 어댑터(`DrSnapshot` 정확도·beta, `Mode::BETA`, `Provenance.domain`, 복사 후 덮어쓰기 인코더, `beta_choose`)—중. T2 `core_bridge`(`map_model_publication`)—하. T3 파이프라인(후진 래치, `model_publication`)—중. T4 런타임(mode 5, 세션 울타리 대체, `beta_provenance`, 틱마다 발행, hold, 상태 journal)—상.
T5 베타 코어 설정—하. T6 저장소 내 재생 하네스(실제 pipeline+bridge+adapter에 JSONL 투입, 가짜 OEM 송신으로 48바이트 검사)—중. T7 패키징(`--mode=BETA`, 번들 기본값, 메뉴2 표시, `analyze_logs`)—하. T8 ARM 시나리오·설치 probe—하. T9 DHU wire 시험—중(소유자 장비 필요).

검증 사다리: 호스트 단위 → 2026-10-04 자료 재생(mode≠0에서 치환 0건, 정확도 ≤40 m, 의사 단절 재확인, 1453~1459초 mode 0 전이) → QEMU ARM(실제 BLM+libpatch) → DHU wire → 결함 주입(송신 반환≠0, 세션 포인터 변경, 추가 LOCATION, 센서 침묵 300 ms, 후진 메시지 누락, 예산 40 m 경계 39.9/40.1, lease 직전, 컨텍스트 풀 고갈, 큐 포화, journal 실패).

## 바뀌는 기존 시험·기록

`tests/adapter/adapter_test.cpp`(DR 치환에서 `sent[16]==0` 기대), `runtime_assist_test.cpp`, `assist_publication_test.cpp`, `test_assist_worker.cpp`, `tools/analyze_logs.py`와 시험, `tests/packaging/test_install_defaults.py`(ASSIST 거부 유지, BETA 허용),
trial 메뉴 표시, journal 시험의 `assist_ready:false` 문자열, `docs/STATUS_KO.md`, `docs/V1_READINESS_KO.md`, `AGENTS.md`의 "ASSIST 비활성" 문구(베타 범위로). 과거 기록의 결과 수치는 편집하지 않는다.

## 남는 한계

실제 터널의 mode 0 지속과 송신 주기, 네이버의 Mazda wire 속도 추종, `libpatch`와 공존한 훅 동작은 `shadow.5` 주행 자료가 필요하다.
