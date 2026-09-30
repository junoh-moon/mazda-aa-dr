# 실제 제품의 요청 관측 설치·journal 연결 — 2026-09-30

이 변경은 앞선 [요청 연결 fixture](REQUEST_LINK_2026-09-30.md)와
[실제 요청 종료 검사](REQUEST_LIFECYCLE_2026-09-30.md)의 경계를 제품에 연결합니다.
대상은 NA 74.00.324A 한 버전입니다. 원본 API를 호출하는 별도 진단 프로그램과
실제 `libmx5dr.so`를 구분하며, 진단 프로그램에 adapter를 다시 링크하지 않습니다.

## 구현

- 첫 BLM 로딩의 동일한 cold lease 안에서 위치·static PostWorker·doWork·D1
  네 진입점과 다섯 GOT 슬롯을 설치합니다. BLM/interface에 더해 JCIDBUS와
  LDS data client의 원본 전체 해시, ELF ABI, 모듈 매핑, 원래 바이트와
  eager binding을 검사합니다. 기존 shim을 건너뛰거나 해시를 완화하지 않습니다.
- 이미 호출자가 있을 수 있는 JCIDBUS의 코드 페이지는 변경하지 않습니다.
  원본 대상과 Observer를 먼저 준비하고, 모든 실패 가능한 메모리 작업이 끝난
  뒤 정리 슬롯부터 submit 슬롯까지 게시합니다. 각 pointer store 뒤 ARM
  barrier를 둡니다. BLM 페이지 복구 불능은 기존 fatal 처리로 전달합니다.
- 원본 method submit 전에 관측을 등록하고 실제 notify에서 reply를 복사합니다.
  util이 오류를 지운 이후에도 실제 error name을 기록할 수 있습니다.
  worker·position의 정확한 살아 있는 포인터로 연결하며 원본 callback context,
  shared_ptr와 userdata 소유권은 유지합니다. submit 반환이나 경과 시간으로
  요청을 지우지 않으며 실제 method free에서만 회수합니다.
- 위치와 송신 journal에 같은 owned Trace를 기록합니다. health에는 관측 손실,
  ABI 오류, 남은 요청/worker 수를 포함합니다. JSON은 길이가 제한되며 문자열을
  escape하고, 잘린 이름과 미확인 필드를 표시합니다. 분석기는 오류 응답·관측
  실패를 집계하고 position/send metadata 불일치도 보고합니다.
- `provenance()`와 `allow_assist`는 기존대로 비활성입니다. 관측 ID·receipt
  시각을 bus/session 수명, receiver 자격, producer 측정 시각으로 승격하지
  않습니다. 이 변경은 v1.0 또는 실차 설치 승인에 해당하지 않습니다.

## 발견·수정한 실패

1. 첫 작성 wrapper 시험은 callback을 submit 내부에서만 실행했습니다. 독립
   리뷰가 제출 직후 잘못된 `request_end`를 추가해도 기존 10개 검사가 통과하는
   문제를 재현했습니다. 반환 후 pending 수와 늦은 callback을 추가했습니다.
   잘못된 mutation은 관련 8개 사례에서 거부됐습니다.
2. ReplyScope 생성자가 getter 호출 도중 예외로 끝나면 TLS에 죽은 scope가
   남았습니다. 생성 중 rollback RAII와 해당 파일의 exception cleanup 빌드를
   추가했습니다. 직접 Observer 회귀는 미존재/바깥 scope 복원을 검사합니다.
   실제 OEM getter가 예외를 던진다는 주장은 아닙니다.
3. 설치기의 GOT 저장 사이에 ARM 순서 보장이 빠져 있었습니다. 슬롯별 barrier를
   추가했으며 독립 리뷰가 pinned GCC의 `str` 뒤 `dmb sy`를 확인했습니다.
4. 첫 제품 VM은 제가 새로 추가한 BLM `dlsym` 검사 때문에 설치에 실패했습니다.
   해당 worker/vtable은 local ELF symbol이므로 외부 조회가 불가능합니다.
   전체 파일 해시·정확한 매핑/segment·runtime 바이트를 사용하도록 수정했습니다.
   JCIDBUS/data client의 실제 공개 심볼 검사는 유지했습니다.
5. 두 번째 VM은 제품 설치 후 정상 응답/SCRUB을 실행하다가 진단 프로그램이
   위치 기록에서 송신 전용 `choice`를 찾으면서 중단됐습니다. 제품 코드는
   변경하지 않고 판독기를 수정했습니다. 그 실행의 정상 시나리오는 실패로
   보존했습니다. 같은 실행의 제공자 부재 사례와 작성 DSO 13개는 통과했습니다.

6. 세 번째 VM에서는 전체 manager 실행 뒤 첫 연결의 네 요청을 취소·회수하고
   제공자를 재개한 다음 새 AA용 요청이 90초 fixture 제한 내 복귀하지 않았습니다.
   마지막 진단 지점은 새 연결 성공 이후입니다. 원래 API에는 동기 제어 조회도
   포함되며, 정확한 내부 대기 원인과 제품 후크의 관련성은 분리하지 못했습니다.
7. 네 번째 VM의 정상 경로에서는 위치 기록이 먼저 보인 시점에 아직 journal에
   도착하지 않은 송신 기록까지 세어 SCRUB 수를 너무 일찍 판정했습니다.
   진단 프로그램이 이벤트의 monotonic 시각으로 구간을 구분하고 실제 SCRUB
   기록을 기다리도록 수정했습니다. 제품 코드는 변경하지 않았습니다.

## 호스트·작성 ARM 검사

`make test`의 C/C++ 회귀와 Python 269개 검사를 완료했습니다. stock firmware와
새 ARM bundle 경로를 명시했으며 skip은 없습니다. 전체 `tests/run_arm_all.sh`도
완료했습니다. 기존 위치/send 예외·취소 8개와 새 request DSO 13개를 구분합니다.

새 13개는 정상/무관 callback/submit 실패/미실행 worker 파괴/notify 예외/
worker 예외/notify 취소/worker 취소/ABI 불일치/다른 worker/늦은 callback/
getter 예외/getter 취소입니다. 실제 제품 DSO의 숨겨진 자체 심볼을 외부 작성
caller에서 호출하며, 제품 ELF를 수정하거나 테스트 API를 공개 export하지 않습니다.

cold transaction 회귀는 14개 mprotect 호출의 개별 실패, 재검사 실패,
설정/할당 실패와 복구 불능을 검사합니다. 독립 리뷰의 추가 4,526개 실패 조합은
성공 697, 안전 복구 3,423, fatal 406이었습니다. 안전 복구는 원 코드·RX·GOT를
유지했고 fatal에서는 GOT를 게시하지 않았습니다. 별도 작성 ARM에서는 실제
private-memory mprotect를 적용한 회귀도 통과했습니다. 차량 메모리 고장이나
모든 커널 동작을 검증한 결과는 아닙니다.

최종 빌드는 `build/arm-request-product-20260930-final`입니다. 전체 회귀를 실행한
r3와 source 차이는 adapter README뿐이며 다섯 배포 ELF가 모두 byte-identical입니다.
최종 source/toolchain 입력 검사도 통과했습니다.

| 산출물 | SHA-256 |
| --- | --- |
| 실제 제품 `libmx5dr.so` | `42568553215bea8ef9998add0ba6e0f208c438455cfb576cfae4e808d883331b` |
| 원본 runtime VM의 작성 request DSO caller | `6d101fe83177a02d7d0f2a433a9c470b2e3708ce0843acc8270090966e22b6e8` |
| 최종 원본 API 외부 caller | `d01b76bb61674579e9a11c10ba79ffa790eb534a998ca4f8e427c11191884c6f` |

## 최종 원본 VM 검사

최종 r5에서는 원본 커널·rootfs·libc/C++ runtime 안에서 제품의 실제 dlopen
bootstrap, 원본 manager/queue, LDS API와 하위 AA send를 실행했습니다.
진단 caller는 원본 API를 호출하고 제품이 쓴 journal을 읽습니다. 입력 위치를
별도 adapter sink에 복사하거나 임시 hook으로 제품 설치를 대신하지 않습니다.

| 사례 | 완료·관측 |
| --- | --- |
| 정상 LDS + 원본 manager 자동 요청 | caller 종료 0, 실제 연계 위치 13개와 명시적 합성 seed 1개, send 153개, 총 journal 188개 |
| 제공자 부재 + 원본 manager | caller 종료 0, 실제 연계 위치 11개와 합성 seed 1개, send 126개, 총 journal 160개 |
| 후크 없는 축소 취소 비교 | 새 제공자에서 AA util 요청 8개, 두 연결의 free/resume 완료, callback 없음, 종료 0 |
| 제품 후크의 같은 축소 비교 | 요청 8개, 두 차례 pending 4→free 후 0, worker 0·loss 0, 연결 주소 재사용, 종료 0 |
| 작성 request caller + 실제 제품 DSO + 원본 공유 runtime | 13개 사례 각각 정상 종료 0 |

별도 검증기는 실제 파일 해시, 정상 종료 marker, JSON, position/send Trace
동일성, process-local ID 구분, 관측 시각 순서, 원본/변형 payload를 검사했습니다.
정상 reply type 1은 13개, 실제 ServiceUnknown/type 2는 11개입니다. util에서
오류가 지워진 뒤에도 원본 error name을 유지했습니다. bus/session과 wire serial은
null입니다. 실제 LDS 위치는 mode·UTC·위도·경도 0이며 유효한 GPS가 아닙니다.

두 journal 모두 durable capture 종료와 마지막 health까지 기록했고,
`dropped=0`, `audit_fault=0`, `loss_reasons=0`, 최종 request/worker 수 0을
확인했습니다. native OBSERVE 바이트를 보존했고 SCRUB 7건은 지정된
speed/bearing 바이트만 변경했습니다. 진단 caller가 OBSERVE 설정 뒤 자체
시험용 mode API로 SCRUB을 켰기 때문에 일반 분석기는 의도한
`scrub_without_scrub_config` 위반 7건을 그대로 보고합니다. 로그를 고치거나
분석기를 완화하지 않았습니다. 요청 metadata 오류·copy mismatch는 없었습니다.

**전체 manager 이후 반복 취소·재개의 timeout은 해결되지 않았습니다.**
최종 정상 파이프라인은 이를 `PIPE_LIFETIME_SKIPPED=separate_comparison`으로
명시하고, 새 LDS 제공자의 축소 API 비교를 별도로 실행했습니다. 축소 비교는
BLM manager/worker/send가 없어 앞선 timeout의 원인을 가려내지 못합니다.
따라서 최종 검증기의 `combined_lifetimes_verified`도 false입니다.
앞선 실패를 통과로 덮어쓰지 않으며 v1.0 후속 조사 항목으로 남깁니다.

최종 initramfs SHA-256은
`cfe6971b8b1df11876891f29cf7c81403b7ccf8913efba72bce2cb201e2cbe16`,
console SHA-256은
`567128dfb403343144d696936017142b4150491c79fd5e98102a2ec2d048ac6d`입니다.
원본 커널을 수정하지 않았고, kernel entry의 machine ID r1만 3837로 맞췄습니다.
네트워크·host 장치·공유 디렉터리가 없는 격리 VM입니다. guest 종료 뒤 runner는
240.023초에 외부 제한 시간으로 끝났습니다. runner 종료 124/QEMU 종료 0은
통과 근거가 아니며 위의 개별 정상 종료와 별도 검증기를 근거로 삼았습니다.

## 독립 검토·남은 범위

Codex 독립 리뷰 네 개가 설치/복구, ARM 호출·unwind, 시험의 거짓 통과,
journal/분석기 경계를 각각 검토했습니다. 발견된 위 결함과 시험 누락을 반영했습니다.
최종 VM 실행·원본 API 조사는 주 에이전트가 직접 수행했습니다.

Claude의 마지막 관련 독립 기록은 `e5d87c1`의
[LDS async 조사](LDS_ASYNC_2026-09-30.md)입니다. 이번 단위에서 새 Claude
실행이나 신규 Claude 리뷰가 있었다고 주장하지 않습니다. 공유 커밋과 validation
변화를 반복 확인했습니다.

정상 전체 SM/차량 기동, 물리 센서의 단위·생산 시각·품질, 같은 이름 재연결,
실제 timeout 만료와 모든 userdata 누수 여부, 정상 폰 연결·Galaxy S25/동글/
네이버 지도 수용은 이 검사로 해결되지 않습니다. 기존 원본 cleanup 오류도
[후크 없는 비교](AA_CLEANUP_BASELINE_2026-09-30.md)에서 재현된 미해결 항목입니다.

OEM 파일·전체 로그·initramfs는 공개하지 않습니다. 로컬 재현 자료와 실패 기록은
`evidence/request-product-20260930/`에 보존합니다. 공개 ZIP은 여전히
`v0.3.1-shadow.1`이며 이번 source 변경의 배포를 의미하지 않습니다.
