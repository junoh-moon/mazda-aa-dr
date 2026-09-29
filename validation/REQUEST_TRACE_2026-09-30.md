# 요청 관측 identity 컴포넌트 검사 — 2026-09-30

`src/runtime/request_trace.*`에 요청·응답·비동기 작업의 관측 정보를 연결하는
독립 C++ 자료구조를 구현했습니다. 같은 위치 값이나 완료 순서로 요청을
추정하지 않고, 실제 생존 경계에서 제공할 객체 식별자와 발급한 token을 씁니다.
현재 제품에는 연결되지 않았습니다. OEM hook·취소 ABI·실제 요청 출처의
연결은 헤더의 명시적인 TODO이며 `provenance=false`, `allow_assist=false`를
완료된 구현으로 바꿔 표현하지 않습니다.

## 구현 계약

- Ledger 하나의 수명 안에서 요청 64개와 대기 작업 64개를 보관합니다.
  용량은 최초 관측 예산이며 OEM의 최대 동시 요청 수를 입증한 값이 아닙니다.
- 요청 시작 당시의 정보를 복사하고, 응답 정보도 소유한 값으로 복사합니다.
  후속 전역 session·receiver 값을 과거 요청에 덧붙이지 않습니다. 알 수 없는
  메타데이터는 unknown이며 잘린 문자열은 완전한 sender 식별자로 쓰지 않습니다.
- 원본 비동기 제출 전에 요청을 등록하고, 원본 작업 제출 전에 응답을 작업에
  복사하는 계약입니다. 제출이 반환하기 전에 응답·작업이 실행될 수 있습니다.
  실제 경계와 호출 ABI를 확인하는 제품 연결은 아직 없습니다.
- 실제 요청 종료, 작업 소비 또는 작업 파괴만 슬롯을 회수합니다. 경과 시간이나
  취소 의도만으로 살아 있을 수 있는 객체를 지우지 않습니다. 소비된 작업은
  동기 scope 하나에서 정확한 위치 포인터로 한 번만 관측할 수 있습니다.
- 경합으로 놓친 이벤트·용량 초과·identity 충돌은 관측 epoch를 바꿉니다.
  이전 슬롯은 실제 정리 경계까지 남으며 새 객체의 주소 재사용과 연결되지
  않습니다. ID/epoch 고갈도 재사용 가능한 값으로 wrap하지 않습니다.
- 공유 상태를 사용하는 관측 API는 mutex trylock을 최대 한 번 시도하며
  고정 용량만 탐색합니다. 동적 할당,
  I/O, sleep, OEM 호출을 하지 않고 errno를 보존합니다. lock-free 원자 연산이
  wait-free나 실행 시간 상한을 보장한다는 뜻은 아닙니다.
- 반환값은 관측 결과입니다. 실패해도 호출자가 원래 동작을 정확히 한 번
  수행해야 합니다. 이 컴포넌트는 callback·userdata·OEM 메모리를 소유하거나
  수정하지 않으며 qualified Provenance와 ASSIST 허용을 결정하지 않습니다.

## 실행 검사

새 테스트는 `make test`의 `test-runtime`과 `tests/run_arm_all.sh`에 연결했습니다.
생산용 `ARM_SOURCES`에는 추가하지 않았습니다.

- 전체 host `make test`: C/C++ 실행 통과. Python 267개 중 초기 21개는
  fixture 경로가 없어 생략됐습니다. 게시된 다섯 바이너리의 해시를 대조한
  release fixture로 한 항목을 실행했고, 원본 네 파일의 고정 해시를 확인한
  뒤 올바른 rootfs 경로로 나머지 원본 설치 20개를 모두 실행했습니다.
  추가 실행까지 포함하면 최초 생략 항목은 모두 검사했습니다.
- 최종 native 회귀는 Apple Clang 17의 C++11 빌드, ASan+UBSan, TSan에서
  각각 같은 12그룹을 통과했습니다. ASan 실행에서 LeakSanitizer는 비활성입니다.
  입력 파일의 실행 전후 해시가 같으며 native sanitizer 결과를 ARM 실행으로
  세지 않습니다. 최종 Linux 통합 `test-runtime`도 통과했습니다.
- 고정 GCC 4.9.1·ARMv7/softfp·기록된 sysroot와 QEMU user 7.2.22에서 새
  12그룹의 컴파일·실행이 모두 exit 0이었습니다.
  `-Wall -Wextra -Werror -pedantic`을 유지했고 ARM ELF·ABI와 소스 전후 동일성을 확인했습니다.
  이 실행은 직접 작성한 합성 프로그램만 사용했습니다. 이번 변경에서 전체
  production ARM suite나 OEM 서비스·차량을 다시 실행했다고 주장하지 않습니다.

12그룹은 같은 값의 서로 다른 요청과 역순 응답·작업, 주소 재사용, 정확한
위치 포인터, 단회 scope, 실제 종료 전 pending 유지, 용량·충돌·경합 손실,
ID/epoch 고갈, unknown·오류·잘린 문자열, 제출 반환 전 응답과 다른 스레드의
작업, 원본 호출 독립성을 검사합니다. 네 스레드가 각각 1,000회 합성 원본
동작을 수행하여 총 4,000회를 확인합니다. 성공한 관측은 실제 요청과 일치해야
하지만 경합 중 관측 성공 수 자체는 일정하지 않으며 성공률 보장이 아닙니다.

리뷰 후에는 소비 시 BUSY가 된 scope를 다시 쓸 수 없음, null out의 BAD_INPUT은
scope를 소비하지 않음, zero token 거부와 이전 출력 초기화, NUL 없는 소유
문자열의 정규화·입력 보존을 기존 그룹 안에 추가했습니다. 이는 별도 12그룹을
새로 만든 것이 아닙니다. 최초 전체 host 검사 뒤의 이 테스트 보강은 최종
`test-runtime`·native·ARM 부분 검사로 확인합니다.

초기 host 시도는 선택한 컨테이너에 git이 없어 중단됐고, 의존성이 있는
컨테이너에서 전체 검사를 통과했습니다. 첫 ARM 컴파일은 GCC 4.9.1의 aggregate
초기화 경고로 실패하여 동일한 value initialization 표기로 수정했습니다.
다음 ARM 실행은 exit 0이었지만 runner가 요약 PASS 문장까지 13번째 그룹으로
세어 실패를 보고했습니다. 이 실패 기록과 수정 후 최종 결과를 구분합니다.

최종 실행은 native r6, authored ARM r4와 host `test-runtime` r6입니다.
마지막 독립 리뷰의 trylock 횟수 문구를 보정한 뒤 아래 동일한 입력으로
검사를 고정했습니다. 구현 본문은 초기 독립 리뷰 뒤 변경하지 않았습니다.

| 최종 입력·합성 산출물 | SHA-256 |
| --- | --- |
| `src/runtime/request_trace.h` | `3230fe5c3d3c57d3f5f0acb27b5e06afbd98720702892c4f2657a224acda0010` |
| `src/runtime/request_trace.cpp` | `3efc17b7e4ef9a2db3b32068644417b8d0dd72deda9526493e0a994673f3489b` |
| `tests/runtime/test_request_trace.cpp` | `208f0e4df5bda5c8607b1b52c279aba0be0dc5eedd82eb850ae92e615a51a63a` |
| 합성 ARM test | `ddebbbea31056ba30fd6af209157572efe89a642d87f07ec86d869328d4bcac0` |

## 독립 리뷰와 남은 경계

두 Codex 리뷰어가 일반 C++ 자료구조를 독립 검토했고, 실제 Claude Code도
소스·테스트를 도구 실행 없는 텍스트 리뷰로 검토했습니다. 원본 OEM 함수나
차량을 Claude가 실행한 것은 아닙니다. Claude는 두 초기화 문구 수정 및
후속 경계 보강 전의 소스를 검토했습니다. 확정 P1/P2는 발견되지 않았으며,
경합 시 정리 함수의 STALE 반환 계약과 추가 경계 검사를 보강했습니다.

검사에서 사용한 요청·응답·파괴·원본 호출은 직접 작성한 합성 fixture입니다.
원본 lifecycle ABI나 실제 설치된 hook의 forwarding 증거로 세지 않습니다.
원본 요청→reply→queue→position의 연결, 수신기·세션 수명, 생산자 시간과 센서
자격, 실제 폰 수용과 차량 복구는 남아 있습니다. [v1.0 완료 조건](../docs/V1_READINESS_KO.md)은
그대로 유지하며 공개 SHADOW ZIP의 바이트와 기본 동작도 이 변경으로 바뀌지 않습니다.
