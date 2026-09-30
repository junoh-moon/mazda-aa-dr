# 실제 버스 연결 수명 관측 — 2026-09-30

NA 74.00.324A 전용입니다. `11c115e`의 요청·세션 관측에 실제 JCIDBUS
연결 경계를 추가했습니다. 같은 `feat/session-observation` 브랜치에서 작업하며,
차량·폰·동글은 사용하지 않았습니다. 공개 설치 ZIP은 변경하지 않았습니다.

## 문제와 변경

기존 요청 기록은 버스 연결 수명을 알 수 없었습니다. 객체 주소가 재사용될
수 있으므로 포인터 값만으로 이전 연결과 새 연결을 같다고 판정할 수 없습니다.
실제 종료 알림도 API 호출뿐 아니라 원본 signal 경로를 관측해야 했습니다.

- 정확한 원본 create/connect/disconnect/free, close callback과 signal handler를
  관측합니다. connect는 0이 아닌 값이 성공이며 disconnect/free는 void입니다.
- 객체와 connect 경계에 프로세스 내 식별자를 부여합니다. 생성부터 관측하지
  못한 객체는 미관측이며 실패한 reconnect는 연결됨으로 표시하지 않습니다.
  식별자는 daemon GUID, provider identity 또는 receiver/session 자격이 아닙니다.
- callback 문맥 64개는 재사용하지 않습니다. free 뒤 이전 callback이 와도
  같은 주소의 새 객체를 종료 처리하지 않습니다. 원본 free 안의 disconnect
  중첩은 정상 경계로 인정합니다. 겹친 lifecycle·소진·예외·취소·관측 충돌은
  명시적인 fault이며 원본 인자·호출·callback·반환·errno 전달은 유지합니다.
- 발행 당시와 실제 응답의 연결을 각각 복사해 worker→POSITION→SEND까지
  보존합니다. 분석기는 변경·관측 불가를 inconclusive로, 모순된 필드를
  malformed로 처리합니다. 이전 로그 형식도 계속 읽습니다.
- 고정 파일 해시·함수 바이트·GOT 원본·메모리 권한 검사를 유지합니다.
  한 cold transaction이 BLM 함수 4곳과 GOT 16곳을 설치합니다. 이미
  실행될 수 있는 JCIDBUS의 코드 페이지는 수정하지 않습니다. signal 확인에
  쓰는 원본 libdbus도 파일 해시·매핑·함수 바이트를 추가 검증합니다.
- 각 연결 경계의 진입·복귀에서 adapter 후보를 철회합니다. MODEL/holdout의
  버스 경계 reset, daemon/provider identity, 요청/session 소유권과 receiver
  qualification은 아직 TODO입니다. live ASSIST는 계속 비활성입니다.

## 원본 실행에서 찾은 단절 관측 누락

첫 제품은 생성 시 등록한 close callback만 감쌌습니다. 첫 VM은 최초 dispatch
전에 전용 daemon을 종료했지만 callback이 오지 않아 시험이 실패했습니다.
원본은 최초 수동 dispatch에서 filter/watch를 등록하므로, 두 번째 VM은 정상
10회 dispatch 뒤 종료했습니다. 이 조건도 실패해 등록 순서 가설을 해결책으로
삼지 않았습니다. 두 실행의 실패와 이전 바이너리·이미지를 보존합니다.

정적 분석에서 general signal filter가 모든 신호에 처리 완료를 반환하여 뒤의
core filter가 `org.freedesktop.DBus.Local.Disconnected`를 받지 못하는 경로를
확인했습니다. 최종 제품은 실제 `JCIDBUS_signal_handler` 진입에서 원본 signal
predicate로 해당 신호를 관측한 뒤 원본 handler를 그대로 호출합니다. 없는
callback을 인위적으로 호출하거나 필터 반환값을 바꾸지 않습니다. 작성 회귀도
수정 전에는 연결이 계속 connected로 남는 assertion 실패를 확인했습니다.

최종 제품의 세 번째 VM은 실제 daemon 종료 뒤 관측을 disconnected로 바꾸고
adapter generation을 30→32로 철회했습니다. 원본 close callback은 그대로
0회이며 관측 fault 0, caller exit 0입니다. 명시적 free 뒤에는 unobserved로
전환됐습니다. 이 결과는 해당 API/signal 관측의 근거이며 daemon의 신원을
인증하거나 물리 버스의 모든 장애 경로를 검증한 결과가 아닙니다.

같은 객체·이름의 reconnect는 원본 반환 0으로 실패했습니다. 원본 로그의
이름 소유자 존재 보고를 보존하지만 전체 원인을 해결한 것으로 세지 않습니다.
첫 두 VM의 새 객체는 같은 주소를 받았고 최종 VM은 다른 주소를 받았습니다.
최종 제품의 주소 재사용/늦은 callback 회귀는 작성 fixture로 별도 검사했습니다.
원본 free의 명시적 disconnect 미호출 경고와 기존 manager의 스레드 관련
경고도 전체 console에 남겼으며 오류 없는 OEM 전체 정리를 주장하지 않습니다.

## 최종 제품의 원본 LDS·AA 실행

NIC·호스트 공유 장치가 없는 원본 커널 VM에서 전용 진단 init/caller를
실행했습니다. 진입 machine ID 인자의 진단 조정을 포함하며 원본 커널/OEM
파일 바이트는 바꾸지 않았습니다. 순정 전체 SM 기동이나 물리 입력 시험이 아닙니다.

| 제품 시나리오 | 세션 부재 | 세션 재생성 |
| --- | ---: | ---: |
| 실제 원본 LDS 요청 / 세션 경계를 넘긴 지연 요청 | 5 / 1 | 12 / 1 |
| 지연 요청의 SEND / LOCATION | 0 / 0 | 2 / 1 |
| LOCATION 반환 / 실제 send session lifetime | 해당 없음 | 0 / 2 |
| 합성 raw / 지연 구간 raw | 543 / 195 | 489 / 135 |
| journal loss / request loss / session fault / bus fault | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |

원본 BLM의 service 연결은 object 2/lifetime 2, HMI 연결은 object 1/lifetime
1이었습니다. 실제 LDS 발행/응답의 연결은 동일한 service 수명으로 유지됐고
POSITION/SEND의 요청 metadata와 원본 송신 바이트도 일치했습니다. 원본
Dbus 정리 뒤 두 연결의 관측은 모두 unobserved였습니다.

모든 실제 LDS 응답의 mode·UTC·좌표는 0이며 MODEL 출력은 모두 invalid입니다.
늦은 요청과 미관측 합성 GPS seed의 기존 MODEL 제외를 유지했습니다. 일반
분석기는 두 로그 모두 inconclusive(exit 2), violation 0입니다. 별도 원본
INVALID 상태 callback 두 주기도 기존 generation 4→6→8, 10→12→14와
fault 0을 유지했습니다. API 반환 0은 정상 폰 연결이나 위치 수용이 아닙니다.

최종 전용 LDS/세션·버스·callback 판정기를 통과했습니다. 버스 판정기의
완료 누락, 단절 후 connected 유지, 가짜 callback, 후보 철회 누락 네 변형은
모두 실패로 검출했습니다. helper 종료 전의 예비 console hash는 이후 종료
기록으로 바뀌어, helper 종료 뒤 전체 판정기와 해시를 다시 확인했습니다.
최종 VM helper는 175초 제한에서 exit 124, QEMU exit 0·timed_out=true이며
이 종료 값 자체는 성공 근거가 아닙니다.

## 작성 코드 검사와 산출물

최종 제품으로 `make test`와 고정 도구체인의 ARM 전체 검사를 모두 통과했습니다.
호스트 Python 검사는 297개이며 두 전체 검사에서 건너뛴 검사는 없습니다.
제품 DSO의 위치 8개·요청 14개·세션 28개·버스 16개, cold transaction의
메모리 권한 실패 25개 경계, 초기 생성자 관측, 요청 연결 유지/재생성 2개와
worker 세션 6개 시나리오를 포함합니다. 원본 공유 런타임을 사용한 버스
DSO 16개도 별도 통과했습니다. DSO의 원본 API 대역 검사는 위 원본 JCIDBUS
실행과 구별합니다. 제품 입력 56개의 해시와 5개 ARM 산출물이 현재 소스 및
빌드 manifest와 일치함을 확인했습니다.

최초 ARM 전체 실행은 새 bus early-init 시험의 링크 목록 누락으로 중단됐습니다.
필요한 veneer/설치 의존성을 시험 빌드에 추가했으며 이 실패를 생산 바이너리의
실행 오류로 세지 않습니다. 이전 host journal 실행 파일로 처음 실행한 검사는
무관한 필드 누락도 보여, 새 fixture를 빌드한 뒤 필요한 두 실패만 다시 확인했습니다.
초기 미구현 관측기·요청 연결의 실패, journal/parser 실패와 15개 슬롯이 기존
7개 제한에서 거부되는 실패도 보존합니다. 이후 16개 슬롯의 복구 경계까지
검사했습니다. 새 독립 에이전트 리뷰는 수행하지 않았습니다.

| 고정 산출물 | SHA-256 |
| --- | --- |
| 최종 제품 libmx5dr.so | `4b3e5d11d355e97a6df72738b596d692793d5df3a8de0d61c35a88fd64841f56` |
| 최종 비공개 initrd | `301d1dd360c22797522aebde842dced7813f907d09845927523106e289867639` |
| 최종 console | `dcca7e6ec199db730583f0c64ba49c8a12b2897b02aedf9e5b7510832ca6bd7a` |

원본 바이너리·전체 console·주소 자료는 게시하지 않습니다. 실패와 최종
실행 근거는 ignored `evidence/bus-connection-20260930-11c115e/`에 보관합니다.

## 원격 변경과 도구

중간 fetch/pull에서 외부 master `e79ca52`의 MODEL 시각·검사 보강을 확인했습니다.
이 버스 구현 단위 이후 같은 브랜치에서 검토·통합할 대상으로 기록합니다.
외부 검증 수치를 이번 실행 결과로 옮기지 않았습니다.

도구는 전용 `mazda-bus-connection` 컨테이너에만 설치했습니다. QEMU·빌드·
DBus 개발 도구 등 추가 패키지 120개, 업데이트 6개와 고정 도구체인
manifest 2124개 항목을 기록했습니다. 검사 후 전용 컨테이너·도구체인·임시
폴더를 제거했고, 호스트 패키지 목록과 기존 Docker 이미지·컨테이너 목록이
작업 전과 동일함을 확인했습니다. 재현 소스·제품·검사 기록·비공개 VM 이미지만
보존하며 추가 설치한 도구 실행 파일은 보존하지 않습니다.
