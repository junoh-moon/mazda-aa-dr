# VIP 누적값 MODEL 거부 통합·직접 검사 — 2026-10-01

이 문서는 외부 master `7b5ceb6`의 실행·검토 기록을 보존한 것입니다.
본문의 직접 실행·root는 그 기록의 작성자를 가리킵니다. 현재 작업의
직접 검증이나 최종 설치 후보의 시험 횟수로 합산하지 않습니다.

기존 MODEL은 요레이트 `합계 / count`만 검사하여, VIP의 16비트 합계가
넘친 값을 작은 정상 평균으로 받아들일 수 있었습니다. 예를 들어 작성한
`4093` 표본 17개의 합은 16비트에서 `4045`가 되고 기존 코드는 이를
`237`로 해석했습니다. 원본 생산자 경로의 제한된 명령어 해석과 8비트
count 계약은 외부 `feat/session-observation`의
[조사 기록](VIP_ACCUMULATOR_2026-10-01.md)에 출처를 유지했습니다.
root가 원본 MCU 전체나 실제 CAN을 실행한 결과로 합산하지 않습니다.

## root가 직접 대조한 원본 입력과 변경

제공 ZIP의 크기 1,005,170,715바이트·SHA-256
`20b7089f37652e095225486dea696d5f7264f57d47fa6667ad94191f27bbe84e`를
다시 확인했습니다. 그 안의 VIP S-record는 589,168바이트·SHA-256
`a166ceec24cc4e23e3dda715a8024474966431506f38c6ebb8800e71f7c11767`로
외부 조사 입력과 일치했습니다. S2 7,557건의 길이·체크섬, S5 선언 개수
7,557과 기록 주소 중복 0을 직접 검사했습니다. 이는 입력 동일성 대조이며
root의 VIP 명령어 해석 실행은 아닙니다.

`Pipeline::enqueue_raw`의 MODEL 요레이트 경로에서 보고된 count가
1–255가 아니거나 `합계 + 65536 <= count × 4095`이면 계산을
`PIPELINE_BAD_INPUT`으로 초기화합니다. 오른쪽은 보고된 개수의 12비트
표본이 만들 수 있는 최대 합계입니다. 이 조건이면 관측 합계와 한 번
넘친 합계를 구별할 수 없으므로 원값을 추정해 복원하지 않습니다.
경계 밖의 정상 합계는 계속 수락하고, 원시 이벤트 객체를 바꾸지 않으며
새 기준점을 요구합니다. count 자체의 넘침, `4095` 등의 구성 표본,
손실된 창과 생산 시각·물리 품질은 이 검사로 구별할 수 없습니다.
MODEL 결과를 qualified ASSIST로 승격하지 않았습니다.

## 실패 재현과 직접 검증

현재 master에 먼저 추가한 회귀의 첫 합계 넘침 조건은 수정 전
x86_64 Linux navigation 바이너리에서 `PIPELINE_BAD_INPUT` 기대에
실패했습니다. 수정 후 같은 테스트는 합계 넘침·경계값·8비트 범위 밖
count의 여섯 음성 조건을 수신/transport 시각 양쪽에서 통과했고,
MODEL 유효성 철회·generation 증가·새 기준점 요구도 확인했습니다.
`make test-navigation`은 navigation 2,740개, live pipeline 815개,
gyro bias 2,677개, GPS/wheel 84,601개, holdout 5,443개와 motion
channel 검사를 통과했습니다.

Python 3.11·x86_64 Debian 검증 컨테이너의 전체 `make test`가 종료 코드
0으로 끝났습니다. 기본 환경에서 원본 identity와 현재 다섯 ARM 산출물을
요구하는 packaging 항목은 건너뛰어졌습니다. 원본 rootfs와 이번에 빌드한
ARM 산출물 경로를 명시해 packaging 20개를 별도로 다시 실행했고 모두
통과했습니다. 최초 macOS 직접 시도는 Linux 소켓 API가 없어 컴파일되지
않았고, 처음 고른 x86 컨테이너의 Python 3.9는 `TestCase.enterContext`
부재로 build 검사를 시작하지 못했습니다. 지원되는 Python 3.11 환경에서
재실행한 결과로 이 실패를 대체하며, 초기 실패 기록을 숨기지 않습니다.

고정 GCC 4.9.1·commit `61ec0343de84f6fc7c46840056df1d600d44be8a`로
다섯 ARM 산출물을 새 디렉터리에 빌드하고 빌드 출처·ELF/ABI 검사를
통과했습니다. `tests/run_arm_all.sh`도 QEMU user에서 종료 코드 0이었고
ARM navigation 2,740개와 나머지 core·adapter·runtime·센서·MODEL
검사를 완료했습니다. 최종 `libmx5dr.so` SHA-256은
`b80f7eb9808036fb0b4e60336b506a78aeccc52aeef13afba84833edee474c9f`로,
외부 브랜치가 같은 동작의 코드에서 보고한 바이너리와 일치합니다.
현재 master의 `pipeline.cpp` SHA-256은
`2bd93e782797693bd957fe7c5a805d3985a797ddbf5304b7658a5b289e6dd4b0`입니다.
주석 문구가 달라 외부 보고서의 소스 해시를 현재 소스 해시로 옮기지
않습니다.

이번 수정 후 원본 OEM LDS·AA VM, 실제 VIP MCU/CAN, 차량·폰·지도 앱은
실행하지 않았습니다. QEMU user와 합성 입력의 수치·상태 검사는 차량의
요레이트 부호·단위·주기 또는 주행 위치 정확도를 검증하지 않습니다.
제품 live ASSIST는 계속 비활성이고 공개 ZIP도 갱신하지 않았습니다.
