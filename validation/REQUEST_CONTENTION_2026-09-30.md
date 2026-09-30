# 요청 관측 잠금 분리와 원본 VM 재검사 — 2026-09-30

요청 정리·worker 실행·상태 조회가 같은 mutex에서 서로 관측을 잃게 만드는
경로를 작성 코드에서 재현하고 분리했습니다. 최종 제품의 원본 VM 두 실행에서
요청 관측 손실은 0이었습니다. **첫 실행은 별도의 journal 큐 drop으로 MODEL
계산이 중단됐습니다. 두 번째 성공으로 그 실패를 닫지 않습니다.**

기준 소스는 `ccfb255`입니다. 이 변경은 v1.0 완료·차량 승인·폰 수용이 아니며,
공개 ZIP은 여전히 `v0.3.1-shadow.1`입니다. ASSIST와 qualified provenance는
비활성입니다. 원본 바이너리·전체 로그·비공개 fixture는 게시하지 않습니다.

## 재현과 한계

[이전 센서→MODEL 검사](SHADOW_RUNTIME_2026-09-30.md)의 r4/r5에는
`LOSS_CONTENTION`이 기록됐지만, 어느 요청 관측 경계끼리 겹쳤는지는 기록되지
않았습니다. 이 단위에서 그 과거 호출 조합을 확정한 것은 아닙니다.

기존 Ledger의 실제 unlock 직전만 작성 테스트 scheduler로 정지했습니다.
status→worker_enter, request_end→worker_enter, worker_enter→request_end
세 조합 모두 contender가 BUSY, 최종 epoch 2·loss 1이 됐습니다. 같은 mutex가
서로 독립적인 작업을 잃게 하는 구조를 확인한 결정적 재현입니다.

잠금을 인위적으로 지연하지 않은 별도 작성 인계 5,000회는 기존 코드에서도
관측 5,000회·loss 0이었습니다. 기존 제품에 **우리 Ledger의 호출·잠금 보유자
카운터만** 추가한 비공개 진단 VM 세 번도 경합을 재현하지 못했습니다.
각각 실제 manager 요청 10·11·70개였고, 마지막은 관측 시간을 60초 늘렸습니다.
진단의 시간 영향과 획득/해제 순간 보유자 미확인 가능성이 있습니다.
이 결과로 과거 r4/r5를 통과로 바꾸거나 실제 실패 경계를 추정하지 않습니다.

## 변경

- 기존 mutex에는 request 표와 64비트 ID/epoch 발급을 남기고 worker 표에
  별도 mutex를 둡니다. 각 표의 trylock은 한 번만 시도합니다.
- worker_post는 request 잠금 안에서 reply와 ID를 소유 복사한 뒤 잠금을
  풀고 worker 표에 게시합니다. 두 잠금을 동시에 잡지 않으며, 두 번째
  단계와 반환 전에 세대 손실을 검사합니다. method가 사라지거나 같은 주소가
  다시 등록돼도 이미 복사한 reply는 바뀌지 않습니다.
- position_take는 TLS 소유 scope를 한 번 소비하고 정확한 위치 포인터와
  원자 epoch/pending/exhausted만 검사합니다. status도 표 잠금을 잡지 않습니다.
- request 소유자가 새 epoch를 먼저 공개한 다음 pending loss를 지웁니다.
  이 사이에는 새 epoch의 token이 발급될 수 없습니다. clear 직전의 추가
  손실은 함께 처리하고, 직후의 손실은 다음 invalidation으로 남깁니다.
- status의 두 count·손실 이유·고갈·갱신 중 표시는 하나의 atomic32 워드입니다.
  epoch/pending/state가 읽는 동안 변하거나 갱신 중이면 zero 출력과
  STALE/BUSY를 반환합니다. pending 상태의 논리 epoch는 이미 금지된 이전
  token 다음 세대이며, 다음 request 처리로 확정돼도 숫자가 되돌아가지 않습니다.
  최대 epoch에서는 wrap하지 않고 고갈을 보고합니다.

실제 same-table 경합·용량 초과·충돌의 손실 검사는 유지합니다. 슬롯은 실제
end/consume/destruction에서만 회수하며, 원본 callback·반환·errno·소유권 계약은
바꾸지 않습니다. 무제한 source CAS 반복, callback의 blocking lock·할당·I/O를
추가하지 않았습니다. 원자 연산 자체의 wait-free/시간 상한을 주장하지 않습니다.

## 회귀와 독립 리뷰

최종 GNU host `make test`는 Python 270개와 C/C++를 생략 없이 통과했습니다.
고정 GCC 4.9.1의 전체 authored ARM suite도 생략 없이 통과했습니다. 실제 제품
DSO의 위치 예외/취소 8개와 요청 wrapper 13개 검사를 포함합니다.

새 handoff 회귀는 실제 cpp의 unlock만 테스트 scheduler에 연결합니다. 잠금을
잡은 채 멈춘 request/worker의 반대쪽 처리, 두 단계 post 사이 pending/확정
손실, 소유 scope와 status의 무잠금 접근, 정상 method end·주소 재사용 뒤 owned
reply 보존을 검사합니다. reader publication 상태 주입과 실제 writer 실행은
구분했습니다. 최종 여섯 handoff 그룹은 native ASan+UBSan·TSan도 통과했습니다.
별도 Trace 12그룹 TSan도 통과했으며 ASan의 LeakSanitizer는 비활성입니다.

독립 리뷰가 기존 검사도 통과하는 잘못된 변형 두 개를 발견했습니다.

1. position_take가 worker mutex를 다시 획득해도 통과했습니다. 두 mutex를
   모두 잡은 상태의 scope 소비·status 회귀를 추가해 native/ARM에서 거부했습니다.
2. pending을 epoch 공개보다 먼저 지워도 통과했습니다. 실제 cpp만 GCC의
   `-fno-inline-atomics`로 별도 컴파일하고, **테스트 실행 파일에서만** static
   libatomic의 exchange를 감쌌습니다. clear 앞/뒤와 추가 worker 손실 유무의
   네 실행을 검사합니다. 정상은 host/ARM 통과, 조기 clear 변형은 양쪽에서
   exit 134로 거부됩니다. 제품 소스에 테스트 hook을 넣지 않았습니다.

publication scheduler는 GCC 전용입니다. Clang에서는 명시적 exit 77 생략을
확인했습니다. 위 최종 전체 host는 GNU 컴파일러여서 이 검사도 실행했습니다.
ARM runner에서는 생략을 통과로 취급하지 않습니다.

Codex 독립 리뷰 네 건을 반영했습니다. 설계·세대 처리, 거짓 통과 검사, ARM
ABI/원자 연산, 최종 독립 정적 검토를 구분합니다. 마지막 정적 리뷰의 정상
주소 재사용 회귀 제안은 후속 전체 host/ARM와 native sanitizer로 검사했습니다.
검토된 범위에서 미해결 P1/P2는 보고되지 않았습니다. 이는 아래 journal 누락이
해결됐다는 뜻이 아닙니다.

독립 ARM 검토는 epoch의 8바이트 정렬과 LDREXD load·LDREXD/STREXD store를
확인했습니다. production-flags request_trace 객체에는 `libatomic`·`__atomic*`·
blocking pthread_mutex_lock 의존성이 없습니다. 컴파일러 내부 원자 retry는
존재합니다. 별도 authored wrapper 실행과 실제 제품 DSO/원본 VM 실행을
동일한 증거로 부르지 않습니다.

## 최종 제품의 원본 VM 두 실행

정확한 NA 74.00.324A 커널/rootfs의 기존 격리 구성, 원본 VIM/VBS/LDS/settings/
aap_service와 실제 제품 DSO를 사용했습니다. 원본 manager/queue와 기존 VBS
callback을 실행했고 센서·GPS 값만 명시적으로 합성했습니다. 제품 요청 카운터
진단과 센서 sendto 진단은 최종 이미지에서 로드하지 않았습니다. 주 에이전트가
직접 실행했으며 userspace debugger·호스트 네트워크/장치/공유 폴더는 없습니다.

| 실행 | 관측 | 결과 |
| --- | --- | --- |
| product-r1 | raw 1,089건, 기록 구간 센서 순번 누락 0, 요청 loss 0 | journal dropped 1·audit_fault 1로 MODEL 중단. 재기준점 후 계산 검사 실패, caller exit 1 |
| product-r2, 같은 이미지 | raw 927건·연속 순번, 요청 loss 0 | 정차 보정·직진/회전·yaw 단절·재기준점 복구, caller 검사 11,268회·exit 0 |

r1의 첫 dropped health는 monotonic 34.589276183s입니다. 요청 관측기는
epoch 1·loss 0인 상태였습니다. 이 counter는 runtime journal 입력의 trylock
실패 또는 큐 포화 때 증가합니다. 어느 쪽인지 아직 분리하지 않았습니다.
audit 중단이나 실패 검사를 제거하지 않았고, raw와 실패 이유를 보존했습니다.

r2는 journal 680행·유효 MODEL snapshot 71개입니다. 실제 LDS mode 0 위치
10건과 요청 없는 합성 GPS 4건을 구분합니다. 끝의 dropped/audit_fault/요청
손실과 남은 request/worker는 0이고 현재 boot의 durable capture 종료를 확인했습니다.

기존 독립 검증기로 raw→합성 API 대응, 48바이트 preview와 일정 속도·각속도
해석식을 대조했습니다. 직진 24.87478591m, 회전 21.55562226m/0.3307867484rad,
복구 후 18.2168632m의 **합성 계산**이며 위치식 잔차 최대 약 1.53e-6m였습니다.
물리 위치 정확도 수치가 아닙니다. native LOCATION 송신은 이번에도 0건으로,
폰 수용·LOCATION byte 보존의 추가 증거가 아닙니다.

일반 분석기는 r1/r2 모두 exit 2/inconclusive입니다. r1에는 drop/audit 실패가
있으며 r2에도 의도한 단절·reset·holdout abort와 LOCATION 부재가 남습니다.
별도 시나리오 성공으로 이 판정을 완화하지 않았습니다.

## 입력 고정과 남은 일

최종 build-r2와 먼저 VM에 넣은 build-r1은 테스트 대상 추가에 따른 Makefile
차이만 있고 다섯 산출물이 모두 byte-identical입니다. 최종 전체 검사는 r2의
source/toolchain 기록으로 수행했습니다.

| 항목 | SHA-256 |
| --- | --- |
| libmx5dr.so | `944304343c73b99dbd3c4b39f1eda3fd17be2fc662d93d3291a5bfe3b8dd00f3` |
| vimtap | `7cea2454bf95334fecf922027684bc6c2034e42ad1710a0676b6abcbb056db3b` |
| 최종 외부 caller | `3ac5b5dfd441e1a1b6c2da565d16cbd2eac627ceb42e4250a64f43647fa949c7` |
| 두 실행의 initrd | `35aedb67befcc6c2cc26699c575466447365621566bd0ef359f9e43fbb3208a0` |
| product-r1 실패 console | `6bc77cdbc198c2b9c0ed97d4a5a852746e5bea46089187c6a4ff3de040c74a26` |
| product-r2 성공 console | `0dc77eceeb797aae44b4fa7ebc6ab8387e01fff7a68c6815ad8fdc65972e17ef` |
| 독립 계산 검증기 | `7f7335bdb78fff7d10ba60061abad1ecfe83defd5ecbc74605fa66ef27837e12` |

runner의 제한 시간 종료 124/QEMU 종료 0은 통과 판정으로 쓰지 않습니다.
비공개 실행·실패·입력은 `evidence/request-contention-20260930/`에 남겼습니다.
Claude의 새 관련 커밋은 확인되지 않았고 마지막 독립 자료는 `e5d87c1`의
[LDS async 기록](LDS_ASYNC_2026-09-30.md)입니다. 이번 새 Claude 실행이나
실차 사용을 주장하지 않습니다.

다음 작업은 별도 journal 큐의 drop 원인 분리입니다. 같은 표 안의 경합,
물리 센서 단위·부호·생산 시각·품질, 정상 전체 차량 기동/복구, 유효 GPS와
실제 폰 수용도 미완료입니다. 이 원본 실행을 정상 차량 전체 검증으로
일반화하지 않습니다.
