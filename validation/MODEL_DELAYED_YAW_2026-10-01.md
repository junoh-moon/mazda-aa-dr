# 지연된 yaw 구간과 GPS 참조 순서 보완 — 2026-10-01

NA 74.00.324A용 MODEL SHADOW가 정상적인 지연 yaw 평균을 `LATE`로 거부할 수
있었습니다. 기존 [기준점 준비 수정](MODEL_WINDOW_ORDER_2026-10-01.md)은
휠만 기다렸지만, 위치 관측이 대기 중이면 예외적으로 휠과 GPS를 먼저
소비했습니다. 기준점이 생긴 뒤에는 이 예외와 무관하게 늦은 yaw 구간보다
새 휠·GPS·모드 전환을 먼저 소비하는 경로도 있었습니다. 이번 수정은
계산기의 원본 250ms 입력 기한을 유지하면서 열린 yaw 구간보다 나중인
MODEL 입력을 큐에 남깁니다. 실차 위치 정확도나 ASSIST 송신 자격은
이 변경으로 입증되지 않습니다.

## 재현과 제품 변경

신선한 Linux checkout의 `MX5DR_TEST_SLOW_YAW=1` worker 회귀가
`PIPELINE_LATE` 초기화로 실패했습니다. 실패 trace에서 위치 관측이
6300ms에 내부 소비 시각을 먼저 진행했고, 6240→6400ms 평균 yaw가
뒤늦게 들어오면서 구간 시작 6240ms를 과거 입력으로 거부했습니다.
원본 raw는 정상 순서로 도착했지만 스트림별 지연이 달랐습니다.
동일 경로를 `test_navigation`에서 위치·GAP·NATIVE 및 기준점 전후의
결정적 순서로 재현했습니다. 첫 yaw 콜백 전에 휠·GPS가 도착하는
교차 스트림 순서도 별도 회귀로 고정했습니다.

`Pipeline::drain()`은 첫 yaw 경계 전의 MODEL 입력을 소비하지 않고,
경계가 생긴 뒤에는 다음 평균 구간이 닫힐 때까지 그 경계보다 나중인
입력을 기다립니다. 대기 중인 유효 GPS·NATIVE·기준점 후보는 옛
MODEL 출력을 즉시 숨깁니다. 반복 GAP만으로 유효한 이전 적분을
계속 숨기지는 않습니다. 실제 yaw 무수신은 기존 250ms 기한에서
`MISSING_SENSOR`로 초기화합니다. 기준점 전 큐가 비어 있어도 마지막
yaw 뒤 251ms의 무수신을 기록합니다. QUALIFIED 도메인의 기존
frontier 기한도 250/251ms 경계 시험으로 확인합니다.

holdout은 GPS 참조 하나를 파이프라인에 이미 제출했으면 yaw 구간이
닫힐 때까지 중복 제출하지 않고 같은 원본 시각으로 기다립니다.
기한이 지난 제출 참조는 `ABORT/stale_reference` 또는
`ABORT/source_fault`와 파이프라인 초기화로
중단하며, cooldown 중에는 해당 사유를 기록하되 원래 종료 시각을
연장하지 않습니다. 아직 제출하지 않은 오래된 참조는 새 기준점으로
사용하지 않고 `SKIPPED/stale_reference`를 남깁니다. 이는 종결
이벤트나 소스 결함을 가장하지 않으며 분석기는 해당 세션을
inconclusive로 판정합니다. 늦은 worker turn이 과거 GPS를 소급하여
holdout `BEGIN`으로 만드는 것도 막습니다. 참조 기한은 요청된 drain
watermark와 이미 수신한 최신 입력 중 늦은 시각으로 판정하며, 별도로
yaw 수신 경계가 250ms를 넘긴 경우에는 `SOURCE_FAULT`를 우선 기록합니다.

## 검증 범위

수정 전의 Linux 전체 suite에서 위 slow-yaw worker가 실패했고,
원인 trace와 수정 전·후 결정적 회귀 원본을 비공개
`evidence/linux-test-20261001/`에 보존합니다. 최신 소스의 Linux
`make test`는 원본 펌웨어 fixture를 지정해 종료 0으로 통과했습니다.
Python 371개와 C/C++·worker 회귀를 실행했고 navigation 2,957개,
holdout 5,685개, GPS-wheel 84,616개 합성 검사가 통과했습니다.
처음 실패했던 slow-yaw worker는 raw 499건을 기록·수락하며 초기화
없이 통과했습니다. 다섯 바이너리 릴리즈 묶음을 아직 만들지 않아
설치 payload 검사 2개는 명시적으로 생략됐습니다. 이 코드 검증은
그 두 검사나 고정 ARM/QEMU·최종 ZIP 검증을 대신하지 않습니다.

Codex 독립 리뷰 네 갈래와 Claude의 읽기 전용 비판 검토를 받았습니다.
첫 yaw 선처리, 무큐 센서 중단, 오래된 미제출 GPS 참조의 cooldown 연장이
후속 검토에서 드러나 코드와 회귀에 반영됐습니다. SKIPPED의 결과 큐
용량 경계에서는 참조를 먼저 제거한 뒤 사본으로 기록하여 재시작 시
큐 길이를 잘못 줄이지 않습니다. 리뷰는 합성 검사의 결과를 실제 차량
승인으로 바꾸지 않습니다.

## 미검증

MODEL은 여전히 수신 시각과 생산자 측정 시각을 구분합니다. 이 순서 수정은
실제 센서 품질·시각·단위, 실차 부하와 정상 기동·복구, GPS와 독립된
정확도, Galaxy S25/무선 AA/지도 앱 수용을 입증하지 않습니다.
live ASSIST는 계속 비활성입니다. 검증된 SHADOW 후보만 한 번의
[통합 현장 시험](../docs/FIELD_TRIAL_KO.md)에 사용합니다.
