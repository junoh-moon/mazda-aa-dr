# 같은 계산기의 ASSIST 재획득 — 2026-10-01

제품 소스는 `c56219b475de06da9b2f422792fcc23ba1b84f09`입니다.
GPS 복귀와 새 기준점이 같은 처리 묶음에 들어오면 기준점을 다시 무효화하여
다음 단절에서 계산하지 못하던 결함을 수정했습니다. 동일 계산기를 유지한
연속 GPS 단절을 시험했습니다. live runtime의 qualified 입력 연결과 실차
검증을 완료한 것은 아니며 공개 v0.3.4-shadow.1 ZIP은 변경하지 않았습니다.

## 수정 전후

앞선 시험의 `reacquire` 사례는 새 외부 기준점으로 `init_qualified()`를 다시
호출했습니다. 그 실행 근거는 그대로 보존하지만, 재초기화 없는 연속 처리의
증거는 아니었습니다. 새 사례는 실제 adapter의 POSITION 관측을 사용하면서
동일 Pipeline을 유지합니다.

기준점 이벤트가 먼저 GPS_RETURN을 적용하고 generation 5의 READY 상태를
만들어도, 뒤이은 같은 GPS 복귀 관측이 제어를 다시 적용했습니다. 같은 번호의
제어가 거부되며 generation 6으로 reset되고 새 기준점이 사라졌습니다.
호스트와 이전 실제 ARM DSO에서 `PIPELINE_BAD_INPUT`, reset 1을 재현했습니다.
반면 GPS 관측을 별도 `drain()`으로 먼저 소비한 대조는 수정 전에도 통과했습니다.

수정은 **qualified·seeded·READY인 새 기준점과 generation이 일치하는 GPS
mode 1/2 관측**이 이미 처리한 복귀를 반복하지 않도록 합니다. ACTIVE 추정,
다른 번호, 번호 없는 관측과 MODEL 입력의 기존 처리는 유지합니다. 센서 lease나
시각을 갱신하지 않고, 다음 GAP에는 새 관측 generation이 계속 필요합니다.

## 시험 범위

- 기존 10개 송신 사례를 유지하고, 재초기화 없는 복귀, 기준점 우선 입력,
  별도 drain, native 위치를 거친 복귀, GPS mode 2 복귀 등 5개를 추가했습니다.
- 각 연속 사례는 같은 worker·Pipeline에서 두 단절을 처리하고 보정 송신 20회를
  확인합니다. reset은 0이며 계산 횟수·solution sequence가 계속 증가합니다.
  새 기준점의 좌표·UTC와 기존 예측 철회, OEM 원본·반환값·errno 보존도 검사합니다.
- 별도 항법 회귀는 측정 시각이 callback 수신보다 20ms 이른 기준점을 사용합니다.
  원본 센서 구간과 시각을 유지한 채 새 기준점 이후 90ms의 0.9m 변위·UTC를
  확인합니다. 맞지 않는 오래된 generation은 여전히 reset·무효 결과입니다.

실제 제품 DSO 시험은 그 ELF의 자체 심볼로 Pipeline·adapter를 호출합니다.
제품을 변형하거나 제품 소스를 시험 실행 파일에 대신 링크하지 않습니다.
이전 실패 DSO의 SHA-256은
`b55320f58278f9c8b0d28b2aa2bf4db82bcc1c840803466c51f704a474d452e9`입니다.

## 직접 검증과 산출물

고정 GCC 4.9.1 도구체인 `61ec0343de84f6fc7c46840056df1d600d44be8a`로
다섯 ARM 제품을 새로 빌드했습니다. 새 `libmx5dr.so`의 SHA-256은
`049a81ffc58bb3e373bb14ce3c620115a3c80c7ec62d0efb4514ca9127e47e0f`입니다.
나머지 네 파일은 이전 빌드와 같습니다.

| 검사 | 이번 실행 |
| --- | --- |
| 호스트 계산→발행→송신 | 15개 사례 통과 |
| 실제 ARM DSO, 고정 sysroot/QEMU | 15개 사례 통과 |
| 같은 DSO, 제공 펌웨어 공유 runtime/QEMU | 15개 사례 통과 |
| 전체 host `make test` | Python 371개와 C/C++·worker·패키징 통과, 종료 0·생략 0 |
| 고정 ARM/QEMU 전체 | 종료 0·생략 0, 시작·종료의 제품 다섯 파일과 도구체인 동일 |
| 실제 제품 DSO의 위치 / 요청 / 세션 / 버스 | 8 / 14 / 29 / 31개 사례 통과 |
| ARM core / navigation / live pipeline | 1,425 / 2,875 / 815개 합성 검사 통과 |
| ARM gyro bias / GPS-wheel / holdout | 2,677 / 84,601 / 5,443개 합성 검사 통과 |

입력·자격·OEM endpoint와 worker 스케줄은 작성한 조건입니다. 순정 공유
runtime을 사용한 검사는 LDS·AA 전체 서비스나 차량·폰 실행이 아닙니다.
이전 기록의 검사 개수를 이 실행 결과로 바꾸지 않습니다.

패키징 회귀용 비공개 `mazda-aa-dr-c56219b-check.zip`은 기본 SHADOW이며
`source_modified=false`입니다. SHA-256은
`aae3867838c9803b841fc5a7097fd7a043f1cb0049093f410e1d60ecfd11533d`입니다.
CRC·전체 manifest와 제품 빌드 입력을 검사했습니다. 새 릴리즈 게시나 이 ZIP의
별도 순정 BusyBox 설치 검사는 수행하지 않았습니다.

## 정리와 남은 구현

`mazda-assist-reacquire-20261001` 컨테이너를 제거했습니다. 그 안에 추가·갱신한
빌드 도구·D-Bus 개발 파일·QEMU·Git/cpio 등 패키지 73개 행, 고정 도구체인과
펌웨어 임시 추출본도 제거됐습니다. 호스트 패키지·Docker 이미지·컨테이너·
binfmt 목록의 전후 동일성을 확인했습니다. 호스트 패키지 설치나 `sudo` 실행은
없었습니다. 기존 이미지와 검증 산출물은 보존했고, 원본 로그·설치 목록은
비공개 `evidence/assist-reacquire-20261001/`에 남겼습니다.

실제 runtime worker는 여전히 MODEL 입력만 연결되어 있습니다. qualified
센서·기준점의 생산 시각·품질·단위, 요청별 provider/session/receiver 근거와
실제 worker 입력·발행 연결은 미완료입니다. 이번 변경은 그 연결에 필요한
연속 계산 동작을 수정한 범위입니다. live ASSIST 비활성 조건을 유지하며
차량·지도 앱 수용, 물리 복구와 v1.0 완료를 주장하지 않습니다.
