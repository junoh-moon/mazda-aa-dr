# HEADING 할당에 속한 RMC 상태 보존 — 2026-10-02

[원본 8입력 실행](LDS_RMC_INHERITANCE_2026-10-02.md)에서는 마지막 MODE와
아홉 값이 A 기준으로 돌아와도 UTC·방위각·속도가 V 할당에서 상속됐습니다.
소비자가 중간 응답을 놓치면 마지막 숫자·할당 번호만으로 A/V를 복원할 수
없습니다. 이를 위해 실제 HEADING 할당에 `heading_rmc_status`를 보존합니다.

## 변경 범위

상태는 두 번째 RMC 데이터 token의 `unknown`·`empty`·`a`·`v`·`other`입니다.
완전한 단일 frame의 경계를 확인한 뒤 course 존재와 독립적으로 분류합니다.
체크섬 문법만 검사하며 실제 parser의 수용을 대신하지 않습니다. 두 정보는
기존 parser→callback→첫 실제 캐시 쓰기의 소유권을 함께 한 번 소비합니다.

HEADING을 쓰는 RMC에서는 그 입력의 상태를 기록하고, GGA/GSA에서는
실제로 저장해 둔 read의 상태를 상속합니다. 중간에 V 쓰기가 있어도 예전
A read로 복사하면 A 출처와 상태가 함께 복원됩니다. 거부·무쓰기 입력은
커밋 상태를 바꾸지 않으며 알 수 없는 쓰기·수명 종료는 지식을 지웁니다.
UTC·속도까지 같은 RMC 출처로 해석하려면 각각의 할당 번호도 같아야 합니다.

sideband는 640바이트를 유지하며 v3의 572번째 바이트에 새 word를 둡니다.
v1은 course/status 모두 unknown, 정상 v2는 기존 course를 보존하고 status만
unknown으로 읽습니다. 구형 packet의 reserved 영역 오류를 승인하지 않습니다.
공유 mapping도 공통 v3으로 올리고 word130에 상태를 둡니다. 원본 OEM 위치
payload나 송신 계약은 바꾸지 않습니다. 같은 callback의 Owned와 POSITION·
SEND·PC 분석기에 상태를 전달하며 옛 JSON의 필드 부재는 unknown입니다.

이는 lexical 관측 정보입니다. A를 VALID 수신기, 새 물리 측정 시각이나
ASSIST 자격으로 취급하지 않습니다. 새로운 MODE/출처 혼합 거부 규칙을
추가하지 않으며 live provenance 공급부와 ASSIST 활성화는 계속 미구현입니다.

## 이번 검증

구현 전 동작 실패와 수정 후 통과를 별도 보존했습니다. 컴파일 오류를
기능 실패로 세지 않았으며 모두 작성한 host fixture 검사입니다.

| 범위 | 결과 |
|---|---|
| token 분류 | 10사례·277검사 통과 |
| 실제 read 상속 ledger | 6사례·87검사 통과 |
| parser/callback/cache hook | 새 3개를 포함한 71사례 통과 |
| 전송·소비·adapter·formatter | 집중 실행 75회 모두 종료 0, skip 0 |
| 실제 C++ 출력과 Python 분석기 연결 | 44개 unittest 통과, skip 0 |

작성 parser의 거부·무쓰기, 같은 workspace 재사용·중첩·두 번째 쓰기·
수명 경계와 오래된 read 복원을 확인했습니다. 상태만 다른 중복 응답의
충돌 처리와 이전 Owned 불변, 다섯 상태의 실제 map→adapter→POSITION/SEND
전파도 검사했습니다. 잘못된 보조 정보가 원시 POSITION/SEND와 MODEL
분석을 지우지 않는지 대조했습니다. 최대 sideband JSON fixture는
3,109/4,096바이트였고 정확한 크기·한 바이트 부족·경계 보존을 검사했습니다.

별도 Claude 검토는 시간 제한 없이 한 번 실행되어 134.159초 뒤 완료됐습니다.
제안은 코드·native 증거와 따로 대조했으며 v2 course까지 지우라는 잘못된
기대는 채택하지 않았습니다. 이 리뷰를 target 실행 검증으로 세지 않습니다.

이번 체크포인트에서는 **전체 host/ARM, 새 여섯 제품의 ABI/TLS,
실제 제품 DSO 및 새 제품의 원본 reader 실행은 아직 미검증**입니다.
고정 커밋으로 별도 수행하여 후속 기록에 남깁니다. 공개
`v0.3.11-shadow.1`의 ZIP은 변경하지 않았고 새 릴리즈도 발행하지 않았습니다.
물리 센서·차량 기동/복구·휴대폰/앱 수용과 v1.0은 계속 미완료입니다.
