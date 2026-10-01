# ASSIST 계산 결과의 비동기 송신 기한 — 2026-10-01

소스 `f3556b4cfe31781b39f2ec96409848433a2463e8`의 오프라인 구현입니다.
관성항법 결과를 나중 OEM callback에 전달하기 위한 계산 파이프라인 API를
추가했습니다. live 센서·요청 자격 입력과 실제 runtime의 qualified 발행 연결은
미구현이며 `provenance()`와 `allow_assist=false`는 변경하지 않았습니다.
공개 v0.3.4-shadow.1의 태그·설치 파일은 바꾸지 않았습니다.

## 동작

기존 `map_core_snapshot`은 호출자가 확인한 기한만 보존합니다. 현재 시각의
코어 결과만 확인한 호출자가 그 시각을 전달하면 1ns 뒤 송신에는 이미 만료됩니다.
이는 기존 API의 계약입니다. 이를 유지하면서 `prepare_core_publication`이
같은 코어를 미래 시각에서도 직접 확인하여 비동기 사용 기한을 구하도록 했습니다.

- 요청한 시각은 상한입니다. 원본 센서 lease, 코어와 호출자 양쪽의 snapshot 나이,
  시간·오차 한도와 현재 거리 한도를 모두 통과하는 범위만 반환합니다.
- 고정된 계산 상태에서 나이·경과 시간·오차 예산은 증가하므로 정수 이분 탐색을
  사용합니다. 150ms 상한으로 최대 28회입니다. worker에서 호출하도록 만든
  API이며 OEM 송신 callback에 이 연산을 추가하지 않았습니다.
- 좌표를 더 적분하거나 원본 frontier·UTC를 송신 시각으로 고치지 않습니다.
  입력의 유효 기간도 갱신하지 않습니다.
- `Pipeline::qualified_publication`은 이미 도래한 GPS/기준점 전환을 거부하며,
  대기 중인 미래 전환이 있으면 기한을 그 직전까지로 줄입니다.
- 발행 이후의 세션·요청 출처·generation과 송신 시각 판정은 기존 adapter의
  책임입니다. 이 API가 물리 센서 검증이나 live ASSIST 허용을 대신하지 않습니다.

현재 시각까지만 매핑하는 초기 경로에서 기한 확장 assertion이 실패했고,
파이프라인의 같은 결손도 별도로 재현했습니다. 수정 후 실제 adapter와 작성한
OEM send 경계를 사용하는 통합 시험에서 13회 exactly-once 호출을 통과했습니다.
20ms 지연 뒤 교체 송신, 기한 정각의 송신과 다음 1ns의 OEM 원본 전달, GPS 복귀
뒤 이전 예측 거부를 확인했습니다. 시간·센서 lease·snapshot 나이·오차 한도의
개별 축소, MODEL 거부, 출력 초기화와 null output도 검사했습니다.
20m/s와 0.3m/s 불확실성에서 0.203m 여유를 약 10ms로 제한하는 경계는
코어와 호출자의 한도를 각각 줄여 확인했습니다. 자격 플래그와 입력은 합성입니다.

## 직접 검증

깨끗하게 커밋한 소스에서 고정 GCC 4.9.1 도구체인
`61ec0343de84f6fc7c46840056df1d600d44be8a`로 다섯 ARM 파일을 새로 빌드했습니다.
새 제품 DSO의 SHA-256은
`8472c2defb7deae3263fbcaacebc3a9ae706c640e62ba770904e0e81f4f1cc74`입니다.
나머지 네 파일은 v0.3.4와 같습니다. 다음 전체 검사는 부하를 겹치지 않고
host 이후 ARM 순서로 실행했습니다.

| 검사 | 이번 실행 |
| --- | --- |
| 전체 host `make test` | Python 371개와 C/C++·worker·패키징 통과, 종료 0·생략 0 |
| 고정 ARM/QEMU 전체 | 종료 0·생략 0, 시작·종료의 다섯 제품 해시·도구체인 동일 |
| ARM core / publication 통합 | 1,425개 합성 검사 / 13회 exactly-once 송신 통과 |
| ARM navigation / live pipeline | 2,810 / 815개 합성 검사 통과 |
| ARM gyro bias / GPS-wheel / holdout | 2,677 / 84,601 / 5,443개 합성 검사 통과 |
| 실제 제품 DSO의 위치 / 요청 / 세션 / 버스 | 8 / 14 / 29 / 31개 합성 사례 통과 |
| 실제 제품 preload loader | 비대상 동시 load 400회와 null path·NOLOAD 통과 |

새 API의 동작은 해당 소스를 링크한 host/ARM fixture에서 검사했습니다.
실제 제품 DSO 검사는 기존 OEM 호출 계약의 회귀 검사이며 새 API가 live worker에서
호출됐다는 증거가 아닙니다. 물리 ARM·OEM 전체 서비스·차량·폰 실행도 아닙니다.

패키징 검사용 비공개 `mazda-aa-dr-f3556b4-check.zip`은 기본 SHADOW이고
`source_modified=false`입니다. SHA-256은
`51160cdbc6f542b21676c398ca46f043caffe95f1cf50e4511adfe778fc33ddc`입니다.
빌더가 CRC·전체 manifest와 제품 입력을 검사했고, 이를 풀어 host 설치 fixture에
사용했습니다. 새 GitHub Release로 게시하지 않았습니다. 설치기 변경이 없어 이
후속 묶음의 별도 BusyBox chroot 설치 검사는 반복하지 않았습니다. 공개 v0.3.4의
최종 ZIP은 같은 작업에서 [순정 BusyBox 설치·다운로드 검증](RELEASE_V034_2026-10-01.md)을
직접 완료했습니다. 이를 후속 묶음의 직접 설치 결과로 세지 않습니다.

검사 뒤 외부 master `3c21fc3`의 GPT 롤오버·LDS 재개 문서를 `bd7a758`에 병합하고
작업 브랜치와 master에 push했습니다. 제품·검사 입력이 `f3556b4`와 같음을
확인했습니다. 외부 작성자의 VM 계측과 이번 직접 실행 결과를 구별합니다.

## 임시 도구와 남은 범위

릴리즈와 함께 사용한 `mazda-release-assist-20261001` 컨테이너를 제거했습니다.
그 안의 빌드 도구·D-Bus 개발 파일·QEMU·Git/cpio 등 추가·갱신 패키지 73개 행,
고정 도구체인과 펌웨어 임시 추출본도 제거됐습니다. 작업용 binfmt 등록과 호스트
QEMU 복사본은 릴리즈 설치 검사 직후 제거했고, 별도 릴리즈 checkout도 삭제했습니다.
호스트 패키지·Docker 이미지·컨테이너·binfmt 목록의 작업 전후 동일성을 확인했습니다.
기존 도구·이미지와 검증 산출물은 보존했습니다. 호스트 패키지 설치나 `sudo` 실행은
없었으며, 원본 로그·설치 목록은 비공개 evidence에 남겼습니다.

실제 센서 시각·품질·단위와 요청별 출처, qualified worker 입력·발행 연결,
차량 부하에서의 송신 지연·복구, 폰/앱 수용은 남아 있습니다. 수치 예산은 위치
정확도 보증이 아니며, 이번 기능이나 QEMU 통과를 v1.0 완료로 표시하지 않습니다.
