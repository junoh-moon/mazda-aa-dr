# 원본 LDS 부분 갱신·잠금 snapshot·응답 연결 — 2026-10-01

NA 74.00.324A의 원본 NMEA parser, 등록된 LDS callback, 위치 캐시,
`GetPosition` 서비스와 생성된 serializer를 실제 실행했습니다. 작성한 NMEA
입력이 원본 캐시를 거쳐 원본 응답과 제품 request token에 이르는 경로를
확인했습니다. **캐시를 한 번에 복사해도 아홉 필드가 같은 입력에서 갱신됐다는 뜻은 아닙니다.**
이 검증은 물리 센서나 live ASSIST 자격을 입증하지 않습니다.

## 대상과 실행 범위

- 제품은 공개 v0.3.5-shadow.1의 변경하지 않은 `libmx5dr.so`입니다.
  소스는 `3ec606b61950d36be5fe80313f5a167d28495a65`, SHA-256은
  `075657004d7ec4cea030adda8a3da508f02d593500f1f8f4ff8735ca21204fb7`입니다.
  후속 master의 qualified callback 짝짓기 변경을 이 바이너리의 결과에
  포함시키지 않습니다.
- 기존 격리 이미지에 GCC 4.9.1 고정 도구체인, QEMU user 7.2.22와
  native private D-Bus daemon 1.14.10을 준비했습니다. ARM 프로그램은
  해당 펌웨어의 loader·glibc 2.11.1·LDS·JCIDBUS·libdbus를 사용했습니다.
  도구체인 2,124개 blob과 원본 모듈 해시를 확인했습니다.
- parser의 지속 작업 버퍼 최초 0 초기화, 서비스 mutex 초기화 순서,
  NMEA 입력 순서와 `TZ=UTC0`은 작성한 조건입니다. 원본 parser 결과를
  원본 callback에 넘기며 파싱된 필드를 작성 코드로 채우지 않았습니다.
- 원본 data interface `Open`, 객체 등록, 실제 JCIDBUS worker, 원본
  service의 잠금 안 출력 복사와 serializer·raw 전송을 실행했습니다.
  클라이언트는 실제 제품 cold installer와 원본 data-client를 사용합니다.
  제품 GOT 20개·코드 진입 4개·파일 해시 5개의 설치 계약도 확인했습니다.
- 클라이언트의 즉시 `WorkerScope` 소비와 마지막 worker stop/disconnect/free
  순서는 작성한 조건입니다. 전체 OEM `ServiceInit`/`ServiceDestroy`, SM
  기동·manager queue·실차 실행을 수행한 것으로 표시하지 않습니다.

## 연결 방법

원본 위치 mutex 안에서 관측한 cache 쓰기에 관측기 자체 `write_seq`를
부여했습니다. 해당 callback 호출을 감싼 작성 입력 번호도 보존했습니다.
실제 `GetPosition`이 같은 mutex 안에서 아홉 출력 인자를 복사할 때
그 출력과 `write_seq`를 함께 기록했습니다. 이는 **캐시 쓰기 호출의 출처**이며,
보존된 개별 필드까지 마지막 callback에서 새로 측정됐다는 뜻은 아닙니다.

원본 path→method→reply→raw reply 객체 연결과 D-Bus bus GUID, client
unique name+request serial, server unique name+response serial+reply serial로
제품 token까지 연결했습니다. 독립 monitor의 원본 헤더도 대조했습니다.
본문·좌표·UTC·가까운 수신 시각·실험 label은 연결 키로 사용하지 않았습니다.
본문의 아홉 값 비교는 identity 연결 후 수행했습니다. monitor의 출력 정밀도를
원본 수치의 bit 단위 증거로 쓰지 않았습니다.

## 실제 실행

| 실행 | 작성 입력 | 관측 cache 쓰기 | 실제 응답 | 목적 |
| --- | ---: | ---: | ---: | --- |
| 최초 연결 | 7 | 7 | 9 | 초기 캐시·부분 갱신·무갱신 재조회 |
| serial 분리 | 7 | 7 | 9 | client의 별도 daemon 질의로 요청/응답 serial을 다르게 함 |
| 같은 바이너리, 서버 계측 off | 7 | 미계측 | 9 | 계측 on/off의 반환값·순서 대조 |
| snapshot 이후 새 쓰기 | 7 | 7 | 10 | 새 cache 쓰기 전에 복사한 응답의 귀속 |

모든 실행의 원본 서버와 제품 클라이언트는 exit 0으로 끝났고, private daemon과
socket을 정리했습니다. 독립 monitor는 회수 후 의도적으로 종료했습니다.
입력 없는 초기 cache의 `write_seq=0`은 생산자를 관측하지 못한 상태로
남겼습니다. 입력을 추가하지 않은 재조회는 새 생산자 갱신으로 세지 않았습니다.

작성 입력에서 확인한 차이는 다음과 같습니다.

| 마지막 입력 | 응답에서 확인한 동작 |
| --- | --- |
| A RMC | A UTC·좌표·방향·속도가 들어옵니다. |
| GSA | horizontal·vertical 값이 갱신되고 기존 위치 필드는 유지됩니다. |
| A GGA | 고도가 갱신됩니다. |
| B GGA만 입력 | 좌표·고도는 B이지만 UTC·방향·속도는 A 값입니다. |
| 같은 cache 재조회 | 동일한 쓰기 번호와 복합 필드가 다시 반환됩니다. |
| B RMC | UTC·방향·속도가 B로 바뀌고 GGA 고도가 유지됩니다. |
| 무효 C GGA | mode 0과 C 좌표·고도가 나오지만 UTC·방향·속도는 B 값입니다. |

경합 실험은 원본 mutex unlock이 **실제로 반환된 후** service worker를
최대 5초 기다리게 했습니다. 기다리는 동안 main이 원본 B RMC callback으로
cache 쓰기 5를 완료한 뒤 worker를 재개했습니다. snapshot 7의 실제 응답은
이전 쓰기 4의 UTC·방향·속도를 유지했고, 다음 snapshot 8부터 쓰기 5의
값을 반환했습니다. 관측 종료 시점의 최신 cache로 이전 응답을 설명하면
잘못된 연결이 됩니다. 이 대기 순서는 작성한 진단 스케줄입니다.

별도 pthread 회귀는 대기 기능의 미구현 실패를 먼저 확인한 뒤, 실제 mutex
해제 후 쓰기, 이전 출력 보존, bounded timeout, 취소 후 자체 mutex 해제와
계측 off를 검사했습니다. host와 고정 ARM/원본 공유 runtime에서 통과했습니다.

독립 비교기는 최초 연결·serial 분리·경합 실행을 모두 통과했습니다. 같은
바이너리의 계측 on/off는 각 버스에서 원본 헤더를 확인한 뒤 아홉 응답의
본문·공개 serial 필드·순서를 대조했습니다. 버스 사이의 동일 unique name이나
serial을 동일 사건으로 묶지 않습니다. 비교기 자체 회귀 30개도 통과했습니다.
최종 비교기 SHA-256은
`86c87b584a9ccd1ffd0c479ed45e381f05328e09b4550332fb4a46db5c5d3d8d`입니다.
경합 실행의 관측 JSONL SHA-256은
`ff0ee04806113b5923bffc9b5afd5fa77258a63d0b567fbea7dd937bebbfa675`입니다.
각 실행의 입력 소스 전후 해시·제품/fixture 해시·로그 해시·종료 코드를 별도로 보존했습니다.

## 제품에 반영할 의미와 남은 범위

원본 응답 하나를 받았다는 사실이나 성공한 polling으로 모든 필드의 fresh/VALID
상태를 만들 수 없습니다. 제품의 live `provenance()`·실제 qualified 입력 공급부는
여전히 미구현이며 ASSIST를 켜지 않았습니다. MODEL의 receipt 기준 계산과 GPS
holdout 차이를 물리 정확도나 qualified 관성항법으로 승격하지 않습니다.

이번 검토에서 제품 `position` journal이 반환 필드 중 고도·horizontal·vertical을
버리는 문제도 찾았습니다. 후속 제품 소스는 `altitude_m`, `horizontal`, `vertical`을
추가하여 원본 callback의 아홉 값을 기록합니다. 누락으로 실패한 새 회귀 3개를
수정 후 기존 검사와 함께 host Python 18개·C++ journal에서 통과했습니다.
고정 최대값 조건의 POSITION 행은 NUL 포함 4,790바이트로 기존 5,120바이트
버퍼에 들어가며 정확한 크기·한 바이트 부족·주변 canary도 확인했습니다.
0·음수는 그대로, nonfinite 실수는 기존 JSON 정책에 따라 `null`로 기록합니다.
계산·자격·저장 공간 정책은 바꾸지 않습니다. ARM의 같은 Python formatter
검사도 전체 runner에 추가했습니다. 최종 실행과 실제 제품 DSO의 직접 검사는
[v0.3.7 릴리즈 검증](RELEASE_V037_2026-10-01.md)에 기록했습니다.
위 v0.3.5 원본 LDS 실행에는 그 후속 제품 변경이 포함되지 않습니다.

실제 제품에 넣을 producer 관측 수명 관리, 개별 필드 생산자·측정 시각,
프로세스 간 provenance 공급부, 전체 SM 기동과 폰/지도 앱 수용은 남았습니다.
원본 바이너리·맵·덤프와 private 실행 로그는 게시하지 않습니다.
비공개 근거는 `evidence/lds-field-lineage-20261001/`에 보존했습니다.

같은 환경에서 후속 제품 검증을 마친 뒤 임시 컨테이너·추가/갱신 패키지 79행·
고정 도구체인과 추출본을 제거했습니다. 최종 ZIP 검사에 쓴 host QEMU 복사본·
binfmt entry·helper도 제거했고 최초 호스트 목록과 일치했습니다.
호스트 패키지를 설치하거나 `sudo`를 실행하지 않았습니다. 사용자 펌웨어와
기존 이미지는 보존했으며 전후 목록과 정리 결과는 위 릴리즈 검증을 따릅니다.
