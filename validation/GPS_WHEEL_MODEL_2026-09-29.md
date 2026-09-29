# GPS 기준점·휠 거리 MODEL 검증 — 2026-09-29

기준 master는 `a425947e2acd3b49041cff71367385321878d639`다. 이 문서를 포함한 변경의 실행 기록이며, 이전 PR의 결과를 새 결과로 대체하지 않는다. 기능 계약은 [GPS·휠 MODEL](../docs/GPS_WHEEL_MODEL_KO.md)에 있다.

## 실행 결과

| 실행 | 결과 |
| --- | --- |
| `make test -j4` | 실행 가능한 호스트 전체 시험 통과; 아래 skip 별도 |
| GPS·휠 전용 fixture | 2,432 합성 검사 통과 |
| GPS holdout fixture | 5,386 합성 검사 통과 |
| 기존 gyro / navigation / live pipeline | 2,644 / 604 / 815 검사 통과 |
| journal Python | 41개 시험 통과; 27개 calibration/holdout, 14개 motion |
| `make arm -j4` | 지정 GCC 4.9.1/기존 sysroot로 runtime·collector·tap 빌드 통과 |
| `tests/run_arm_all.sh` | ARM/QEMU 전체 합성 fixture 통과, 실제 커널 소켓 부분은 skip |
| 실제 ARM encoder → Python parser | 두 formatter를 QEMU로 실행해 journal 41개 시험 재통과 |
| ASan + UBSan | GPS·휠 2,432, holdout 5,386 검사 통과; leak 검사 비활성 |
| `git diff --check` | 통과 |

도구체인 소스는 `lmagder/m3-toolchain`의 `61ec0343de84f6fc7c46840056df1d600d44be8a`, ARMv7 Cortex-A9/softfp이며 QEMU 8.2.2를 사용했다. ARM 전용 fixture에서 `std::nan`을 지원하지 않는 초기 컴파일 오류를 `std::numeric_limits<double>::quiet_NaN()`으로 고친 뒤 전체 실행을 완료했다. 경고 억제나 다른 ABI로 우회하지 않았다.

현재 ARM `sizeof(Pipeline)=62,944`, `sizeof(GpsHoldout)=93,768` 바이트다. 둘의 합 156,712바이트이며 worker의 다른 지역 변수·호출 스택을 포함한 총량은 아니다. 실제 CMU CPU 시간·최대 스택은 측정하지 않았다.

## 핵심 시나리오

- 정상 이동·후진 이동 방향, 큰 GPS 점프·속도/heading 불일치·정차 모호성·네 바퀴 불일치를 구분한다.
- 날짜 변경선의 작은 이동, UTC 정체/역행/큰 점프, stale/늦은 wheel·reverse 근거와 센서 단절을 검사한다.
- 정상 GPS 복귀는 과거 DR 위치에서 멀어도 새 GPS 쌍으로 재획득한다.
- 고정 환산보다 3% 긴 GPS 직진 이동에서 후보를 만들고 다음 기준점에서만 적용한다. ±5% 밖의 비율은 clamp하지 않고 거부한다.
- 회전·가속·후진·센서 단절은 학습을 끊는다. source/reset은 적용값을 지우고 정상 holdout 종료만 적용값·버전을 보존한다.
- 50ms 휠/100ms yaw와 yaw 평균 구간 내부의 GPS callback에서도 보정을 학습한다. 후보를 지지하는 실제 센서 수신보다 이른 기준점에는 적용하지 않는다.
- 두 holdout에 같은 차량 입력을 주고 RUNNING 중 GPS 좌표·속도·방향만 다르게 만든다. 예측 위치·scale·버전은 동일하다. 다음 GPS-visible cooldown에서는 2% 거리 차이를 학습해 두 번째 비교에 적용한다.
- 새 로그 필드의 누락·잘못된 범위·근거와 비교 도중 scale/버전 변경을 검출한다. 과거 필드 없는 로그도 계속 읽는다.

## 독립 리뷰와 수정

Astra 하위 에이전트가 구현과 별개로 코드를 검토하고 별도 재현 프로그램을 실행했다. 두 결함을 확인해 수정했다.

1. 정상 seed 100ms 뒤의 +1,000m GPS 점프가 1초 쌍 대기 경로로 들어가 기존 seed를 유지했다. 빠른 callback에서도 큰 변위와 의미 있는 이동 방향 불일치를 먼저 검사한다. 수정 후 바로 이어진 GPS 단절의 MODEL 위치는 invalid로 남는다.
2. 완료된 yaw 평균 구간을 짧게 나눠 적분할 때 수신 시각을 각 조각 끝과 비교해, 서로 다른 센서 주기에서 학습 근거가 계속 지워졌다. 공통 코어와 같은 완료 평균 구간 계약을 사용하고 후보의 수신 완료 시각을 따로 묶었다. 수정 후 50ms/100ms 재현에서 후보가 생성되고, 아직 수신되지 않은 근거의 조기 적용은 거부된다.

리뷰어가 수정 후 재현과 두 전용 fixture를 재실행했으며 남은 차단 결함을 보고하지 않았다. 이 기록은 Claude 리뷰나 실차 검증을 주장하지 않는다.

## Skip과 남은 한계

- packaging은 30개 중 10개 실행 통과, 원본 펌웨어 fixture가 필요한 20개 skip. OEM 파일을 공개하거나 검사를 약화하지 않았다.
- 호스트와 QEMU의 실제 Unix datagram 소켓 통합 부분은 환경의 `EPERM`으로 skip. codec/cursor 시험은 실행했다.
- CMU에서의 실제 수신 주기·지연·보정 성립률·성능, 차량 보정과 정확도, AA/touch 공존, 폰/네이버 지도 수용은 미검증이다.
- MODEL의 수신 시각·yaw 평균 구간·센서 단위 가정은 그대로다. ±5%와 나머지 임계값도 합성 시험의 제한이며 확정된 차량 보정치가 아니다.
- 위치 출력은 MODEL SHADOW다. 순정 송신을 유지하고 `allow_assist=false`, provenance 차단과 MODEL bridge 거부를 유지한다. 이 검증은 merge·release·차량 설치 승인이 아니다.

## 재실행

```sh
make test -j4
make arm -j4 ARM_PREFIX="$MX5_TOOLCHAIN/bin/arm-cortexa9_neon-linux-gnueabi-" \
  ARM_SYSROOT="$MX5_TOOLCHAIN/arm-cortexa9_neon-linux-gnueabi/sysroot"
CROSS_COMPILE="$MX5_TOOLCHAIN/bin/arm-cortexa9_neon-linux-gnueabi-" \
  QEMU_SYSROOT="$MX5_TOOLCHAIN/arm-cortexa9_neon-linux-gnueabi/sysroot" \
  sh tests/run_arm_all.sh
```

`MX5_TOOLCHAIN`은 [도구체인 안내](../docs/toolchain.md)의 검증된 설치 경로다. journal의 `MX5DR_SHADOW_FIXTURE`와 `MX5DR_MOTION_FIXTURE`에 각각 `qemu-arm -L <sysroot> build/arm-full-tests/shadow-log-test`, `.../motion-batch-test` 명령을 주면 실제 ARM formatter roundtrip을 재실행한다.
