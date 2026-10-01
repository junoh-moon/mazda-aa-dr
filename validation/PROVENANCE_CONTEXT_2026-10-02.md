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
이 집중 검사에 포함하지 않았으며, 후속 실행을 아래에 구분합니다.

초기 fixture에서 잘못된 위치의 예상 reason과 작성한 bus 상태가 빠진
오류도 발견해 고쳤습니다. 해당 실행을 지우지 않았으며, 최종 회귀를 옛
소스와 새 소스 양쪽에서 대조했습니다. 비공개
`evidence/lds-assist-source-20261002/inline-context/`에 그 실행과 원본 로그를
보존합니다. DSO용 fixture는 작성한 요청 Ledger만 링크하고, adapter는
검사할 제품 파일의 함수를 직접 호출하도록 연결했습니다.

## 고정 제품의 후속 전체 검사

`8d5669c3cd06180dfcca52758c976c02d7b51b23`의 별도 clean checkout에서
고정 GCC 4.9.1로 여섯 ARM 제품을 새로 만들었습니다. 전체 host `make test`는
Python 577개와 C/C++를 포함해 종료 0, 고정 ARM 전체는 Python 105개와
C/C++를 포함해 종료 0입니다. 두 실행 모두 생략 0이며 순서대로 실행했습니다.

실제 AA 제품 DSO 검사는 기존 여덟 suite와 새 `provenance-context` suite,
총 아홉 suite·152개 사례가 모두 통과했습니다. 새 일곱 사례는 작성한 요청
Ledger와 검사 호출기만 링크하고 adapter 함수는 선택한 제품 ELF에서 직접
호출합니다. 원본 LDS 라이브러리를 사용하는 설치기 71개도 별도로 통과했습니다.
이 설치기 검사는 설치기 소스를 별도 호출기에 링크한 실행이며 실제 제품의
자동 기동·전체 서비스 실행과 구분합니다.
시작·종료의 제품, 빌드 입력, 도구체인과 원본 fixture 해시가 일치했습니다.

| 새 실행 제품 | SHA-256 |
| --- | --- |
| AA | `c1c0bf9175a57ebf0bf677aaa24d2d03ca958f5510337c6b5b9efe86f4ec7180` |
| LDS | `e6f560bc2185c68af5e9901f3ba05a2cecafbde30b0bfb1aaa5b2055e4b9c342` |

LDS 제품에도 공통 `adapter.cpp`가 링크되므로 해당 파일의 실제 코드가
변했습니다. 앞선 `0f9ffdd`의 원본 reader·제품 실행을 새 제품의 재실행으로
세지 않습니다. 이번 pin에서 정상 전체 OEM 기동이나 순정 BusyBox 최종 ZIP
검사는 재실행하지 않았으며 새 설치 ZIP도 발행하지 않았습니다.

이후 `8f952984e83f53baf4d2afda416e9f577c9ca4ce`에 외부 master의
필드 출처 PC 분석기를 통합했습니다. 제품 빌드 입력 75개가 위 pin과 모두
같음을 확인하고, 변경된 분석기를 포함한 journal Python 163개를 새로
실행하여 종료 0·생략 0을 확인했습니다. 문서 충돌에서는 완료된 관측 연결과
남은 qualified 공급부를 구분했습니다. 외부 작성자의 전체 검사 횟수와 이
집중 검사의 횟수는 합산하지 않습니다.

비공개 원문은 `evidence/lds-assist-source-20261002/full-8d5669c/`와
`merge-f6a64c8/`에 보존합니다. 호스트 패키지를 추가하지 않았고 검사용
컨테이너의 도구 목록도 전후 같습니다. 기존 임시 도구체인·QEMU·주 컨테이너는
후속 실제 ASSIST·원본 BLM 송신 시험 준비에 필요하여 유지 중이며 전체 정리
완료로 표시하지 않습니다.

## 남은 구현

이 변경은 출처 검증 함수가 실제 현재 요청을 볼 수 있게 한 연결입니다.
runtime의 기본 출처 검증은 여전히 false이고 `allow_assist=false`입니다.
물리 자격을 갖춘 입력 공급부, 송신 전에 확립한 provider·receiver 근거,
실제 DR 위치 대체와 GPS 복귀 및 폰/지도 반영까지의 완료를 뜻하지 않습니다.
기존 공개 설치 ZIP은 변경하지 않았습니다.
