# 세션 관측의 제품 연결 — 2026-09-30

NA 74.00.324A의 실제 세션 생성·파괴·상태 callback 관측을 제품 preload에
연결했습니다. 요청 발행 때의 유일한 live 세션 문맥을 복사하고, 송신 때의
실제 저장소 인자로 찾은 세션을 별도로 기록합니다. **두 관측의 일치는 요청의
세션 소유권이나 폰 수용을 증명하지 않습니다.** qualified session 필드는
여전히 unknown이며 `provenance()`의 false와 `allow_assist=false`를 유지합니다.
완성된 v1.0 또는 설치 권고가 아닙니다. 차량·폰·동글을 사용하지 않았습니다.

## 구현과 계약

- 기존 cold-install에 create/destroy GOT 두 개를 추가했습니다. 이제 전체
  transaction은 BLM code entry 4개·GOT 7개입니다. 기존 전체 파일 해시·매핑·
  원본 진입 바이트·다음 대상 검사를 유지하며 destroy를 create보다 먼저 게시합니다.
- 원본 76바이트 callback table 중 상태 callback만 감쌉니다. 나머지 18개 항목,
  NULL을 포함한 userdata, 전체 SessionInfo 포인터, 원본 호출 1회와 반환·errno를
  보존합니다. 정리 scope는 C++ 예외와 deferred pthread cancellation을 전달합니다.
- 생성별 문맥 64개는 프로세스 종료까지 재사용하지 않습니다. 같은 저장소 주소를
  다시 사용해도 늦은 이전 callback은 이전 문맥에 남습니다. 용량 초과·관측 경합·
  잘못된 callback·unwind는 sticky fault로 기록하고 원본 호출은 계속 전달합니다.
  무제한 재연결 관측을 구현했다고 주장하지 않습니다.
- 원본 함수 호출을 제외한 관측 경로에는 mutex·할당·I/O·소스 재시도 루프가
  없습니다. lock-free atomic이라는 뜻이며 실행 시간 상한을 보증하지 않습니다.
  상태와 event를 한 atomic 값으로 읽고, 생성/파괴 중에는 transition을 반환합니다.
- `request.session_context`의 basis는 `unique_live_context`입니다.
  `send_session`의 basis는 `send_storage`입니다. 요청의 옛 `session_lifetime`,
  `session_state`, bus 자격 필드를 이 ambient 관측으로 채우지 않습니다.
  journal·health·분석기가 unavailable/ambiguous/fault 및 서로 다른 수명을 보존합니다.

## 실패를 재현하고 수정한 부분

1. 새 관측의 구현 전 prepare 실패, 요청 시점 복사 누락, 실제 send 인자 미연결,
   7개 slot 경계 및 journal 누락을 각각 검사로 확인한 뒤 구현했습니다.
2. 전역 생성자보다 먼저 호출한 create의 상태가 나중에 초기화되어 사라지는
   오류를 재현했습니다. `constexpr Context`만으로는 호스트가 통과해도 고정
   GCC 4.9.1 ARM에서 여전히 실패했습니다. 상위 pool 전체를 constant-initialize하여
   수정했습니다. 우선순위 101의 실제 early constructor 회귀는 host·ARM·stock
   runtime에서 통과했으며 최종 ARM object에 `.init_array`가 없음을 확인했습니다.
3. GCC 4.9의 단일 memory-order CAS overload가 약한 C++ helper를 preload의
   동적 심볼로 내보내는 것을 실제 DSO 검사에서 잡았습니다. 성공·실패 order를
   명시하여 외부 노출을 제거했고 기존 숨김 검사는 유지했습니다.
4. stock runtime의 `std::thread` 시험에서 pure virtual abort가 발생했습니다.
   제품을 로드하지 않는 최소 2-thread 프로그램에서도 같은 실패를 재현했습니다.
   검사 코드를 `pthread_create/join/cancel`로 바꾼 뒤 아래 14개 사례를 모두
   통과했습니다. 이 harness 문제를 제품 수정으로 세지 않습니다.

## 최종 제품과 검사

작업 브랜치는 `feat/session-observation` 하나로 유지했습니다. 원격 master의
요청 잠금 분리와 journal MPSC 변경 `1884228`을 merge `cd8d445`로 반영한 뒤
새 checkout에서 빌드했습니다. 소스 patch와 빌드 입력 해시는 비공개 증거에
보존했습니다. 공개 설치 ZIP은 기존 v0.3.1-shadow.1 그대로입니다.

최종 `libmx5dr.so` SHA-256:
`8f065b8aff4405f56b8ae7b49266652cde3bd7ddb8e274eb3a4d21ee481acb9b`.
adapter 설명 문서 갱신 후에도 새 빌드를 만들었습니다. 빌드 입력 52개가 최종
소스와 일치하고 다섯 산출물 모두 실행한 빌드와 byte-identical임을 확인했습니다.

- 전체 `make test`: Python **282개**와 C/C++ 검사 통과, skip 없음.
  stock rootfs와 기존 USB fixture를 명시했습니다. 신규 early-init·7-slot rollback,
  외부 journal queue/close 경계와 기존 요청 동시성 검사도 포함합니다.
- 고정 GCC 4.9.1·ARMv7 softfp 전체 runner 통과, skip 없음. 실제 제품 DSO의
  기존 위치 예외/취소 8개·요청 13개와 새 세션 14개를 실행했습니다.
- 마지막 변경은 검사 프로그램의 pthread 전환뿐입니다. 이후 `test-adapter`와
  실제 DSO 세션 suite 14개를 다시 통과했습니다. 제품 ELF는 변경되지 않았습니다.
  같은 최종 suite 14개를 stock loader/libc/shared C++ runtime chroot에서도
  통과했습니다. 제품 코드를 테스트 실행파일에 다시 링크하지 않았습니다.
- 세션 사례: 정상/early callback·실패 반환·중첩 생성·동일 저장소·생성 중 파괴·
  용량 64/65·userdata 불일치·동시 reader·create/destroy/status 예외와 취소입니다.
  정상 사례에는 전체 callback 인자·errno·지연된 이전 callback 전달을 검사합니다.
- 최종 native TSan: callback 5,000건과 reader 2개, 생성 중 reader 검사 통과.
  최초 기본 ASLR 실행의 mapping 실패는 보존했습니다. 통과 실행은 해당 프로세스에만
  `setarch -R`을 사용했습니다. ASan+UBSan은 취소를 제외한 11개 사례를 통과했습니다.
  세 취소 사례의 sanitizer 검사는 생략했으며 일반 host·ARM·stock DSO 검사에는 포함했습니다.

## 원본 커널·LDS·AA 실행

최종 제품을 원본 Linux 3.0.35·rootfs의 QEMU/Sabrelite VM에서 preload했습니다.
호스트 장치·guest 네트워크·공유 디렉터리를 연결하지 않았습니다. kernel-entry
machine ID 조정 후 debugger를 분리했습니다. 원본 BLM/interface 해시와 함수
진입값을 검사했고, 제품 자체의 cold-install이 실제 GOT를 설치했음을 확인했습니다.
작성 probe는 원본 singleton/getter를 사용하며 OEM 객체 내부를 덮어쓰지 않습니다.

서로 다른 프로세스에서 아래 두 진단을 실행했습니다. callback 진단과 manager
진단을 동시에 정상 폰 연결로 실행한 것으로 합치지 않습니다.

| 최종 실행 | 실제 결과 |
| --- | --- |
| 상태 callback 진단 | 생성/송신/시작 반환 0, raw state 0(INVALID), stop 264, 파괴 후 핸들 NULL을 두 주기에서 관측 |
| callback별 수명 | 같은 저장소의 첫·두 번째 생성은 lifetime 1·2, 새 생성은 상태 unknown에서 시작하고 각각 event 1을 수신 |
| 원본 manager→LDS→worker | 각 수명에서 원본 요청 6건씩, 총 12건의 mode/UTC/좌표 0 응답을 관측 |
| 명시적 합성 seed | 각 주기 1건씩 원본 BLM callback을 큐에 게시; mode 2 및 합성 입력임을 별도 기록 |
| journal position/send | position 14건, send 104건; 요청 metadata 복사 일치, request 문맥과 실제 send 저장소의 수명 일치 |
| mode 0의 native LOCATION | 수명별 3·6건. 합성 seed를 캐시 재송신한 것으로, 유효한 새 GPS 표본이 아님 |
| 원본 바이트·송신 반환 | OBSERVE의 original/outgoing 일치, 원본 반환 0 보존 |
| 관측 정리 | 두 진단 모두 journal drop 0, request loss 0, session fault 0, 최종 request/worker 0과 durable capture 종료 확인 |

상태 진단의 시작 인자는 304바이트 합성 0값입니다. transport·장치 초기화가
실패하는 경로이며 정상 연결이 아닙니다. 원본 callback이 큐에 게시한 worker는
이 진단에서 실행하지 않습니다. manager 진단은 별도로 원본 큐를 시작하여
manager 초기화·시작·정지·종료를 큐에서 호출했습니다. 각 정지 후 요청과 worker가
배출되었음을 확인하고 마지막에 원본 큐 stop·pthread join을 완료했습니다.
manager 진단에서 세션 상태는 unknown이며 `start_session`을 호출하지 않았습니다.

첫 중간 VM은 상태 callback을 600ms 만에 판정하여 실패(exit 91)했습니다.
실패 로그를 남기고 실제 callback까지 최대 10초 기다리도록 probe를 고쳤습니다.
다음 중간 실행의 첫 callback은 약 7.4초 뒤 도착했습니다. 최종 제품에서는
약 2.3초·0.8초 뒤 관측했습니다. 고정 지연이나 반환 0으로 성공을 만들지 않았습니다.

최종 fixture 두 개는 각각 exit 0과 완료 표식을 남겼습니다. 바깥 VM runner의
175초 제한 종료는 통과 기준이 아닙니다. 전용 검사기는 위 순서·identity·payload·
health·durable 종료를 검증했습니다. 일반 분석기는 pipeline에 local_checks_pass,
별도 callback 송신에는 위치 요청 문맥 부재로 inconclusive(exit 2)를 반환했습니다.
원본 SDK의 mutex/semaphore/transport 오류는 비공개 원본 로그에 보존했습니다.
정상 전체 SM 그래프, 오류 없는 전체 종료 또는 누수 부재의 증명은 아닙니다.

## 증거와 도구

비공개 `evidence/session-product-20260930-abu5lb`에 실패/통과 로그, 작성 probe·
판정기·소스 patch·빌드 manifest·설치 도구 목록을 보존합니다. OEM 이미지·전체
로그·분석 dump는 공개 git에 추가하지 않습니다.

| 최종 자료 | SHA-256 |
| --- | --- |
| 작성 ARM probe | `89abb5be622d5b54769e6af8785ba0a498e8fd4da2aa03ea872f0172b5f3e080` |
| VM initrd | `c98e65cfe70ec3e0b0a346d14f9da363254e6acb26c781b4a98a53a7bbcb041f` |

호스트 패키지 설치는 없었습니다. 전용 컨테이너에만 패키지 120개를 추가하고
6개를 갱신했으며, 고정 toolchain 2,124개 blob을 받았습니다. 컨테이너·도구체인·
임시 작업 폴더와 이번에 만든 VM 이미지 세 개를 제거했습니다. 재현용 작성 소스·
로그·산출물·이미지 해시는 비공개 증거에 남겼습니다. Docker 컨테이너와 이미지
목록을 작업 전후 대조했고 기존 기반 이미지는 보존했습니다.

**남은 범위:** 실제 요청의 session/receiver 소유권과 세션 전환을 가로지르는
원본 지연 응답, 물리 센서의 단위·생산 시각·품질, 폰/앱 수용, 정상 전체 기동·
복구입니다. 늦은 callback·동시 생성의 작성 회귀를 원본 장치 실행으로 세지 않습니다.
새 독립 에이전트 리뷰와 실차 시험은 하지 않았습니다. 파일만 사용하는 현재
범위에서 얻은 관측이며 ASSIST 연결 또는 v1.0 완료를 선언하지 않습니다.
