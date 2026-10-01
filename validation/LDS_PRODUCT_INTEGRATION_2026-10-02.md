# LDS 제품·설치 통합 — 2026-10-02

관성항법 위치를 실제 AA 송신에 적용하기 위해 필요한 원본 위치의 할당
출처를 LDS 프로세스에서 관측하도록 구현했습니다. 별도
`libmx5dr-ldstap.so`가 원본 LDS의 cold load에서 설치되며 기존 AA worker의
용량 제한 journal로 응답 연결 정보를 보냅니다. 이 문서는 구현 단계의
검증입니다. 최종 고정 소스 전체 검사·최종 ZIP·공개 배포를 대신하지 않습니다.

물리 생산 시각·입력 품질·폰 수용은 아직 미검증이며 live ASSIST는 계속
비활성입니다. 작성한 NMEA·직접 API 호출이나 QEMU 성공을 정상 전체 차량
기동 또는 v1.0 완성으로 표시하지 않습니다.

## 제품과 설치 동작

- LDS 설치기는 원본 여섯 모듈과 정확한 코드·데이터 위치를 검증한 뒤
  25개 데이터 슬롯을 변경합니다. 원본 코드는 그대로이며 준비한 원본
  함수 주소와 모듈은 프로세스 종료까지 유지합니다.
- OBSERVE·SCRUB·SHADOW의 normal/WCP 시험 템플릿에 LDS preload를
  넣습니다. OFF에서는 추가하지 않습니다. 기존 AA 터치 설정과 다른
  preload는 보존합니다. 센서 수신·시각 자격이나 sideband receiver의
  존재를 설치·기동의 새 조건으로 넣지 않았습니다.
- 패키지는 LDS 제품을 포함한 여섯 ARM 산출물을 검사·설치합니다.
  일회성 guard v3는 LDS 제품 SHA를 마지막 여덟 번째 입력으로
  결합합니다. 기존 disarm·한 부팅 소비·다음 부팅 원본 복귀 정책을
  유지합니다. 새로운 측정 자격을 guard로 구현한 것은 아닙니다.
- 회수는 기존 v2와 새 v3 표식을 모두 해석하고 LDS 파일 해시도
  수집합니다. 이전 설치에 LDS 파일이 없어 진단이 partial이어도 원시
  자료 회수는 진행합니다. 제거는 소유한 LDS 토큰만 지우며 실행 중일
  수 있는 라이브러리와 로그를 보존합니다.
- LDS 관측기는 자체 영구 로그나 worker를 추가하지 않습니다. 실제
  응답 뒤 비차단 datagram을 보내고 기존 AA 기록 한도 안에서
  저장합니다. 수신 불가·관측 누락은 원본 전달을 막지 않습니다.

## 원본과 일치하지 않던 설치 fixture 수정

독립 검토에서 새 firmware manifest의 `libdbus-1.so.3`가 실제로는
심볼릭 링크임을 발견했습니다. 기존 host fixture는 `copyfile`로 링크를
일반 파일로 펼쳐 복사하여 설치 성공으로 통과했습니다. 실제 순정 구조는
기존 `regular()` 검사에서 설치 전에 거부되는 조건이었습니다.

순정 target bytes와 상대 symlink를 보존한 새 회귀를 먼저 실행하여
stock·synthetic 두 경우 모두 동일한 설치 거부를 재현했습니다. manifest를
동일한 SHA의 실제 일반 파일 `libdbus-1.so.3.7.2`로 고친 뒤 두 회귀를
통과했습니다. 일반 파일 보호는 변경하지 않았습니다. 여섯 원본 의존성의
변조 거부까지 포함한 4개 시험·12개 변조 하위 사례는 생략 없이
통과했습니다. 이 결과는 host shell 검사이며 실제 BusyBox 최종 ZIP
실행과 구분합니다.

새 LDS 연결의 host packaging 전체 250개가 먼저 통과했지만 위 문제를
검사하지 못했습니다. 해당 과거 통과를 실제 파일 형태의 보장으로
취급하지 않습니다. 비공개 실패·수정 근거는
`evidence/lds-runtime-20261002/worker/libdbus-alias/`에 보존했습니다.

## 빌드와 독립 검사

고정 GCC 4.9.1·binutils 2.22에서 LDS 제품에 section GC를 적용한 초기
빌드는 linker 내부 ARM unwind assertion을 출력했습니다. 종료 0도
산출물 정상의 근거가 아니었고 AA entry의 미해결 참조가 남았습니다.
해당 결과물은 배포하지 않습니다.

실제 AA ARM entry 19줄을 `arm_entry.cpp`로 분리하고 AA 빌드에는 그대로
포함했습니다. LDS는 해당 entry를 제외하되 실제 공통 adapter 무효화
구현을 사용합니다. 가짜 veneer나 성공 stub을 넣지 않았습니다. LDS의
section GC·function sections를 제거한 새 빌드는 assertion·미해결 AA
entry 없이 완료됐습니다. 제품 여섯 개의 새 ARM 빌드와 72개 입력을
기록했습니다. AA wrapper의 EHABI·취소·예외 계약은 새 제품의 전체 ARM
검사에서 별도로 확인해야 합니다.

[원본 설치기 검사](LDS_COLD_INSTALL_2026-10-02.md)의 ARM 71개·호스트
1개, tap 정책의 host 18개·원본 공유 runtime ARM 18개, 별도 generic
loader 공존 host 15개를 통과했습니다. guard v3는 host 33개와 원본
runtime ARM 33개를 통과했습니다. 뒤의 ARM 범위에는 ARM guard 26개와
호스트 AWK 편집 7개가 포함됩니다. 이 횟수를 실제 CMU 전체 부팅으로
세지 않습니다.

설치·진단·회수·빌드 의존성의 신규 회귀는 구현 전 실패를 보존했습니다.
빌드 메타데이터 전체 시험은 구현 전 HEAD를 clone하는 한 사례가 아직
다섯 제품 스크립트를 가져와 실패했습니다. 나머지 28개는 통과했습니다.
이는 uncommitted 여섯 제품 소스와 이전 HEAD를 섞은 검사 조건이며,
해당 검사를 약화하지 않고 새 소스 고정 뒤 전체 재실행으로 확인합니다.

## 남은 검증

자동 제품 로딩·원본 parser부터 service 응답·sideband·실제 AA 기록까지의
연결, 최종 고정 소스 전체 host/ARM, 정상 파일 형태를 유지한 최종 ZIP
BusyBox 실행과 공개 파일 재다운로드를 각각 완료하고 새 근거로 기록해야
합니다. 정상 ServiceInit·SM 전체 수명·실차 복구·물리 센서·폰 수용은
그 검사들과도 별개입니다. [v1.0 완료 조건](../docs/V1_READINESS_KO.md)을
축소하지 않습니다. 임시 도구는 후속 검증을 위해 유지 중이며 최종 정리
완료를 아직 주장하지 않습니다.
