# SHADOW 센서 로그: 원본 필드를 보존하는 배치 기록

PR #12의 [Claude 리뷰](https://github.com/junoh-moon/mazda-aa-dr/pull/12#issuecomment-5884112251)는 센서마다 긴 JSON 객체를 기록하면 8MiB × 3 순환 로그에서 주행 초반 기록이 빨리 사라질 수 있다고 지적했다. 이 변경은 기존 `motion` 레코드의 **모든 RawEvent 필드**를 보존하면서 반복 필드명을 줄인다. SPI 패킷 전체를 새로 캡처하거나 센서의 물리적 측정 시각을 알아낸 것은 아니다.

## 실행 동작

- `navigation.enqueue_raw(raw)`는 기존과 같이 모든 channel-accepted 입력에 호출한다. 계산 입력을 샘플링하거나 속도·yaw·후진 값을 평균화하지 않는다.
- 기존 worker에서만 최대 32개를 고정 6,144바이트 버퍼에 묶는다. 새 스레드, 타이머, 힙 할당, 파일, 설정 옵션을 추가하지 않는다.
- 배치가 가득 차거나 source epoch가 바뀌면 기존 배치를 기록하고 **같은 입력을 재시도**한다. 입력을 버리지 않는다.
- 채널 오류 표식 직전과 매 receive-loop 종료 시 배치를 기록한다. 빈 소켓, 256회 처리 제한, 오류 후 조기 종료 모두 해당한다. 다음 worker sleep이나 POSITION 처리까지 배치를 보관하지 않는다.
- 기존 journal의 크기 제한·순환·flush·실패 처리를 사용한다. 새 형식은 stdio의 전원 단절 내구성을 개선하지 않는다. 쓰기/flush 실패는 기존처럼 audit fault로 처리한다.
- VBS 후크, Unix datagram 프로토콜, DR 계산, AA 원본 송신, ASSIST 차단은 변경하지 않는다.

## 파일 형식

기존 `motion` 한 줄 대신 **자체 해석 가능한** `motion_batch` 한 줄을 쓴다. 파일이 순환되어 앞부분이 없어도 별도 사전 없이 필드를 복원할 수 있다. 전체 세션의 완전성을 보장하지는 않는다.

```json
{"kind":"motion_batch","schema":1,"epoch":42,"producer_time_status":"unknown","events":[[2,101,1000000000000,0,6141,0,0,0,3,0],[3,102,1000000000010,0,0,0,0,0,0,1]]}
```

`events`의 각 행은 정확히 10개의 정수다.

| 0 기반 열 | 기존 필드 | 의미 |
| --- | --- | --- |
| 0 | sensor | 1=wheel, 2=yaw, 3=reverse |
| 1 | receive_seq | 관찰자 수신 순번. 생산자 순번 아님 |
| 2 | received_ns | 수신 측 monotonic ns. 측정 시각 아님 |
| 3 | source_mono_ms | 기존 signed 64-bit 필드 그대로. 현재 VIM IPC는 0이며 생산 시각 없음 |
| 4–7 | raw[0..3] | 기존 네 unsigned 16-bit 값 |
| 8 | count | 기존 unsigned 16-bit 값. 0도 기록에서 보존 |
| 9 | reverse | 전송 계약의 0..65535 원값. 항법 적합성은 별도 판단 |

`epoch`는 배치 전체에 공통이고 `producer_time_status`는 항상 `unknown`이다. uint64를 부동소수점으로 바꾸거나 시간 차로 인코딩하지 않는다. JavaScript Number는 64-bit 정수를 손실할 수 있으므로 제공된 Python 분석기처럼 범위를 보존하는 판독기를 사용한다. `schema:1`은 배치 형식 버전이며 릴리즈 번호가 아니다.

## PC 분석기

기존 `tools/analyze_logs.py`가 과거 `motion`, 새 배치, 혼합 로그 모두 읽는다.

```sh
python3 tools/analyze_logs.py /path/to/exported-logs.tar.gz --json
```

- `motion.samples`는 복원한 센서 레코드 수, `motion.batches`는 유효한 배치 수다. `record_counts`는 실제 JSON 행 종류를 센다.
- 형식·버전·필드 수·정수 범위 검증은 **한 배치 전체**가 통과해야 한다. 마지막 행이 잘못돼도 앞부분만 정상 입력으로 반영하지 않는다.
- 모든 센서에 걸친 수신 순번의 단절·역행·중복과 수신 시계 역행을 보고한다. 역행 레코드가 high-water mark를 낮추지 않는다. 첫 순번이 1이 아니라고 수신 이전 누락 수를 추정하지 않는다.
- `shadow_input_reset`이 앞뒤 순번의 누락을 덮지 않는다. 채널 검사가 거절한 입력과 물리적인 센서 측정 손실을 구분해야 한다.
- SHADOW pipeline/result/state/model-valid 통계와 reset/rejected 최대값을 보고한다. OVERFLOW 등 오류와 fault counter 증가는 `inconclusive`다. 정상적인 WAITING/NO_ANCHOR/MISSING_SENSOR 자체를 원본 송신 위반으로 판정하지 않는다.
- 배치·SHADOW 뒤의 health 누락, audit fault, 순환으로 사라진 boot 등 불완전 관측 조건을 유지한다.
- `local_checks_pass`는 기록 구간의 로컬 불변식 검사 결과다. 모델 계산 성공, 항법 오차 검증, 실차 설치 승인, 폰/지도 앱 수용을 의미하지 않는다. 구버전 분석기는 새 형식을 모르는 것으로 보고하므로 이 변경을 포함한 분석기로 읽는다.

Python의 `decode_motion_records(row)`는 기존 `motion` 객체와 같은 필드의 리스트를 반환한다. 실제 위치·이동 경로를 공개 저장소에 올리지 않는다.

## 감소량과 한계

합성 혼합 센서 9,000개를 8개씩 묶는 검사에서 기존 JSON **1,744,442바이트 → 585,692바이트(66.4% 감소)**였고 모든 필드·순서가 일치했다. 약 150개/초를 가정한 타임스탬프를 사용한 시험 입력이며 실제 CMU 센서 빈도를 측정한 수치가 아니다. 낮은 빈도에서는 배치 이득이 더 작다.

이 수치는 **motion 레코드만** 비교한다. 100ms SHADOW 요약, POSITION/SEND, health, 별도 collector 기록량은 그대로다. 전체 flash 쓰기량이나 보존 시간이 66.4% 개선됐다는 뜻이 아니다. 유한 순환 로그이므로 긴 주행의 시작 부분 보존을 보장하지 않는다. 새 설정이나 사용자 조작 없이 적용된다.

[검증 결과](../validation/COMPACT_SHADOW_LOGS_2026-09-29.md).
