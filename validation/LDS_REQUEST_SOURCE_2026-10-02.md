# LDS 요청 연결 공급부 — 2026-10-02

v1.0의 목표는 관성항법 위치를 실제 AA 송신에 적용하는 것입니다. 이번 변경은
원래 별도 로그로만 남던 AA POSITION과 LDS 응답을 기존 worker의 메모리에서
연결합니다. 실제 `AssistSource` callback이 이 소유된 연결을 조회할 수 있도록
`run_worker_inputs()`에 공급부를 연결하며 기존 worker 진입점은 유지합니다.

**물리 자격 공급부와 live ASSIST는 미완료입니다.** 연결 결과 자체는
`AssistInput`이 아니며 센서 단위·측정 시각·품질, receiver 자격, 생산자
watermark를 제공하지 않습니다. 이미 실행한 callback의 false provenance를
사후 true로 바꾸지 않으며 원본 AA 송신을 기다리게 하거나 재실행하지 않습니다.
`allow_assist=false`와 기존 live provenance 실패를 유지합니다.

## 입력과 수명

서버 GUID·client/server 고유 이름과 요청/응답/reply serial 여섯 요소,
원본 아홉 위치 필드 및 실제 request/worker token을 보존합니다. 어느 쪽이
먼저 도착해도 같은 요청을 연결하며 부분 캐시 할당 출처는 그대로 복사합니다.
필드 출처 번호가 뒤로 가거나 정상 응답 순서가 바뀌었다는 이유로 막지 않습니다.
초기 session 관측이 unknown이어도 정확한 wire 연결은 시도합니다.

보관 공간은 64개 항목이며 첫 항목 수신부터 5초 뒤 보관창을 폐기합니다.
중복·조회로 이 시간을 연장하지 않습니다. 용량 소진·reset도 보관창을 끝내고
worker의 실제 관측 시각을 경계로 과거 입력 재생을 거부합니다. 이후 새 요청은
다시 처리합니다. 이 시간은 메모리 보관 정책이며 센서 freshness나 DR 허용
시간이 아닙니다. 폐기되거나 연결되지 않은 원시 입력도 기존 journal에 남습니다.

조회 결과는 값 복사본과 revision입니다. 늦은 충돌, 보관창 폐기와 종료는
이전 revision을 무효화합니다. `current()`는 같은 소유 객체의 보관 수명을
검사하며 호출자가 수정한 복사본을 인증하거나 현재 차량·폰 세션을 원자적으로
보증하지 않습니다. 다른 wire key로 바뀐 충돌 식별자도 보존해 잘못된 회복을
막습니다. LDS Path 관측이 원래 요청 발행보다 앞서는 연결도 거부합니다.

소켓 수신 뒤와 POSITION을 가져온 뒤 시계를 읽습니다. 별도 잘못된 datagram은
거부하되 정상 요청을 전역 차단하지 않습니다. worker 중단·audit 실패는 보관
결과를 폐기하며, 기존 용량 제한 health에 누적 연결·충돌·폐기 수를 남깁니다.
`matches_total`은 보관 중 성립했던 연결의 누적 수이며 나중 충돌을 지우거나
ASSIST 자격을 증명하는 수가 아닙니다. 새로운 영구 기록 파일은 만들지 않습니다.

## 수정 전 실패

실제 worker의 수신·원시 journal·source readiness 호출이 실행되었지만,
공급부 연결이 없을 때 `wait_count(consumer.matched)` assertion이 실패했습니다.
core도 명시적 TODO stub에서 MATCHED assertion이 실패했습니다.
추가 검토에서는 충돌 식별자의 다른 key 재사용과 요청 발행 이전의 LDS 관측을
재현한 두 실패를 확인한 뒤 수정했습니다. 컴파일 경고 및 잘못된 fixture의
조회 기대값 정정은 이 행동 실패와 구분해 비공개 기록에 보존합니다.

## 검증 범위

구현과 함께 추가한 core 회귀는 양쪽 도착 순서, 아홉 필드 차이, 식별자 충돌,
부분·미확인 입력, 누적 drop, 용량·보관 시간·clock 경계와 재생을 검사합니다.
worker 회귀는 실제 큐·credentialed socket과 `AssistSource.readiness()` 조회를
사용합니다. 입력은 작성한 POSITION이며 OEM callback 전체를 실행한 시험이
아닙니다. callback은 물리 자격을 false로 보고하고 발행·입력 pop은 0입니다.

고정 전 집중 검사에서 host `make test-lds`와 core의 고정 GCC 4.9.1/QEMU
검사가 통과했습니다. core는 각각 12개 사례·344개 assertion입니다. 실제
worker는 POSITION 먼저, sideband 먼저, payload 불일치, 늦은 충돌, 미리
요청된 중단, 잘못된 datagram 뒤 회복의 여섯 사례가 host에서 통과했습니다.
잘못된 datagram에 전역 reset을 넣은 별도 음성 대조는 정상 연결 회복 assertion에서
실패했습니다. 기존 journal의 보존·회전·종료 집중 검사도 통과했습니다.

전체 host·고정 ARM 및 원본 LDS/AA 제품 실행은 아직 이 기록에 완료로
계상하지 않습니다. 고정 구현 commit의 별도 실행 뒤 결과와 도구 정리를
추가합니다. 현재 공개 v0.3.10-shadow.1 ZIP은 변경하지 않습니다.

다음 단계는 연결 결과를 검증된 기준점·제어 입력으로 바꾸는 실제 공급부와,
AA 송신 전에 확립하는 요청별 출처입니다. 물리 측정 시각·보정 근거와 실제
폰/지도 반영을 확보하지 않은 상태를 v1.0 완료로 취급하지 않습니다.
