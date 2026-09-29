# PC 원본 로그 항법 재생 검증 — 2026-09-29

기준 master는 `a319166dfe1b4a175d713291447947113506f2e4`다. 이 기록은 그 위의 원본 로그 재생·고정/자동 보정 비교·센서 수신 요약 변경을 검증한다. 과거 시험 결과를 소급 수정하지 않는다.

## 변경과 근거

- `tools/replay_navigation.cpp`가 실제 `Pipeline`, `GpsHoldout`, 공통 C 코어를 fixed/adaptive 두 설정으로 실행한다. mode=0 주 항법 계산과 GPS 제외 비교를 모두 수행한다.
- fixed도 같은 GPS 기준점 검사를 유지한다. 추가한 내부 인자는 학습만 끄며 기존 차량 호출의 기본값은 유지한다. qualified 경로, 설치 모드, AA 송신, ASSIST 자격을 바꾸지 않는다.
- Python 도구는 기존 로그 감사·압축 안전 검사를 재사용하고 원본 정수/필드를 보존한다. 입력과 자식 프로세스 시간·출력을 제한한다. 소스 측정 시각을 새로 증명하지 않는다.
- 비교는 기준점/참조 시각이 일치하고 양쪽 모두 END에 도달한 구간만 포함한다. ABORT·미완료·짝 없는 참조와 시작 전 ABORT를 별도로 집계한다.
- 센서 요약은 원본 수신 시각의 관측 간격이다. 새 합격 기준이나 생산 지연 측정이 아니다.

## 실행 결과

| 검사 | 결과 |
| --- | --- |
| `make test -j4` | exit 0. Python 총 152개 중 132 통과, 설치 시험 20 skip |
| 새 C++ 실행기 시험 | 8개 통과. 실제 mode=0 계산/정차, 영점·휠 보정, 참조 GPS 격리, 끝 시각 제한, uint64/잘못된 입력/오류 보고 |
| Python 재생 도구 | 12개 통과. 실제 실행기로 보정 학습과 동일 참조 비교, 불완전/ABORT 제외, 로그 감사, 압축 안전, 크기·시간 제한, 실행기 출력 검사 |
| 새 센서 수신 요약 | 8개 통과. 세션/epoch 경계, 중복/역행, uint64 정밀도, 30,000 이벤트에서도 고정 상태 크기 |
| 마지막 Python 변경 후 `test-tools` 재실행 | 기존 20 + 신규 12 = 32개 통과 |
| 고정 GCC 4.9.1 ARM 빌드 | 4개 산출물 빌드 exit 0 |
| `tests/run_arm_all.sh` | exit 0. 실제 소켓 항목은 아래 skip 참조 |
| 독립 Astra 소스 리뷰 | 기본 make 대상, 시작 전 ABORT 집계, Python 3.11 전용 해시 API 문제를 발견하고 수정 확인. 남은 차단 의견 없음 |

호스트와 ARM/QEMU에서 core 1,425, navigation 604, live pipeline 815, gyro 2,644, GPS/wheel 2,832, holdout 5,386, runtime 38개 검사를 통과했다. 기존 adapter 11개 시나리오와 ARM veneer 검사도 통과했다. 새 PC 실행기와 Python 도구는 호스트에서 시험했다.

GPS/wheel 검사는 fixed/adaptive 양쪽에서 동일한 거부 조건이 동작함을 확인한다. 55초 합성 로그에서는 adaptive가 1.02 휠 scale을 학습하고 fixed는 1/version 0을 유지한다. 완료된 동일 참조 구간이 둘 이상 생성된다. 별도 C++ 합성 입력은 정차 영점 변화와 실제 mode=0 적분을 검사한다. 합성 결과는 실차 보정값이 아니다.

## 건너뛴 검사와 남은 확인

- 대상 NA 74.00.324A 비공개 원본 identity fixture가 없어 설치 시험 20개를 건너뛰었다. 실제 release payload 설치 통합 시험도 포함된다. 펌웨어 검사를 완화하거나 원본을 공개하지 않았다.
- 호스트/ARM의 커널 Unix datagram 통합은 환경의 `EPERM` 때문에 exit 77로 skip했다. codec/cursor 검사는 통과했다. 이 환경에서 실제 프로세스 간 통신을 확인했다고 주장하지 않는다.
- Python 3.8에서 직접 실행하지 않았다. 3.11 전용 `hashlib.file_digest` 대신 분할 읽기 SHA-256을 사용했으며 현재 호스트 Python에서 시험했다.
- 실제 CMU 부팅·touch 공존·회복·센서 주기/부호/후진·폰/네이버 지도 수용은 미검증이다. ASSIST는 계속 차단한다.
- 콜백 수신 순서와 100ms 대기 규칙으로 재계산한다. 기록되지 않은 worker drain 시각을 복원하지 않는다. 완료 구간의 선택 편향과 GPS 자체 오차가 있어 비교 숫자를 정확도/자동 합격으로 해석할 수 없다.

## 별도 master 설치 후보

이번 PC 도구 PR과 별도로 기준 master `a319166dfe1b4a175d713291447947113506f2e4`의 깨끗한 checkout에서 설치 후보를 준비했다. 새 PR 코드는 그 ZIP에 포함되지 않는다.

- 파일명: `mazda-aa-dr-shadow-candidate-a319166.zip`
- 크기: 1,012,787 bytes
- SHA-256: `5cf1596cfcccbedf9fb6dda990d325d443792188bc3a9edeea2a7a1fcf8885af`
- 고정 ARM 도구체인의 4개 바이너리, 한국어 설치·검증 기록, build-info, ELF 검사, manifest 포함. 압축 구조와 새 디렉터리에 풀어 계산한 manifest를 확인했다.
- ARM32 little-endian/softfp, TEXTREL 없음, GLIBC_2.4만 요구, 동적 libstdc++ 없음. D-Bus 의존성은 collector에만 있다.
- 후보 자체의 호스트·ARM/QEMU 시험도 exit 0. Python 104 통과/20 skip. 위와 같은 설치 fixture·소켓 환경 제한이 남는다.
- 기본 OBSERVE, 기존 명시적 SHADOW 선택 방식 유지. 설치 검증 미완료·실차 미검증 후보이며 태그나 릴리즈를 게시하지 않았다.
