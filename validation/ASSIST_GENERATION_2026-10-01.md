# ASSIST 계산·송신의 상태 전환 연결 — 2026-10-01

제품 소스는 `3d05a2c738f74968fb3da31a5afce2d1d0700152`입니다.
실제 ARM 제품 DSO의 계산기와 adapter를 연결해 검사하여, GPS 상태 전환 뒤
두 구성 요소의 generation이 어긋나는 결함 두 가지를 수정했습니다.
입력과 자격은 작성한 조건이며 live ASSIST 입력 연결이나 차량 검증은 아닙니다.
공개 v0.3.4-shadow.1의 태그와 설치 ZIP은 변경하지 않았습니다.

## 재현한 결함과 수정

이전 `f3556b4`의 실제 ARM DSO에서 다음 실패를 확인했습니다. 이 DSO의 SHA-256은
`8472c2defb7deae3263fbcaacebc3a9ae706c640e62ba770904e0e81f4f1cc74`입니다.

1. GPS mode가 `1 → 2 → 0`으로 바뀌면 adapter는 두 전환을 모두 셉니다.
   계산기는 유효 GPS 품질 변경에 별도 제어를 실행하지 않으므로 한 번만 셌습니다.
   코어 계산은 정상이지만 generation이 계산기 4, adapter 5가 되어
   송신 후보 매핑이 `CORE_BRIDGE_UNQUALIFIED`로 실패했습니다.
2. 새 yaw 구간 없이 GPS가 복귀하면 `drain()`이 먼저 지역 DISABLE을 적용하고
   실제 GPS_RETURN을 처리했습니다. 하나의 관측에 두 번호를 소비하여
   계산기 6, adapter 5가 되었습니다. 위치 계산 API만 따로 검사하면 놓치는
   실제 adapter 관측과의 연결 문제입니다.

qualified 제어는 해당 POSITION 관측에 담긴 generation을 사용합니다.
GPS 품질 전환으로 건너뛴 번호를 보존하고, 뒤로 가거나 같은 번호의 새 제어는
fault로 처리합니다. 입력 구간을 기다릴 수 없는 qualified GPS/기준점 처리는
그 자체로 이전 예측을 철회하므로, 앞선 중복 DISABLE을 제거했습니다.
MODEL의 지역 번호 및 기존 제어 의미는 유지했습니다. GPS/기준점이 대기 중일 때
예측을 숨기는 처리와 adapter의 철회·유효 기간 검사는 유지합니다.

## 제품 경로 시험

새 `tests/adapter/assist_publication_test.cpp`는 실제 adapter callback이 만든
관측을 별도 pthread 시험 worker의 qualified Pipeline에 전달합니다. 기한 계산과
`publish_snapshot` 뒤 20ms 후 실제 송신 adapter를 호출해 작성한 OEM endpoint에서
48바이트 LOCATION 결과를 받습니다. 기대 문맥은 코어의 상태를 복사하지 않고
adapter에서 읽으므로 위 불일치를 검출합니다.

- 직진, GPS 품질 변경, 품질 왕복, 회전, 후진, 기한 만료, 새 기준점 재시작,
  native 위치 복귀, 오래된 제어, 미검증 자격 등 10개 사례입니다.
- 10m/s 직진과 0.2rad/s 정속 회전·후진의 1초 변위를 별도의 원호 식과 대조합니다.
  송신 좌표·UTC·속도와 선택 flag, 원본 payload 불변성, 정확히 한 번의 OEM 호출,
  반환값과 errno를 검사합니다.
- 만료 시각 정각에는 교체하고 1ns 뒤에는 원본을 전달합니다. 별도로 아직
  기한이 남은 후보를 GPS/native 복귀가 철회하는지, 이전 후보 재발행 및 다음
  GAP에서의 재사용을 거부하는지 검사합니다. 미검증 자격 사례의 교체는 0회입니다.
- 재시작 사례는 새로 작성한 검증된 기준점으로 계산기를 초기화합니다.
  live 기준점 생성·출처 인증 또는 실제 runtime 입력 worker를 구현한 것이 아닙니다.

ARM 시험에서는 검사 대상 제품을 수정하지 않고 해당 ELF의 자체 심볼 주소로
Pipeline·코어 연결·adapter 함수를 호출합니다. 제품 소스를 시험 실행 파일에
다시 컴파일해 대신 실행하지 않습니다. 입력 소스와 제품 해시를 전후 대조하고,
기존 위치·요청·세션·버스 DSO 검사와 함께 전체 ARM 실행 목록에 추가했습니다.

## 직접 검증

고정 GCC 4.9.1 도구체인 `61ec0343de84f6fc7c46840056df1d600d44be8a`로
다섯 제품 파일을 새로 빌드했습니다. 새 `libmx5dr.so`의 SHA-256은
`b55320f58278f9c8b0d28b2aa2bf4db82bcc1c840803466c51f704a474d452e9`이며,
나머지 네 파일은 이전 빌드와 같습니다.

| 검사 | 이번 실행 |
| --- | --- |
| 새 호스트 계산→발행→송신 시험 | 10개 사례 통과 |
| 실제 ARM 제품 DSO, 고정 sysroot/QEMU | 같은 10개 사례 통과 |
| 같은 ARM DSO, 제공 펌웨어의 loader·공유 runtime/QEMU | 같은 10개 사례 통과 |
| 전체 host 및 패키징 보완 | Python 371개 항목과 C/C++ 통과, 아래 초회 생략·재검사 기록 참조 |
| 고정 ARM/QEMU 전체 | 종료 0·생략 0, 시작·종료의 다섯 제품 해시와 도구체인 동일 |
| 실제 제품 DSO의 위치 / 요청 / 세션 / 버스 / ASSIST | 8 / 14 / 29 / 31 / 10개 사례 통과 |
| ARM core / navigation / live pipeline | 1,425 / 2,810 / 815개 합성 검사 통과 |
| ARM gyro bias / GPS-wheel / holdout | 2,677 / 84,601 / 5,443개 합성 검사 통과 |

전체 host `make test`는 종료 0이었으나, 임시 추출에서 `usr/bin/aap_service`를
빠뜨려 패키징 20개가 생략됐습니다. 해당 파일을 추출하고 네 펌웨어 identity의
원본 해시 일치를 확인한 뒤 `make test-packaging` 전체 163개를 다시 실행했습니다.
재검사는 종료 0·생략 0이며, 이 보완으로 host Python 371개 항목을 모두 실행했습니다.
초회 생략을 숨기거나 두 실행의 중복 항목을 새 검사 개수에 더하지 않습니다.

순정 loader·libc·libpthread·libm·공유 C++ runtime은 임시 펌웨어 추출본을
사용했습니다. 이 검사는 LDS·AA 전체 서비스, 물리 센서, 차량, 폰을 실행한
결과가 아닙니다. 합성 자격 플래그를 실제 qualification으로 세지 않습니다.

패키징 회귀용 비공개 `mazda-aa-dr-3d05a2c-check.zip`은 기본 SHADOW이고
`source_modified=false`입니다. SHA-256은
`b8761b2cbf926b76e021eb409828f9e3705f31268ce9809f193cf7b8a8bcbaac`입니다.
생성 시 CRC·전체 manifest와 빌드 입력을 확인했습니다. 새로운 GitHub Release로
게시하지 않았으며 이 ZIP의 별도 순정 BusyBox 설치 검사는 수행하지 않았습니다.

## 임시 도구와 남은 범위

`mazda-assist-dso-20261001` 컨테이너를 제거했습니다. 그 안에 추가·갱신한
빌드 도구·D-Bus 개발 파일·QEMU·Git/cpio 등 패키지 73개 행, 고정 도구체인과
펌웨어 임시 추출본도 함께 제거됐습니다. 호스트 패키지·Docker 이미지·컨테이너·
binfmt 목록은 작업 전후 모두 같았습니다. 호스트 패키지 설치나 `sudo` 실행은
없었으며 기존 이미지와 검증 산출물은 보존했습니다. 원본 로그·설치 목록은
비공개 `evidence/assist-worker-20261001/`에 남겼습니다.

live `provenance()`와 `allow_assist=false`는 변경하지 않았습니다. 실제 센서의
생산 시각·품질·단위 및 요청별 provider/session/receiver 근거, qualified 입력을
실제 runtime worker에 연결하는 구현, 차량 부하·복구와 지도 앱의 위치 수용은
여전히 남습니다. 이번 수정은 이 연결을 막던 소프트웨어 결함을 해결한 범위이며
v1.0 완료나 실차 정확도 보증이 아닙니다.
