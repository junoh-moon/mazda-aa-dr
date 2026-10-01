# ASSIST 기준점과 GPS 콜백 결합 — 2026-10-01

대상은 Mazda Connect NA 74.00.324A용 qualified ASSIST 계산 경로입니다.
구현 커밋은 `6b7b6bf`이며 최종 통합 소스는 `bdda755`입니다. 공개
`v0.3.5-shadow.1` ZIP에는 이 수정이 없습니다. 그 버전의 결함은 릴리스 노트에도
표시했습니다. 일반 설치의 live ASSIST는 계속 비활성입니다.

## 재현한 결함

기존 계산기는 검증된 기준점 A1 뒤에 새로운 GPS 위치 P2를 받더라도, P2에
대응하는 기준점이 없으면 A1의 READY seed를 유지했습니다. 같은 GPS 모드의
연속 콜백과 GPS 품질 1↔2 전환에서 각각 재현했습니다. 뒤따른 GAP은 이전
seed로 코어를 ACTIVE로 만들고 adapter의 새 generation 아래 DR 후보를
송신할 수 있었습니다. 이는 실제 물리 입력 공급부가 아직 없는 비활성 경로의
결함이지만, 향후 ASSIST로 승격하면 잘못된 위치를 낼 수 있으므로 수정했습니다.

또 다른 경우에는 동일 수신 시각의 새 미결합 POSITION을 먼저 처리한 뒤
늦게 도착한 이전 콜백의 ANCHOR가 오래된 seed를 되살릴 수 있었습니다.
단순 시각 비교만으로는 두 콜백을 식별할 수 없으므로 callback 순번을
명시적으로 결합했습니다.

## 변경한 계약

- `AssistInput.position_call_sequence`가 기준점의 근거가 된 adapter POSITION
  콜백의 순번을 명시합니다. qualified 경로에서 순번 없는 ANCHOR는 거부합니다.
- 계산기는 ANCHOR를 seed한 뒤 **바로 그 순번의 GPS POSITION**을 처리해야만
  다음 GAP에 사용할 수 있는 후보로 인정합니다. 기준점 없는 GPS 위치와
  기준점만 있는 상태의 GAP은 이전 후보를 철회하고 `NO_ANCHOR`로 남깁니다.
- 처리한 POSITION의 최대 callback 순번은 같은 source/session epoch 안에서
  후보 철회·재설정 뒤에도 유지합니다. 따라서 늦은 이전 ANCHOR 또는 순서가
  뒤집힌 콜백으로 seed를 복구하지 않습니다. 새 epoch에서는 초기화합니다.
- `unpaired_positions`를 worker 상태와 journal에 기록해 다음 정상 tick이
  거부 이유를 덮더라도 주차 중 상태 확인에서 원인을 찾을 수 있게 했습니다.

새로운 ANCHOR가 수신 시각보다 앞서 측정된 정상 사례는 허용하되, 수신 시각을
producer 시각이나 자격 증거로 사용하지 않습니다. 정렬된 큐에서 먼저 처리된
미결합 GPS 때문에 이미 포착된 이후 기준점까지 버려지는 경우가 있습니다.
옛 generation의 후보에 새 generation을 임의로 붙일 수 없으므로 의도한
fail-closed 가용성 제한입니다.

Claude의 별도 설계 비평은 코드를 읽지 않은 조건부 검토였습니다. 콜백을 먼저
drain하고 기준점을 나중에 받는 경우의 영구 거부를 지적했으며, 이는 위
가용성 경계와 일치합니다. 순번 재사용의 조건부 안전 반례도 제시했지만,
현재 adapter 순번은 프로세스 전역 단조 `uint64_t`이고 worker는 기준점의
source/session/generation 전체가 입력 context와 같은지 검사합니다. 이
비평을 제품 실행 검증이나 실차 증거로 세지 않았습니다.

## 검증과 경계

수정 전 작성한 제품 경로 회귀에서 미결합 GPS 뒤 GAP의 DR replacement를
재현했고, 수정 후 동일 조건은 순정 전달과 `NOT_READY`로 끝났습니다.
같은 시각의 오래된 기준점, 잘못된 순번, ANCHOR만 받은 GAP, 정상 재획득과
측정·수신 시각 차이도 제품 DSO·worker·navigation 시험에 추가했습니다.
최종 `bdda755`의 고정 ARM 전체 검사에는 제품 DSO ASSIST 28개와
runtime-assist 9개 사례가 포함되며 종료 0, 생략 0입니다. 전체 host 및
최종 ZIP 검사는 [v0.3.6 발행 기록](RELEASE_V036_2026-10-01.md)에 남깁니다.

실차 센서의 측정 시각·단위·품질, LDS producer→snapshot→응답 자격,
물리 복구와 폰/지도 앱 수용은 여전히 미구현 또는 미검증입니다. 이 결과로
live ASSIST를 켜거나 v1.0 완료를 주장하지 않습니다. 선택 후 실제 OEM
send 직전의 세대 변경 경합도 별도 검증 범위로 남습니다.
