# 세션 경쟁 수정과 기존 요청·MODEL 경계 통합 — 2026-09-30

NA 74.00.324A 전용입니다. `feat/session-observation`의 `91f2c93`에 외부 master
`20bf583`을 통합했습니다. 같은 작업 브랜치를 유지하며, 기존 검증 기록의 횟수를
바꾸지 않습니다. 외부 변경의 리뷰·실행 이력은
[원래 기록](SESSION_CONTEXT_REVIEW_2026-09-30.md)에 보존합니다. 아래는 통합본을
이번에 직접 실행한 결과이며 새로운 독립 에이전트 리뷰는 수행하지 않았습니다.

## 문제와 통합

기존 wrapper가 원본 create 반환 뒤 `*storage`를 읽었습니다. 원본의 handle
쓰기 잠금 밖이므로 동시에 실행되는 destroy의 쓰기와 경쟁할 수 있었습니다.
또한 destroy 안에서 다시 생성하거나 서로 다른 storage의 lifecycle 호출이
겹치면 반환 순서만으로 관측 문맥의 생존을 확정할 수 없었습니다.

- 원본 create의 반환 0만 관측하고 OEM handle storage를 다시 읽지 않습니다.
  `observed`는 현재 handle 유효성이나 요청의 소유권을 뜻하지 않습니다.
- create/destroy가 겹치면 해당 프로세스의 세션 관측을 fault로 유지합니다.
  원본 API·callback의 호출, 인자·전체 본문·반환·errno 전달은 계속합니다.
- lifecycle 중 정상 status callback이 들어오는 경우와 lifecycle 호출끼리
  겹치는 경우를 별도 counter로 구분합니다. callback도 기존 revision과 예측
  무효화 경계에 참여하지만 정상 초기 callback을 경쟁 fault로 바꾸지 않습니다.
- 기존 송신 후보 철회, MODEL/holdout reset, revision 소진 검사와 발행 당시
  요청 경로 복사를 보존했습니다. 외부 코드에 없던 이 기능들을 지우지 않았습니다.
- 같은 lifetime의 callback event 역행·같은 event의 상태 불일치·known→unknown은
  분석기 violation입니다. revision 변경 진단과 이력 모순 진단을 함께 낼 수
  있도록 하여 한 조건이 다른 조건을 숨기지 않게 했습니다.
- 실제 submit wrapper→지연 notify→worker→새 세션 send를 통과하는 작성 통합
  검사를 추가했습니다. 이전 lifetime/state뿐 아니라 revision 2와 route가
  끝까지 유지되고 실제 송신 대상은 lifetime 2/revision 6인 것을 검사합니다.

요청이 사용한 **bus 연결 수명**, 실제 provider identity, 위치 snapshot의
receiver 자격 및 요청/session 소유권의 qualified 연결은 **미구현**입니다.
이번 작업은 새 원격 경쟁 수정의 통합이며 이 항목들을 완료한 것으로 세지 않습니다.
live `provenance()`의 false와 `allow_assist=false`를 유지합니다.

## 작성 코드 검사

| 범위 | 직접 실행한 결과 |
| --- | --- |
| 수정 전 회귀 | 보관한 `91f2c93` 소스로 외부 회귀 6개를 빌드해 각각 assertion 실패를 확인했습니다. `closing_create`, `creating_during_destroy`, `null_success`, `output_race`, `late_destroy`, `distinct_storage`입니다. |
| ThreadSanitizer | 수정 전 `output_race`에서 create의 storage 읽기와 destroy 쓰기의 data race를 검출했습니다. 수정 후 같은 경로는 exit 0이며 sanitizer 경고가 없습니다. |
| `make test` | C/C++ 및 파이썬 297개 검사 통과, skip 0. 현재 ARM 산출물과 원본 BusyBox 설치 fixture를 명시했습니다. |
| 세션·요청 통합 | 호스트 세션 28개, 초기 constructor 경계와 submit→send 통합 검사 통과. 전체 callback 2400바이트 비교, 예외·취소 및 기존 예측 철회 8개를 포함합니다. |
| 고정 ARM | 다섯 제품 바이너리 빌드 및 최종 전체 검사 통과, skip 0. 제품 DSO의 position 8·request 14·session 28개와 MODEL 작업자 6개 시나리오 포함입니다. 첫 실행의 합성 송신 실패는 아래 별도로 남깁니다. |
| 원본 공유 runtime | 같은 제품 DSO의 세션 28개 검사 통과. 작성 API fixture와 원본 공유 runtime 조합이며 실제 LDS 실행은 아래 별도 범위입니다. |

ARM 전체 검사의 첫 실행은 `worker-session-test recreate`의
`sender.send_event(r)` assertion에서 exit 134로 중단됐습니다. 그때 호스트 검사와
이미지 생성도 실행 중이었지만 이것만으로 원인을 부하로 확정하지 않습니다.
실패 로그와 부분 journal을 보존했습니다. 다른 검사를 끝낸 뒤 QEMU syscall
기록을 켠 같은 바이너리의 단독 실행은 통과했고 `sendto` 오류가 0건이었습니다.
이 성공은 최초 실패의 원인을 해결했다는 증거가 아닙니다. assertion이나
생산 코드의 손실 판정을 완화하지 않았습니다. 다른 시험과 겹치지 않게 실행한
후속 ARM 전체 검사도 같은 제품으로 통과했습니다. 최초 실패 원인은 미분리입니다.

첫 baseline 컴파일은 병합 중 소스와 겹쳐 충돌 표식 때문에 실패했으며, 위의
수정 전 근거는 이후 보관한 불변 소스로 다시 빌드한 실행입니다. 최초 ARM
스크립트 직접 호출의 실행 권한 오류는 `sh` 호출로 정정했습니다. 생성 중인
initrd를 사용하려던 첫 실행은 입력 변경 검사에서 거부되어 OEM 시험으로
세지 않습니다. 이미지 생성 종료 후 새 결과 경로에서 아래 VM을 실행했습니다.

## 원본 커널·LDS·AA 실행

기존 비공개 진단 init/caller를 통합 제품으로 다시 빌드했습니다. NIC·호스트
공유 장치 없이 원본 커널에서 LDS와 AA를 실행했습니다. 전용 init을 사용하며
machine ID 인자의 진단 변경을 포함하므로 순정 전체 SM 기동 검사가 아닙니다.
물리 센서·차량·폰·동글은 사용하지 않았습니다.

| 제품 시나리오 | 세션 부재 | 세션 재생성 |
| --- | ---: | ---: |
| 실제 원본 LDS 요청 / 파괴를 넘긴 지연 요청 | 5 / 1 | 10 / 1 |
| 지연 요청의 SEND / LOCATION | 0 / 0 | 2 / 1 |
| LOCATION 반환 / 송신 대상 lifetime | 해당 없음 | 0 / 2 |
| 합성 raw / 지연 구간 raw 수집 | 546 / 195 | 483 / 132 |
| journal loss / request loss / session fault | 0 / 0 / 0 | 0 / 0 / 0 |

두 실행 모두 실제 LDS 응답의 mode·UTC·좌표는 0이었습니다. 지연 요청의
lifetime 1과 발행 당시 revision·route가 유지됐고, 연결된 POSITION/SEND의
요청 기록과 원본 송신 바이트가 일치했습니다. MODEL은 각각
`session_unavailable`, `session_changed_since_issue`로 늦은 위치를 제외했습니다.
직접 넣은 합성 GPS seed도 요청 관측이 없어 제외됐습니다. 모든 MODEL 출력은
invalid이며 일반 로그 분석기는 두 실행 모두 inconclusive(exit 2)입니다.

별도 원본 getter 진단의 요청 4건은 issue/notify 경로가 일치했으며 공개 serial
getter 값은 계속 0이었습니다. 원본 INVALID 상태 callback 두 주기는 generation
4→6→8, 10→12→14를 유지했고 세션 fault가 없었습니다. 시작 API의 반환 0이나
이 실패 상태 callback을 정상 폰 연결로 해석하지 않습니다.

완료 marker·capture 종료와 전용 route/callback 판정기를 확인했습니다.
VM helper는 175초 제한에서 exit 124, QEMU exit 0, `timed_out=true`였습니다.
그 종료 값 자체를 성공 근거로 사용하지 않습니다.

| 고정 산출물 | SHA-256 |
| --- | --- |
| 제품 `libmx5dr.so` | `bdc623138d14d17937a5d5a4077eed24c3def363e11a4ddc4461e78ac7ebd0d1` |
| 비공개 진단 initrd | `6a48d1439bc8bfaa583c07e97d39203d39ded55b07d89338b44c19cf6b6ee3e9` |
| 최종 console | `bc129f68a88b71d912faff8f36c6107b0ce5d0ee974e6ff7535044f430b8b3db` |

ARM 빌드의 53개 입력 파일 해시를 실제 작업 소스와 대조했습니다. 전체 console,
스크립트·산출물·실패 근거는 ignored `evidence/bus-lifetime-20260930-91f2c93/`에
보관합니다. 이 로컬 폴더 이름은 처음 예정했던 조사 대상이며 bus lifetime
구현 완료를 뜻하지 않습니다. 공개 릴리즈와 원격 master는 변경하지 않습니다.

## 도구 정리

추가 설치는 전용 `mazda-bus-lifetime` 컨테이너 안에서만 했습니다. QEMU와
빌드·DBus 개발 도구 등 추가 패키지 120개, 업데이트 6개와 고정 도구체인
manifest 2124개 항목을 기록했습니다. 검증 후 컨테이너와 그 안의 도구체인,
`/tmp/mazda-bus-lifetime.8so2jftz`를 모두 제거했습니다. 호스트 패키지 목록과
기존 Docker 이미지 ID 집합은 설치 전후 동일합니다. 추가 도구 실행 파일은
남기지 않았으며, 비공개 증거에는 작성한 검사 산출물과 실행 결과를 보관했습니다.
