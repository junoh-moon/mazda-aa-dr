# MODEL 결과 분석기 통합 — 2026-09-30

`feat/session-observation`의 `e2c31c5`에 외부 master
`8cbcc29f8748c37793bd1f31322d58ceae8e1372`를 병합했습니다. 유효성·조회 결과·상태·시각·수치가
서로 모순되는 MODEL 기록을 정상 증거로 취급하지 않는 분석기 수정입니다.
외부 검토 이력은 [원래 기록](MODEL_RESULT_REVIEW_2026-09-30.md)에 보존하며,
아래는 통합 작업 트리에서 직접 실행한 후속 검사입니다.

- `make -j2 test`: host Python **330개**와 C/C++ 검사를 skip 없이 통과했습니다.
  원본 rootfs와 기존 검증 제품 bundle 경로를 지정했습니다.
- 별도 `make -j2 test-motion-journal`: journal Python **94개**를 통과했습니다.
  이는 전체 host 검사와 겹치므로 새 검사 수에 더하지 않습니다.
- 고정 GCC 4.9.1로 변경된 journal fixture를 빌드하고 QEMU ARM으로 실행했습니다.
  결과 의미 검사 **12개**와 fixture 자체 검사를 통과했습니다.
- 실제 producer 출력 **18개**는 이번 host/ARM 실행에서 바이트까지 같았습니다.
- `src/`와 `Makefile`은 이전 제품과 동일합니다. 이번 분석기 통합을 이유로
  ARM 전체 suite와 제품 원본 VM을 다시 실행하지 않았습니다.

이전 원본 journal 세 개의 전체 보고서가 새 분석기와 동일하다는 직접 재분석은
[앞선 통합 기록](BUS_CLOCK_MERGE_2026-09-30.md)을 따릅니다. 같은 검사를 새 원본
실행으로 세지 않습니다. 병행한 GPIO 진단은 별도 LDS 기동 조사이며 유효 GPS나
관성항법 적용 완료를 의미하지 않습니다.

generation 소진 후 fault가 기존 MODEL 유효성을 재노출하는 Pipeline 결함은
이번 분석기 수정의 범위 밖이며 **미구현 TODO**입니다. 실제 보정 위치의 AA 선택,
qualified 센서·요청 출처와 폰/앱 수용은 여전히 미완료입니다. ASSIST는 비활성이며
공개 설치 ZIP을 변경하지 않았습니다.

비공개 실행 기록은 `evidence/lds-gpio-20260930-e2c31c5/`에 보존합니다. 같은 전용
컨테이너에서 GPIO 조사가 진행 중이므로 추가 도구 정리는 후속 GPIO 검증 기록에
남깁니다. 아직 제거했다고 주장하지 않습니다.

| 파일 | SHA-256 |
| --- | --- |
| 전체 host 로그 | `44ceea6d93795db85d3787d31bafd48245e51a5600eee07621f2bcef2e2c9f51` |
| 별도 host journal 로그 | `97e527dbb8a95d9d2cc2bf5d68f080e97392cf42c95a799aaa3fb03af9e92ceb` |
| 고정 ARM journal 로그 | `5422bd10276edf4513ec90fa1c9577aa3e7c75332c4da3e59ac4be559b83667d` |
| host/ARM producer 출력 | `d964f1018b58dc066f3d5174f873f9b7a48d75ccfdc4888424a7356ff17b7253` |
| 통합 분석기 | `a104cb9b167cca5812c5a414935c1caa971647e4e22b85e70115a8e8efa17087` |
