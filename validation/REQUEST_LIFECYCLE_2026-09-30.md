# 실제 요청 취소·연결 종료 관측 — 2026-09-30

이 기록은 [요청 관측 연결](REQUEST_LINK_2026-09-30.md)의 후속 비공개
fixture입니다. 원본 JCIDBUS 연결 종료와 실제 method 해제 경계를 실행했고,
제품 `Observer`의 슬롯 회수를 별도 사건 기록으로 대조했습니다. 제품의
자동 cold-install·journal 연결이나 ASSIST 자격의 완료 기록은 아닙니다.

## 실행 범위

대상은 NA 74.00.324A의 동일한 원본 커널·rootfs·BLM·JCIDBUS·LDS입니다.
고정 GCC 4.9.1로 작성 fixture를 빌드하고, 호스트 네트워크·장치 전달·공유
디렉터리 없는 전체 시스템 VM에서만 OEM 코드를 실행했습니다. 원본 공개
연결/API와 이전에 검증한 요청·worker 경계를 사용했습니다. private LDS
객체 debugger 작업은 재시도하지 않았습니다.

원본 manager의 자동 위치 요청, 명시한 합성 캐시 입력, 기존 OBSERVE/SCRUB
검사를 먼저 실행했습니다. 원본 생산자 정지·추가 task 소멸·큐 배출 뒤 아래
네 경우를 수행했습니다. 정상 응답의 mode·UTC·좌표는 여전히 0입니다.

| 경우 | 실제 API와 조건 | 대기 요청 | 실제 method 종료 | callback/위치 관측 |
| --- | --- | ---: | ---: | ---: |
| AA 연결 해제 | AA용 util로 제출 후 해당 연결을 dispatch하지 않고 `conn_free` | 4 | 4 | 0 |
| AA disconnect | 같은 제출 조건에서 `conn_disconnect`, 이후 free | 4 | disconnect에서 4 | 0 |
| 응답 전 해제 | LDS를 SIGSTOP한 뒤 data API로 제출·dispatch, 연결 free | 4 | 4 | 0 |
| 새 연결의 늦은 응답 | LDS 정지 중 data API로 제출·dispatch, 이후 SIGCONT | 4 | 4 | 4 |

AA용 util은 비동기 data 요청 앞에서 동기 control 조회를 수행합니다.
따라서 첫 두 경우는 제공자가 실행 중일 때 제출하고, 원본 callback이 아직
실행되지 않은 상태에서 연결을 종료했습니다. 버스 응답이 이미 대기 중일
가능성이 있습니다. 뒤의 두 경우만 제공자 정지 중 data API를 직접 호출한
시험이며, AA용 util의 동기 단계까지 검증한 지연 시험으로 바꾸지 않습니다.

네 경우 모두 300ms 대기 창에서 요청 슬롯 네 개를 유지했습니다. elapsed
time·disconnect 의도를 종료로 간주하지 않았습니다. 실제 method 해제가
종료 근거입니다. 새 연결에서는 주소가 세 번 재사용됐고, 새 요청 네 건만
완료됐습니다. 마지막 500ms 배출 창까지 취소 요청의 추가 callback은 없었습니다.
원본 manager는 이미 정지했으므로 마지막 네 위치에서 native send는 0건입니다.

전체 실행은 요청 29건(기존 정상 13, 취소 12, 마지막 정상 4), 위치와 송신
관측 170건, 경계 사건 158건이었습니다. method 주소 재사용은 13회입니다.
별도 기록한 실제 notify의 method 인자와 worker 생성/실행/파괴 순서를
Observer가 내보낸 ID와 대조했습니다. 최종 요청/worker 슬롯과 손실은 모두
0, loss epoch는 1, fixture는 검사 4,098회 후 정상 종료 0이었습니다.

## 실패와 남은 조건

- 첫 시도는 제공자를 먼저 정지시킨 뒤 추가 연결/AA 요청 단계에서 60초
  deadline에 도달했습니다. 마지막 표식만으로 connect와 후속 동기 조회를
  분리할 수 없어 실패로 보존했습니다. 이후 원본 util의 동기 조회 호출을
  확인하고 시험 순서와 호출 표식을 수정했습니다.
- 두 번째 시도는 첫 네 요청의 free와 슬롯 회수에 성공했습니다. 같은
  well-known 이름으로 새 연결을 만들 때 connect가 0을 반환해 종료 1이었습니다.
  최종 시험은 경우마다 다른 이름을 사용했습니다. 같은 이름의 재연결 문제를
  고쳤거나 원인을 확정한 것으로 세지 않습니다.
- timeout **만료**에 따른 원본 정리는 아직 검증하지 않았습니다. 이전
  45초 이상 지연 후 완료 기록과 이번 짧은 대기 창은 만료 증거가 아닙니다.
- 관측 슬롯 회수는 OEM callback userdata 할당의 누수 부재를 증명하지
  않습니다. 원본 세션 정리 오류의 영향도 해결하지 않았습니다.
- wire serial, bus/session/receiver 자격, 물리 센서와 폰 수용은 미검증입니다.
  실제 reply를 관측했다는 이유로 수신 시각을 측정 시각으로 바꾸지 않았습니다.
- 요청 후크는 비공개 fixture 설치입니다. 제품의 단일 설치/rollback,
  notify/worker 후크 전체의 예외 경계와 journal 연결은 후속 작업입니다.

## 고정된 근거

| 항목 | SHA-256 |
| --- | --- |
| 최종 작성 fixture | `2172e5cd993f971fc95e77f7f6fa5cb3de89bdaa4570a1b99688794d7ad67c4c` |
| 최종 진단 initramfs | `539138192a7796482cc7fdd4f45a896ed3a2876ad404de210746fba9eb3512c8` |
| 최종 콘솔 | `aff1196c61fbd10cccf3b268714391159fc0b1de88823a115f2cb10db389e9f5` |

비공개 `evidence/request-lifecycle-20260930/`에 r1/r2 실패, r3 입력 스냅샷,
빌드 명령, 독립 verifier와 결과를 보존했습니다. 원본·주소 포함 로그는
게시하지 않습니다. VM은 guest halt 이후 외부 180초 제한으로 종료됐으며
runner 124/QEMU 0은 성공 판정에 사용하지 않았습니다.

원본 실행과 검증은 작성자가 직접 수행했습니다. 이 단위에서 Claude를 새로
실행했다고 주장하지 않습니다. 주기적으로 확인한 저장소에서 기존
`e5d87c1` 이후 Claude의 추가 조사 커밋은 없었습니다.
