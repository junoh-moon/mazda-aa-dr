# 새 제품의 LDS→현재 AA callback 연결 — 2026-10-02

고정 소스 `3625449ba1cd713c4b7ce13f778847acd86d8819`에서 새 제품 여섯 개를
빌드했습니다. 원본 LDS reader·응답에서 현재 AA POSITION/SEND까지 출처를
연결하는 실제 제품 경로를 실행했습니다. 이 연결은 관측 근거이며 물리 센서
자격 공급부와 live ASSIST는 여전히 미구현·비활성입니다. 새 설치 ZIP은
발행하지 않았으며 공개 `.1`·`.2`의 사용 중단을 유지합니다.

| 새 제품 | SHA-256 | 실제 PT_TLS 크기 |
| --- | --- | ---: |
| AA `libmx5dr.so` | `9e248620c8f046586480f9e55b6f41819f75e12ca386cd99498d4a4dddcbce10` | 196바이트 |
| LDS `libmx5dr-ldstap.so` | `2da6112779140af2760520e2143e56b2f4bcbdc18aa75a10f1abe997b6a4cff0` | 52바이트 |

두 TLS의 정렬은 4바이트입니다. 이전 외부 LDS 제품의 48바이트와 이번
52바이트를 혼동하지 않습니다. 두 clean checkout과 Git archive를 대조했고
빌드 입력 79개·실제 제품·묶음 식별자를 비공개 기록에 고정했습니다.

## 실제 제품 집중 검사와 전체 검사 실패

[AA 풀 후속 검사](AA_POOL_ASSOCIATION_2026-10-02.md)의 실제 새 DSO에서
문맥 풀·fork 12개와 작은 스택 2개, 합계 **14사례가 생략 없이 통과**했습니다.
caller는 제품 구현을 소스 링크하지 않고 실제 ELF의 함수를 호출합니다.
16KiB·24KiB 스레드에서 1KiB 작성 로컬 배열을 유지하며 원본 호출 한 번,
포인터·48바이트·반환값·errno와 POSITION/SEND의 소유된 사본을 대조했습니다.
표본 frame 주소는 최심 스택이나 원본 OEM 스택 여유의 측정값이 아닙니다.
416개 tracked 파일·79개 빌드 입력·여섯 제품과 14개 실제 case log의 해시를
독립적으로 다시 읽어 대조했습니다.

같은 pin의 전체 `make test`는 직접 종료 0이며 Python **592개**와 C/C++
검사를 생략 없이 통과했습니다. 반면 첫 전체 ARM 실행은
`test_worker_session`의 `bus_late_same`에서 **종료 134로 실패**했습니다.
작성 motion seq411의 실제 검사 나이는 251,299,274ns였고 제품의 250ms
제한이 `stale`로 거부했습니다. 다음 seq412는 순번 불연속으로 거부됐습니다.
시험의 “거부 기록 없음” assertion이 실패한 것이며, 제품의 시간 제한을
늘리거나 원시 입력 시각을 갱신하지 않았습니다.

같은 ARM binary·argv의 단독 대조 한 번은 종료 0으로 위치 9건·원시 입력
528건, 늦은 요청 거부 3건과 기존 모든 assertion을 통과했습니다. 처음의
전송 지연이 발생한 원인은 아직 확정하지 않았습니다. 별도 원본 실행의
wrapper 구간이 첫 실패 시험과 겹쳤지만 인과관계의 증거로 세지 않습니다.
최초 실패의 journal·core·명령·입력은 보존했습니다. 전체 ARM은 **실패·미완료**
상태이며 단독 성공이나 뒤의 제품 실행을 합산해 전체 통과로 바꾸지 않습니다.

## 원본 reader·LDS·AA의 실제 연결

원본과 제품을 별도 IPC namespace에서 실행했습니다. 작성한 부분 기동,
PTY 문장과 시작 순서를 사용하며 실제 차량·정상 전체 SM 기동은 아닙니다.

- LDS loader가 실제 원본 7개 모듈과 27개 슬롯을 확인·설치했습니다.
  fixture는 슬롯을 직접 수정하거나 map/FD·Registry adoption을 주입하지
  않았습니다. 원본 reader·parser·callback·cache·DBus 응답을 실행했습니다.
- 원본 BLM의 주기적 위치 요청과 원본 AA LOCATION 송신 **10쌍**을 읽었습니다.
  raw DBus의 정확한 요청·응답 키, POSITION의 숫자 **90개**, 송신의 원본/
  출력 **48바이트 10쌍**을 대조했습니다. 임의 위치 callback 호출은 없습니다.
- 첫 callback 뒤에 실제 AA worker를 시작하는 조건을 작성했습니다.
  처음 **3건은 unavailable**, 이후 **7건은 matched_locked_for_send**였습니다.
  같은 callback의 요청·worker 토큰, 필드 할당 출처와 소유된 연결 값이
  POSITION/SEND에서 일치했습니다. 이는 send 성공이나 물리 신선도 자격이
  아닌, 메시지 잠금 단계에서의 관측 사본입니다.
- 첫 worker 이전 sideband 한 건의 누락을 의도한 조건으로 구분했습니다.
  이후 sideband 9건과 정확한 요청을 대조했고 추가 drop은 없었습니다.
  실제 LDS의 쓰기 공유 mapping과 AA의 읽기 전용 mapping이 같은 삭제된
  backing inode를 사용했습니다.
- client/server는 각각 종료 0이고 worker·원본 큐의 정상 종료를 확인했습니다.
  관측 종료 후 resolver 보관 항목은 0이며, 별도 daemon·monitor와 소유한
  IPC·socket·alias는 정리했습니다. errno는 passive sink 진입 시각의 보존
  검사이며 원본 caller 복귀 직후의 독립 계측이 아닙니다.

최초 실행기의 **후처리 checker가 8초 제한을 넘었으므로 전체 orchestration은
실패로 보존**했습니다. 원본을 다시 실행하지 않고, 저장한 동일 checker를
동일 캡처에 실행한 대조는 0.064초·종료 0이었습니다. 별도 독립 읽기도 위
10쌍·90필드·7개 inline 연결과 mapping을 확인했습니다. 후처리 시간 초과의
원인은 미확정이며 성공한 원본 구간과 실패한 실행기를 구분합니다.

실행 전 fixture 출력의 닫는 따옴표 누락도 별도로 발견했습니다. 실제 C++
출력에서 POSITION/SEND 두 행의 JSON 파싱 실패를 재현하고 수정 후 두
검사를 통과했습니다. 최초 compile-only 성공은 출력 형식 검증을 대신하지
못했습니다. 이 fixture 수정은 제품 코드 변경이 아닙니다.

## 남는 범위

별도 [새 제품 ASSIST 적용·복구](AA_POOL_APPLICATION_2026-10-02.md)는 작성한
자격 입력으로 18쌍·DR 대체 6건을 다시 확인했습니다. 이 fixture의
association_reader는 null이며 위 실제 관측 연결과 합쳐 물리 공급부 완료로
세지 않습니다. 부분 SM 기동은 후속 작업입니다. 물리 생산 시각·센서 품질·단위·보정·receiver/provider 자격,
실제 CMU 기동·복구와 S25/동글/네이버 수용은 미검증입니다. 관성항법 위치를
실제로 적용하는 [v1.0 목표](../docs/V1_READINESS_KO.md)는 유지합니다.

원본 바이너리·maps·원시 로그는 공개하지 않습니다. 실행·실패·독립 읽기는
비공개 `full-3625449`, `product-pool-focused-3625449`, `product-association`
기록에 각각 보존했습니다.

## 후속 검사 보강: 거짓 통과 대조

실제 Claude CLI의 독립 리뷰에서 FD 거부·fork·retire 검사의 일부 조건이
다른 거부 조건에 가려질 수 있음을 지적받았습니다. 제품 코드는 그대로 두고
검사를 보강했습니다. descriptor 수·읽기 전용·nlink·크기·mode 검사 제거와
Registry child-disable·retire 무효화, 총 **7개 사적 오류 주입판**에서 이전
해당 검사가 모두 통과했습니다. 보강 후 동일 7개는 모두 기대 assertion으로
실패했습니다. 리뷰 문장 자체를 실행 결과로 세지 않았습니다.

FD 음성 대조에는 새롭고 수락 가능한 publisher identity와 유효한 map을
사용하며, 같은 identity의 정상 FD가 바로 채택되는 양성 대조를 둡니다.
fork 검사는 읽을 수 있는 even sequence에서 child disable 유무를 비교하고,
retire는 capacity 교체 이전의 실제 MATCH를 즉시 철회하는지 확인합니다.
기존 17개 case를 유지한 새 집중 검사는 host·고정 ARM 각각 **17사례·1,162
assertion, 생략 0개**를 통과했습니다. 이전 499개 결과는 소급 변경하지
않습니다. fstat의 다른 소유 UID를 직접 주입하지는 않았고 기존 peer UID
거부는 유지합니다. 이 대조는 하드웨어 ARM 메모리 순서의 실행 증명이 아닙니다.
