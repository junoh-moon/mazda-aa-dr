# 동시 상태 조회 회귀와 원격 잠금 분리 통합 — 2026-09-30

진단용 상태 조회가 실제 요청 관측을 잃게 만드는 경로를 별도 회귀로 재현했습니다.
작업 중 원격 `05ddd5ff1340b4b9042e6d59e84b8699bd182da4`가 같은 영역의 잠금
분리를 제공하여 fetch/pull로 반영했습니다. **최종 제품 코드는 그 원격 구현이며,
이번 추가 변경은 동시 조회 회귀·실행기·검증 기록입니다.** 로컬에서 먼저 시험한
대체 구현의 코드와 로그는 비공개 exploration 자료로만 보존했습니다.

기존 펌웨어/세션 진단 브랜치와 원격 master의 병합 기준은 `642a89f`입니다.
외부 실행·독립 리뷰 기록과 이번 직접 실행을 구분합니다. 원격 수정의 설계와
기존 OEM VM 한계는 [요청 잠금 분리 기록](REQUEST_CONTENTION_2026-09-30.md)을 따릅니다.

## 새 회귀

[`test_request_status.cpp`](../tests/runtime/test_request_status.cpp)는 실제 Ledger를
두 상태 조회 스레드와 한 이벤트 스레드에서 호출합니다. 합성 method·worker·
position 주소로 생성→응답→게시→worker 소비→위치 소비→실제 요청 정리를
2,000회 수행합니다. 반환, 요청/worker 식별자, 발행 당시 metadata, 최종 슬롯
회수와 손실 0을 검사합니다. 조회의 errno, 허용된 BUSY/STALE의 빈 출력,
정상 출력의 count·epoch·손실도 확인합니다.

잠금이나 원본 API를 성공 stub으로 바꾸지 않습니다. 실제 pthread 실행의
스케줄에 의존하는 동시성 회귀이며 모든 스케줄의 증명은 아닙니다. 원격에
추가된 결정적 handoff/publication 회귀도 함께 실행합니다.

동일한 최종 회귀를 수정 전 `ccfb255`의 Ledger와 컴파일하면 native와 pinned
ARM 모두 첫 주기에서 실패합니다. 두 실행 모두 이벤트 실패 6건·loss 1,
assertion 종료 134입니다. 새 검사가 기존 결함을 거부함을 직접 확인했습니다.

| 최종 구현 실행 | 완료 주기 | 상태 조회 시도 | 이벤트/상태 오류·손실 |
| --- | ---: | ---: | --- |
| 전체 GNU host 검사 | 2,000 | 24,598 | 모두 0 |
| 고정 ARM suite | 2,000 | 125,734 | 모두 0 |
| 원본 libc/C++ runtime | 2,000 | 109,552 | 모두 0 |

조회 횟수는 실행별 스케줄의 결과이며 처리량·차량 지연 상한으로 사용하지 않습니다.
`make test`와 `tests/run_arm_all.sh`에 이 검사를 연결했습니다.

## 전체 검사와 실행 범위

- `make test`: Python **281개**와 전체 C/C++ 검사 통과, 생략 0입니다.
  추출한 stock rootfs·기존 USB fixture를 명시했습니다. Python 집계는
  41/53/28/1/10/118/30이며 새 상태 회귀는 별도 C++ 검사입니다.
- GCC 4.9.1 고정 도구체인으로 다섯 제품을 새로 빌드하고 전체 ARM suite를
  통과했습니다. 시작·종료의 `release_verified=true`, 원격 추가 publication
  회귀, 실제 제품 DSO의 위치 예외/취소 8개와 request wrapper 13개를 포함합니다.
  ARM 생략도 0입니다.
- 최종 상태 회귀, 기존 Trace 12그룹과 handoff 6그룹은 native TSan을
  통과했습니다. 새 상태 회귀는 ASan+UBSan도 통과했습니다. 초기 PIE TSan은
  `unexpected memory mapping`으로 시작하지 못했으며 실패 로그를 보존했습니다.
  최종 sanitizer 실행 파일에만 `-fno-pie -no-pie`를 적용했습니다.
- 제공 펌웨어에서 추출한 stock rootfs 안으로 chroot하고 정적 QEMU user로
  새 ARM 회귀를 실행했습니다. 원본 libc·loader·libstdc++를 사용한 **작성
  코드 검사**입니다. 원본 커널 VM, OEM manager/센서 서비스와 물리 차량을
  재실행한 검사가 아닙니다.
- 새 production-flags `request_trace.o`에는 `libatomic`/`__atomic*`나 blocking
  `pthread_mutex_lock` 미해결 참조가 없습니다. 컴파일러 내부 원자 재시도까지
  없다는 주장이나 wall-clock 상한은 아닙니다.

새 `libmx5dr.so` SHA-256은
`944304343c73b99dbd3c4b39f1eda3fd17be2fc662d93d3291a5bfe3b8dd00f3`이며,
원격 검증 기록의 제품과 일치합니다. 공개 ZIP은 바꾸지 않았습니다.

## 추가 도구 정리와 남은 조건

기존 캐시 이미지를 사용한 전용 컨테이너에 패키지 **120개**를 추가하고
**6개**를 갱신했습니다. 고정 도구체인 2,124개 blob의 검증 내역과 설치 전후
패키지 이름·버전을 보존했습니다. 호스트 패키지 설치는 없습니다. 준비 뒤
컨테이너 네트워크를 제거하고 검사했으며, 완료 뒤 컨테이너·도구체인·임시
복제본·추출본·빌드 폴더를 모두 삭제했습니다. Docker 이미지/컨테이너 목록의
복원도 확인했습니다.

Git에서 제외된 `evidence/request-status-20260930-1hsvec/`에 최종 integrated
로그·source/artifact 해시·재현 스크립트·설치/삭제 내역을 보존합니다. 초기
대체 구현의 결과는 최종 제품 검증과 구별합니다. 이 단위에서 새 독립 에이전트나
Claude 리뷰를 실행하지 않았습니다.

**미완료:** 과거 OEM VM 실패의 정확한 호출 조합, 같은 표의 동시 이벤트 손실,
별도 journal 큐 drop, 센서 생산 시각·단위·보정, 요청별 receiver/session 자격,
폰/앱 수용과 실제 기동·복구입니다. 상태 조회 회귀 통과로 이를 닫지 않습니다.
live ASSIST와 qualified provenance는 비활성이며 v1.0은 미완료입니다.
