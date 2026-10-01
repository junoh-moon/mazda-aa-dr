# 실제 runtime worker의 ASSIST 연결 — 2026-10-01

제품 소스는 `23133739c6adf8293e9ff6eeadc9b33b0bea668b`입니다.
외부에서 검증한 입력을 기존 runtime worker가 소비하고, qualified 계산 결과를
실제 adapter에 발행·철회하는 경로를 구현했습니다. 이전 시험 전용 worker와
달리 제품의 `run_worker()`가 계산기와 journal을 함께 운용합니다.

**live 입력 공급부는 여전히 미구현입니다.** 물리 센서의 생산 시각·품질·단위,
기준점 검증과 요청별 provider/session/receiver 근거를 공급하는 backend는
추가하지 않았습니다. 정상 기동은 공급부 없이 같은 worker를 실행합니다.
ASSIST 설정 거부, `allow_assist=false`, live provenance 실패를 유지하며,
raw 입력이나 MODEL 계산을 qualified 입력으로 승격하지 않습니다.

## 수정 전 실패와 처리

- 실제 runtime 경로를 연결하기 전 합성 OEM endpoint 시험에서 공급부 호출이
  `reads=0`이고 보정 송신이 없어 `replacement` assertion이 실패했습니다.
  controller 자체도 명시적인 미구현 stub 상태에서 `PUBLISHED` 검사가
  실패하는 것을 먼저 확인했습니다. 구현 후 같은 제품 worker 경로가 통과했습니다.
- tick의 첫 시각을 모든 입력의 미래 시각 검사에 사용하면, 처리 도중 정상
  도착한 입력을 거부했습니다. pop 도중 실제 시계가 40ms 진행하는 회귀의
  실패를 확인하고, 각 입력을 소유한 뒤 시계를 다시 읽도록 수정했습니다.
  측정·수신 시각을 덮어쓰지 않으며 계산은 최초 producer watermark까지만
  진행합니다. 나중 watermark는 다음 tick에서 사용합니다.
- source/session epoch 전환을 처음 발견한 tick 시각이 새 BEGIN보다 늦으면,
  이미 검증해 큐에 넣은 정상 BEGIN을 과거 복구 입력으로 잘못 거부했습니다.
  새 source와 새 session 모두 수정 전 실패를 재현했습니다. 복구 시각 제한을
  자격을 잃은 epoch에 연결해 새 epoch의 원래 시각을 보존합니다. 같은 epoch의
  자격 상실 후 과거 BEGIN·기준점 재사용을 거부하는 대조도 유지했습니다.
- worker 상태 기록이 없는 경우 journal assertion이 실패함을 확인한 뒤
  `assist_worker` 진단을 추가했습니다. 원본 관측을 먼저 기록하고 계산·발행과
  진단을 진행합니다. 중단 시에도 원본 queue와 꼬리를 배출한 뒤 진단과 완료 기록을 남깁니다.

## 입력·발행 계약

BEGIN, 원본 POSITION 복사본, qualified 기준점과 정규화된 속도·요레이트·후진
입력을 받습니다. 공급부와 clock callback은 bounded/nonblocking 계약이며
한 tick에서 최대 128개 입력을 소비합니다. 처리 한도 도달은 EMPTY가 아니므로
미처리 GPS 복귀·fault를 건너뛰어 발행하지 않고 후보를 철회합니다.

각 입력의 source/session 문맥과 실제 POSITION generation을 보존합니다.
센서 생산자의 `evidence.source_epoch`는 요청 provenance의 source epoch와
별개의 식별자이며, 두 값을 동일하다고 가정하지 않습니다. 계산 후 시각과
자격을 다시 확인하고 현재 adapter generation과 일치하는 결과만 발행합니다.
경쟁 중 generation이 바뀌면 이전 ready 결과를 새 번호로 붙여 발행하지 않습니다.

발행 기한은 원본 센서 lease, snapshot age와 시간·거리·오차 예산으로 제한합니다.
EMPTY 조회나 재발행은 측정 시각·UTC·좌표를 갱신하지 않습니다. source fault나
자격 상실은 현재 generation의 non-ready 후보를 발행하여 철회하며 generation을
불필요하게 증가시키지 않습니다. 같은 epoch의 복구에는 상실 이후의 새 BEGIN과
기준점이 필요합니다. 정상 GPS 재획득은 동일 worker·계산기에서 이어집니다.

중단 요청, journal/audit 실패, 후크 미설치와 SHADOW 모드에서도 qualified
발행이 실행되지 않는지 검사합니다. raw 관측·기록의 기존 순서를 유지합니다.
`AssistStatus.last_publication`은 **가장 최근 ready 발행의 진단 사본**입니다.
이 값이 남아 있어도 현재 후보가 유효하다는 뜻은 아니며 현재 `state`와
`withdrawals`가 이후 철회를 나타냅니다.

## 직접 검증

| 검사 | 이번 실행 |
| --- | --- |
| host / ARM controller | 각각 12,643개 assertion 통과 |
| host 실제 runtime worker | 9개 사례 통과 |
| 실제 ARM 제품 DSO / 고정 sysroot / QEMU | 실제 runtime worker 9개 사례 통과 |
| 같은 DSO / 제공 펌웨어 공유 runtime / QEMU | 같은 9개 사례 통과 |
| 전체 host `make test` | Python 371개와 C/C++·worker·패키징 통과, 종료 0·생략 0 |
| 고정 ARM/QEMU 전체 항목 | 시험 링크 보완 후 중단 지점부터 끝까지 재개하여 통과, 생략 0 |
| 제품 DSO 위치 / 요청 / 세션 / 버스 / 기존 ASSIST | 8 / 14 / 29 / 31 / 15개 사례 통과 |
| ARM core / navigation / live pipeline | 1,425 / 2,875 / 815개 합성 검사 통과 |
| ARM gyro bias / GPS-wheel / holdout | 2,677 / 84,601 / 5,443개 합성 검사 통과 |

ARM 최초 실행은 기존 core·journal·세션·버스 worker·수집기·loader 검사를
통과한 뒤 **새 controller 시험 실행 파일의 `clock_gettime` 링크 누락**으로
종료 1이었습니다. 구형 glibc의 `-lrt` 링크를 보완했으며 시험은 실제 ARM
adapter·veneer 의존성을 함께 사용합니다. 동일 제품과 도구체인으로
그 명령부터 마지막 검사까지 실행하여 종료 0을 확인했습니다. 최초 한 번의
전체 스크립트가 종료 0이었다고 주장하지 않습니다. 최초 시작·재개 시작·재개
완료의 제품 다섯 파일과 도구체인 확인 JSON 3개가 모두 같고
`release_verified=true`였습니다.

순정 공유 runtime의 DSO fixture에서는 GCC 4.9 aliasing 경고를 typed object
pointer 보관으로 해결하고 `librt`를 연결했습니다. 시험 보완 커밋은
`21726fc6156d63e312a3b5b9175b54c84459df36`이며 제품 바이너리·자격·시각 제한은
바꾸지 않았습니다. ARM 명령과 fixture에서 경고를 끄지 않았습니다.

9개 사례는 `publication`, `source_fault`, `unqualified`, `recovery`, `audit`,
`journal_failure`, `pre_stopped`, `unhooked`, `shadow`입니다. 제품 adapter의 OEM 호출
인터페이스를 사용해 보정 송신, 만료 전 철회, 새 기준점 복구, 원본 payload·
exactly-once send·반환값·errno와 journal 기록을 검사합니다. 입력·자격과
OEM endpoint는 작성한 시험 조건입니다. 공급부·기동 설정은 fixture에서만
명시적으로 주입하며 정상 설치 경로를 활성화하지 않습니다. 실행 스케줄에 따른
송신 횟수 자체를 성능이나 차량 처리량의 근거로 사용하지 않습니다.

DSO 시험은 제품 ELF의 자체 심볼로 실제 worker·controller·adapter를 호출합니다.
controller나 계산기를 시험 실행 파일에 다시 링크하지 않습니다. 제품 DSO의
SHA-256은 다음과 같습니다.

`7ded837015630f1b8b283f927b307933dc8978ec186de5e187e9038e03e6f038`

고정 GCC 4.9.1 도구체인의 commit은
`61ec0343de84f6fc7c46840056df1d600d44be8a`입니다. 제공 펌웨어의 공유 라이브러리로
실행한 QEMU 검사는 OEM LDS·AA 전체 서비스, 실제 센서·차량·폰 실행이 아닙니다.
차량 위치 정확도나 지도 앱의 실제 보정 수용을 입증하지 않습니다.

소스를 위 제품 commit에 고정한 비공개 패키징 검사 ZIP은 기본 SHADOW이며
`source_modified=false`입니다. CRC·전체 manifest와 제품 빌드 입력을 검사했습니다.
`mazda-aa-dr-2313373-check.zip`의 SHA-256은 다음과 같습니다.

`7beb6d812cad785fba923d41a2e612886122a7392082854be635d71e15a019a4`

공개 v0.3.4-shadow.1 ZIP은 유지했습니다. 새 릴리즈 게시와 이 ZIP의 별도
순정 BusyBox 설치 실행은 수행하지 않았습니다. 패키징 회귀는 위 host 전체
검사에 포함되며, 차량 설치·정상 전체 OEM 서비스 실행은 하지 않았습니다.

## 통합과 도구 정리

구현 `2313373`과 시험 보완 `21726fc`를 기존 `feat/session-observation` 및
master에 push했습니다. 중간 fetch와 pull에서 추가 외부 변경은 없었습니다.
이전 검증 기록의 검사 수·제품 해시는 변경하지 않았습니다.

`mazda-assist-runtime-20261001` 컨테이너를 제거했습니다. 그 안에 추가·갱신한
빌드 도구·D-Bus 개발 파일·QEMU·Git/cpio 등 패키지 73개 행, 고정 도구체인과
펌웨어 임시 추출본도 제거됐습니다. 호스트 패키지·Docker 이미지·컨테이너·
binfmt 목록의 전후 동일성을 확인했습니다. 호스트 패키지 설치나 `sudo` 실행은
없었습니다. 기존 이미지와 검증 산출물은 보존했고, 패키지 목록·실행 로그·
제품 입력 대조와 시험별 JSON은 비공개 `evidence/assist-runtime-20261001/`에
남겼습니다.

실제 live 공급부와 요청별 qualification, 차량 센서·복구·폰 수용 검증은 남아
있습니다. 이번 구현은 qualified 계산을 제품 worker에 연결한 범위이며,
live ASSIST 활성화나 v1.0 완료를 주장하지 않습니다.
