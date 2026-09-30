# 원본 LDS 요청과 AA worker 관측 연결 — 2026-09-30

실제 LDS 요청의 identity와 응답 오류를 원본 AA worker 큐 너머의 위치·send
관측까지 연결했습니다. 새 `request_observer` 컴포넌트와 adapter의 선택적
관측 인터페이스를 구현하고, 비공개 cold-start fixture에서 원본 경로를
실행했습니다. **제품 cold-install과 journal 연결은 아직 미구현**이며 공개
USB ZIP은 바뀌지 않았습니다. ASSIST·수신기·센서 자격의 완료도 아닙니다.

## 구현과 실제 실행 경계

`src/runtime/request_observer.*`는 기존 Ledger를 사용합니다. 원본 비동기
제출 전에 method를 등록하고, 원본 notify의 동기 scope에서 공개 JCIDBUS
getter로 reply의 종류·sender·오류를 복사합니다. 원본 PostWorker 전에 그
요청을 worker에 복사하고, 실제 doWork의 scope에서 원본 위치 포인터가
일치할 때 한 번 소비합니다. 중첩된 미관측 callback이나 다른 Observer가
바깥 scope를 가져가지 않도록 했습니다. scope는 생성 스레드의 stack에서
LIFO로 끝나야 하며 다른 스레드로 넘기는 객체는 포인터 없는 Trace입니다.

adapter는 이 Trace를 POSITION과 해당 동기 SEND 관측에 보존합니다. 이
정보는 기존 qualified Provenance와 별개이며 ASSIST 선택 조건에 쓰지
않습니다. 실패한 관측은 이전 Trace를 남기지 않습니다. production runtime은
아직 request reader를 설정하지 않으며 Observer도 ARM_SOURCES에 넣지 않았습니다.

비공개 fixture는 NA 74.00.324A의 원본 rootfs·Linux 3.0.35·JCIDBUS·LDS
제공자·BLM·aap_service를 격리 QEMU 7.2.22에서 사용했습니다. NIC·호스트 장치·
공유 폴더는 없습니다. 기존 kernel-entry machine ID 조정 뒤 debugger를
분리했으며 userspace debugger나 LDS private 객체 메모리 읽기는 사용하지
않았습니다. method/reply는 원본 공개 API에 전달하는 opaque 인자입니다.

원본 data client의 async-submit import를 감싸되 같은 connection·method·
context·timeout을 전달하고 실제 원본 notify를 한 번 호출했습니다.
method-free 두 함수, BLM PostWorker·doWork·D1의 실제 경계를 연결했습니다.
PostWorker는 static 함수로 r0에 간접 shared_ptr 인자가 옵니다. shared_ptr의
참조 수·원본 callback context·위치 padding을 수정하지 않았습니다. 이 BLM
ABI는 앞선 원본 객체 실행과 이번 해시 고정 구조 검사를 함께 사용했습니다.

기존 원본 Dbus/VDM 시작 API의 자동 LDS 요청과, 원본 AA용 공개 API로 명시한
네 요청의 동시 대기를 구분했습니다. 명시적 위치 cache seed 한 건은 합성
입력으로 계속 표시하고 요청 identity를 붙이지 않았습니다. 위치 값이 같다는
이유나 callback 순서로 요청을 맞추지 않았습니다.

## 최종 실행 결과

최종 r3의 두 case는 같은 VM·aap_service에서 별도 fixture 프로세스로
실행했습니다. 두 번째 case 전에 LDS를 종료하고 실제 NameHasNoOwner를
확인했습니다. 두 case 모두 네 원본 method의 동시 대기를 확인했습니다.

| 검사 | 정상 LDS | LDS 제공자 종료 후 |
| --- | --- | --- |
| 실제 연결한 요청 | 13건: 자동 9 + 명시 4 | 10건: 자동 6 + 명시 4 |
| 원본 method 동시 대기 최댓값 | 4 | 4 |
| method 주소 재사용 관측 | 7회 | 3회 |
| 요청을 붙이지 않은 합성 seed | 1건 | 1건 |
| 원본 native LOCATION 호출 | 11건 | 8건 |
| 기존 SCRUB 적용 | 7건 | 0건 |
| 종료 시 request/worker slot·관측 손실 | 모두 0 | 모두 0 |
| fixture 정상 종료 | 0 | 0 |

기능 assertion 평가는 각각 2,574/1,861회이며 반복 바이트·상태 검사를
포함합니다. 독립 시나리오 수가 아닙니다. guest 완료 후 외부 runner 제한으로
180.026초에 종료했습니다. runner=124·QEMU=0·timed_out=true를 통과 기준으로
삼지 않고 각 fixture의 검사와 종료 0을 대조했습니다.

원본 AA용 util이 ServiceUnknown을 NULL error·0값 위치로 평탄화하더라도,
새 관측에는 그 실제 오류와 버스 sender가 남았습니다. 오류 응답도 어느
worker와 위치·송신으로 이어졌는지 관측한 것이며 정상 위치로 승격하지
않습니다. 정상 응답의 mode·UTC·좌표 등 아홉 값도 모두 0이었습니다.

원본 공개 reply type getter의 값은 return=1·error=2입니다. 앞선 Claude
기록의 raw 내부 type 3/4와 서로 다른 표현입니다. 공개 reply serial getter는
이번 경로에서 값을 얻지 못했습니다. 관측의 serial은 unknown·0으로 남겼고,
fixture 전용으로 읽은 공개 method serial도 모든 응답에 0이었습니다.
두 값을 추정한 serial로 대신하지 않았습니다.

r3의 별도 fixture TLS는 notify에 실제로 전달된 method를 PostWorker 시점에
기록했습니다. 검사기는 그 원본 인자 연결과 관측 request/worker ID를
대조했습니다. 별도 dbus-monitor에서는 정상 요청 수·reply_serial별 원본
0값 응답·sender를 확인했습니다. 그러나 공개 getter가 serial을 제공하지
않으므로 개별 관측 ID와 wire serial의 매칭은 증명하지 못했습니다. 제공자
부재 시 monitor에는 요청이 보이지 않았으므로 그 case에 같은 버스 대조가
있었다고 주장하지 않습니다.

LOCATION의 실제 하위 반환은 0이었고 기존 OBSERVE의 바이트 보존과 SCRUB의
지정 필드 변경을 검사했습니다. 폰이 없는 로컬 세션이며 원본 서비스에는
미시작 세션의 요청 거절이 기록됐습니다. send=0은 폰 수용이 아닙니다.
추가 task 소멸·큐 stop/join·정상 fixture 종료를 확인했지만 원본 정리 오류도
그대로 보존했습니다. r3 두 case 모두 server-event 0x108과 thread-cancel
오류 3을 기록했습니다. 오류 없는 전체 AA 종료나 누수 부재의 증명은 아닙니다.

## 실패 기록과 회귀 검사

- r1은 fixture 빌드는 성공했지만 공개 export가 아닌 원본 notify 이름을
  dlsym으로 찾으려 하여 두 case 모두 관측 설치 전에 종료 1이었습니다.
  실패를 보존했습니다. 실제 공개 GetPosition으로 정확한 모듈을 확인하고,
  공개 submit 인자로 들어오는 원본 notify 주소·원본 바이트를 대조하도록
  fixture만 수정했습니다. 원본 파일·해시 검사를 약화하지 않았습니다.
- r2는 정상 요청 9건과 제공자 부재 요청 6건의 연결, 두 정상 fixture 종료,
  종료 시 빈 Ledger를 확인했습니다. r3는 네 요청 동시 대기와 별도
  notify 인자 대조를 보강한 후속 실행입니다. 최초 r3 결과 검사기는
  method serial과 wire serial의 일치를 요구하여 실패했습니다. 원본 getter가
  실제로 0만 반환한 기록을 보존하고, 그 항목은 미확인으로 분리했습니다.
  fixture·원본 반환·생산 코드를 바꾸어 serial을 만들어내지 않았습니다.
- 전체 host `make test`: C/C++ 회귀 통과. Python 267개 중 잘못 지정한
  bundle 경로 때문에 패키징 두 항목이 처음 생략됐습니다. 새로 검증한 ARM
  산출물 경로로 두 항목을 따로 실행하여 모두 통과했습니다.
- 다섯 제품 산출물의 fresh pinned GCC 4.9.1 ARM 빌드와 전체 ARM 합성
  suite가 통과했습니다. 기존 veneer의 인자·r0–r3 반환·errno 검사와 새
  adapter의 관측 전달·ASSIST 비승격 검사도 포함합니다. 이 합성 suite는
  새 비공개 fixture의 모든 실패 경로·ABI 검증을 대신하지 않습니다.
- 새 Observer는 metadata 소유 복사·serial 실패·중첩/다른 인스턴스·취소와
  주소 재사용·다른 스레드의 작업·동시에 열린 두 reply scope를 검사합니다.
  마지막 여섯 번째 그룹 추가 뒤 native·ASan+UBSan·TSan·pinned ARM에서
  해당 여섯 그룹을 다시 통과했습니다. 전체 suite를 그 뒤 다시 실행한
  것으로 세지 않습니다. ASan의 leak detection은 비활성입니다.

Codex가 직접 구현·실행·대조했습니다. 이번 변경의 새 독립 리뷰는 수행하지
않았습니다. Claude 커밋을 확인했으나 `e5d87c1` 이후 새 조사 기록은 없었습니다.
기존 Claude의 LDS 내부 관측은 [그 별도 기록](LDS_ASYNC_2026-09-30.md)의
범위이며 이번 실행 주체나 새 구현 리뷰로 세지 않습니다.

## 남은 구현과 범위

제품의 단일 cold-install/rollback transaction, journal 직렬화, 실제 취소·
timeout·연결 종료·예외 unwind에서의 새 경계, 손실 시 처리의 원본 ARM 검사가
남아 있습니다. 이번 정상 원본 method 종료와 합성 취소 검사를 실제 취소
경로의 완료로 세지 않습니다. 제품 설치와 bus/session/receiver 자격의
미구현은 새 헤더에도 명시했습니다.

실제 bus/session/receiver lifetime 자격, 센서 생산 시각·품질·단위와 보정,
유효 위치의 정확도·폰/앱 수용도 남아 있습니다. `provenance()`와
`allow_assist=false`는 유지하며 [v1.0 조건](../docs/V1_READINESS_KO.md)을
완료 처리하지 않습니다. 새 릴리즈·실차 검증을 수행하지 않았습니다.

소스 기준은 `9703f2ee480cf16a98c86fc80b691d30bb77ace5`와 이 변경입니다.
작성한 fixture/veneer·compiler command·고정 파일 구조 검사·image/run/결과
검사·소스 해시는 비공개 evidence에 보존했습니다. 원본 입력은
[AA manager 실행](AA_PIPELINE_2026-09-30.md)과 같으며 OEM 바이너리·전체 로그·
주소 기록을 게시하지 않습니다.

| r3 산출물 | SHA-256 |
| --- | --- |
| ARM fixture | `46e3dc2cd7315f21437d2b5c24676b9997aa15549dddd81643d7715f2c8b934a` |
| initrd | `d9b3ece006ee70ff905aad2a5f681c7541f4655eed1b3e1ecd08b5acd54d5d8a` |
| console | `a5e35159091d502fa0fccced4920fb8f1f8a934058e22cc0ee3d516425f87185` |
