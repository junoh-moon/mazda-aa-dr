# 위치 callback의 요청 문맥 전달 — 2026-10-02

기존 `position_enter`는 one-shot 요청 조회로 Trace를 확보한 다음 출처
검증 함수를 불렀지만, 검증 함수에는 위치 숫자만 전달했습니다. 그 함수가
같은 조회를 다시 시도하면 이미 소비한 요청을 사용할 수 없습니다. 늦게
도착한 LDS sideband도 과거 callback의 출처를 소급해 확립하지 못합니다.

새 `PositionContext`는 현재 callback에서 확보한 위치 사본, 요청 조회 결과,
Trace, call sequence, prediction generation을 출처 검증 함수에 전달합니다.
위치와 Trace는 callback 동안만 빌린 참조이며 별도 조회·대기·할당·전역
보관은 추가하지 않습니다. 기존 중첩 frame과 원본 송신·errno 계약을 유지합니다.

mode 변경에 따른 철회를 먼저 처리하고 generation을 캡처한 뒤 검증 함수를
호출합니다. 검증 중 generation이 바뀌어도 현재 callback의 generation을
새 값으로 바꾸지 않습니다. 실패한 요청 조회의 Trace는 기존처럼 비웁니다.
Trace 관측 성공 자체는 provider·receiver·센서의 자격이 아닙니다.

## 현재 검사

실제 요청 Ledger의 one-shot 소비와 adapter를 함께 실행하는 새 회귀를
추가했습니다. 같은 위치 숫자를 가진 서로 다른 요청을 사용하여 값 비교로
식별자를 추정하지 않습니다.

- 정상 요청과 원래 POSITION/SEND에 동일한 요청·worker token을 전달합니다.
- 중첩 callback의 별도 frame이 바깥 요청의 참조를 덮지 않습니다.
- 실패하거나 없는 요청 조회는 빈 Trace와 원래 실패 결과를 전달합니다.
- 검증 중 generation 철회 뒤에는 새 후보를 발행해도 옛 callback에서
  DR 송신을 선택하지 않습니다.
- 작성한 출처 검증이 false이면 정상 Trace가 있어도 원본을 전달합니다.
- 해독할 수 없는 위치는 검증 함수를 부르지 않고 원본 송신을 유지합니다.

수정 전 제품 소스로 여섯 사례가 문맥 누락 assertion에서 실패했고 잘못된
위치 대조 한 사례는 통과했습니다. 수정 후 같은 핵심 회귀 일곱 사례가
host와 고정 ARM에서 통과했습니다. host의 기존 adapter 및 ASSIST 파이프라인
집중 검사도 종료 0입니다. 실제 제품 DSO 및 새 고정 커밋의 전체 검사는
아직 이 기록에서 완료로 세지 않습니다.

초기 fixture에서 잘못된 위치의 예상 reason과 작성한 bus 상태가 빠진
오류도 발견해 고쳤습니다. 해당 실행을 지우지 않았으며, 최종 회귀를 옛
소스와 새 소스 양쪽에서 대조했습니다. 비공개
`evidence/lds-assist-source-20261002/inline-context/`에 그 실행과 원본 로그를
보존합니다. DSO용 fixture는 작성한 요청 Ledger만 링크하고, adapter는
검사할 제품 파일의 함수를 직접 호출하도록 연결했습니다.

## 남은 구현

이 변경은 출처 검증 함수가 실제 현재 요청을 볼 수 있게 한 연결입니다.
runtime의 기본 출처 검증은 여전히 false이고 `allow_assist=false`입니다.
물리 자격을 갖춘 입력 공급부, 송신 전에 확립한 provider·receiver 근거,
실제 DR 위치 대체와 GPS 복귀 및 폰/지도 반영까지의 완료를 뜻하지 않습니다.
기존 공개 설치 ZIP은 변경하지 않았습니다.
