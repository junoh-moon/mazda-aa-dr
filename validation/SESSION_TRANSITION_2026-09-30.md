# 세션 전환을 가로지르는 원본 LDS 응답 — 2026-09-30

**원본 manager 정지·세션 파괴는 대기 LDS 요청을 취소하지 않았습니다.**
응답 전달 전에 세션과 manager를 재시작하면, 이전 세션에서 발행한 실제 요청의
LOCATION이 새 세션으로 송신됐습니다. 제품은 요청 시점 lifetime 1과 실제 송신
lifetime 2를 구별했고, 분석기는 `session_changed_since_issue`로 inconclusive를
반환했습니다. 이를 요청 소유권이나 폰 수용 성공으로 승격하지 않습니다.

## 실행 조건

- NA 74.00.324A 원본 Linux 3.0.35·rootfs·LDS·BLM·AA interface와 제품 preload를
  격리 QEMU VM에서 실행했습니다. 호스트 장치·네트워크·공유 디렉터리는 없었습니다.
  machine ID 조정 뒤 debugger를 분리했고 원본 서비스의 응답을 사용했습니다.
- 제품은 [세션 관측 구현](SESSION_PRODUCT_2026-09-30.md)의 동일 ELF입니다.
  SHA-256은 `8f065b8aff4405f56b8ae7b49266652cde3bd7ddb8e274eb3a4d21ee481acb9b`입니다.
  제품 자체의 cold-install·원본 함수 검사·파일 해시 조건을 유지했습니다.
  이번 변경은 검사와 기록이며 제품 소스·기본 모드·공개 ZIP은 바꾸지 않았습니다.
- 원본 singleton/getter로 얻은 manager·큐·세션을 원래 API로 초기화했습니다.
  원본 자동 요청 스레드를 시작하고, 실제 큐에 명시한 합성 mode 2 위치 한 건을
  넣었습니다. 이후 LOCATION의 좌표는 이 캐시입니다. 실제 LDS 응답은 mode·UTC·
  좌표·속도 등이 모두 0이었으며 유효한 GPS가 아닙니다.
- LDS를 정지하거나 응답 본문을 바꾸지 않았습니다. 작성 caller가 클라이언트
  dispatch 호출을 잠시 멈춰 **응답의 callback 전달**을 지연했습니다. 요청을
  받은 정확한 서비스 시각이나 물리 센서 지연을 측정한 시험이 아닙니다.
- 제품의 실제 pending method 1개를 확인한 뒤, 원본 큐에서 manager를 정지하고
  세션을 파괴했습니다. 두 작업 뒤에도 pending 1이 남은 것을 확인했습니다.
  재생성 경우에는 새 세션을 만들고 manager를 다시 시작한 뒤 dispatch를 재개했습니다.
  서비스·버스 연결을 새로 만들거나 OEM 객체 내부를 덮어쓰지 않았습니다.

## 두 경우의 실제 결과

각 경우는 별도 프로세스입니다. 같은 프로세스의 세션 저장소는 원본 getter로
얻은 동일 객체의 주소이며, 재생성은 lifetime 1→2로 구별됩니다.

| 관측 | 세션을 파괴한 채 유지 | 세션·manager 재시작 |
| --- | ---: | ---: |
| 실제 원본 LDS 요청/위치 | 5건 | 9건 |
| 파괴 이전 발행, 이후 callback 전달 | 1건 | 1건 |
| 해당 지연 요청의 POSITION 관측 | 1건 | 1건 |
| 해당 지연 요청의 하위 send | 0건 | 2건(type 1 LOCATION·type 21) |
| 지연 LOCATION | 없음 | 1건, 원본 하위 반환 0 |
| 요청 발행 당시 문맥 | lifetime 1 | lifetime 1 |
| 실제 하위 송신 대상 | 호출 없음 | lifetime 2 |
| journal drop / request loss / session fault | 0 / 0 / 0 | 0 / 0 / 0 |
| 최종 request / worker | 0 / 0 | 0 / 0 |

두 지연 요청 모두 원본 reply type·sender, 발행/관측 시각 순서와 소유 복사된
request ID를 검사했습니다. 최초 문맥은 callback 시점의 현재 세션으로 바뀌지
않았습니다. 해당 send의 request metadata는 POSITION과 같고 OBSERVE의
original/outgoing 바이트도 같았습니다. 새 세션에서 발행한 후속 요청은 lifetime 2를
가졌습니다. bus/session qualified 필드는 계속 unknown입니다.

정지 유지 경우의 하위 호출 0건은 오류 반환 256으로 대체한 것이 아닙니다.
원본 `StopSendVehicleData`는 실행 flag를 내리며, `RequestSendPosition`은 이
flag를 검사해 송신 전에 빠져나갑니다. 디스크의 원본 코드에서도 이 경로를
확인했습니다. 반대로 재시작 후의 원본 송신 경로는 현재 singleton의 RaceAap를
사용했습니다. 원본 API 호출 성공만으로 이전 요청이 새 세션에 적합하다고
판정할 수 없다는 근거입니다.

첫 전용 판정기는 정지 유지 경우에도 하위 LOCATION을 기대해 실패했습니다.
로그와 원본 조건문을 확인한 뒤 그 가정을 버리고, 실제 late POSITION과 하위
호출 부재를 따로 검사했습니다. 첫 실패와 수정된 판정기 모두 보존했습니다.
수정된 판정기는 두 경우의 전체 순서·실제 pending·지연 요청과 바이트·관측
loss·durable 종료를 검사합니다. 요청 문맥을 새 세션으로 바꾼 입력과 fixture의
종료 표식을 제거한 입력은 각각 거부했습니다.

fixture 두 개 모두 완료 표식과 exit 0을 남겼습니다. 바깥 runner의 145초 제한
종료 124/QEMU 종료 0은 통과 근거가 아닙니다. 일반 분석기는 정지 유지 로그에
`local_checks_pass`, 재생성 로그에 세션 불일치 2건과 `inconclusive`(exit 2)를
반환했습니다. 전자는 기록된 로컬 조건 검사이며 세션 자격 검증이 아닙니다.

## 지속적인 회귀와 외부 변경 통합

작업 브랜치는 `feat/session-observation` 하나를 유지했습니다. 중간 fetch에서
외부 master `afed202`를 확인하고 `e901a4a`로 통합했습니다. 기존 제품 관측과
새 NULL callback·errno·진단 종료 검사 양쪽을 보존했고, 중복 request-status
실행은 하나로 정리했습니다. 통합 `make test`는 Python **290개**와 C/C++ 검사를
생략 없이 통과했습니다. 고정 ARM에서 새 callback 회귀와 상태 조회 회귀도
통과했습니다. 외부 기록의 실행 주체·과거 숫자를 이번 결과로 바꾸지 않습니다.

[request ARM 회귀](../tests/adapter/request_hooks_arm_test.cpp)에
`session_transition`을 추가했습니다. 실제 제품 DSO의 세션 1에서 요청을 발행하고
callback을 지연시킨 뒤 세션을 파괴·재생성합니다. worker가 읽은 문맥은 이전
수명/상태 1/11이고, 현재 send 저장소 조회는 2/22여야 합니다. qualified 필드는
unknown이어야 합니다. 이 작성 fixture의 상태값 11·22는 폰 상태의 재현이 아닙니다.

- 직접 ARM adapter runner와 실제 제품 DSO의 요청 suite **14개**를 통과했습니다.
- 같은 DSO suite 14개를 stock loader/libc/shared C++ runtime chroot에서 통과했습니다.
- 원본 세션 문맥 복사를 제거한 별도 비공개 변형 빌드에서는 기존 사례들이
  통과하고 새 `session_transition`이 이전 문맥 검사에서 실패했습니다.
  변형 빌드는 테스트 민감도 확인용이며 제품이나 설치물에 반영하지 않았습니다.
- 마지막 검사 코드 변경 후 빌드 입력 회귀 41개도 통과했습니다.
  전체 제품 ARM runner를 다시 실행하지는 않았습니다. 제품 ELF가 이전 전체
  ARM 검증과 동일하고 고정 toolchain·빌드 입력을 확인했으며, 이번 범위의 ARM·
  stock 검사는 위와 같습니다. 실차·폰과 새 독립 에이전트 리뷰는 사용하지 않았습니다.

## 자료·정리와 다음 조건

비공개 `evidence/session-transition-20260930-0gcrsb`에 작성 caller·init·판정기,
실패/통과 로그, 소스 patch·해시와 도구 목록을 보존합니다. OEM 원본·전체 console·
분석 dump는 공개하지 않습니다.

| 실행 자료 | SHA-256 |
| --- | --- |
| 작성 ARM caller | `bd1d86f2d6935617c5e42080c069a11cf9a0d43e5a006d61e6e1c18110a0c00b` |
| VM initrd | `8514e073c8183a96abb6497e947f518f5df29af8632bd3478930e236b01c10ff` |
| 종료 후 console | `885791c15d4ac4f34311f79c7580fedf12ca407ff17dfebf3e69547e1a411afe` |

추가 도구는 전용 컨테이너에만 설치했습니다. 추가 패키지 120개·갱신 패키지
6개와 고정 toolchain 2,124개 파일을 기록했고, 컨테이너·임시 작업 디렉터리·
VM 이미지를 제거했습니다. QEMU와 컴파일러는 남기지 않았으며 호스트 패키지
설치는 없었습니다. 기존 Docker 이미지 목록과 컨테이너 목록이 설치 전과
같음을 확인하고 비공개 `cleanup.json`에 기록했습니다.

**아직 미완료:** 정상 폰 연결에 따른 lifecycle, 실제 receiver·provider 자격,
세션 전환 중 센서/계산 snapshot의 수명과 ASSIST 연결, 물리 센서의 시간·단위·
품질, 폰/앱 수용과 정상 전체 기동·복구입니다. 이번 실행으로 과거 모든 timeout의
원인을 해결했다고 세지 않습니다. `provenance=false`, `allow_assist=false`와
v1.0 미완료 상태를 유지하며, 제공된 펌웨어만으로 다음 경계를 검사합니다.
