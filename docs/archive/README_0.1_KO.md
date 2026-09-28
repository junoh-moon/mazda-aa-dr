> **Historical document / 과거 기록.** This document predates the 2026-09-28 release-blocking review. Read [current status](../STATUS_KO.md) first. It does not authorize vehicle installation. Older design choices and GO statements below may be superseded.

# MX-5 v74 Android Auto 위치 패치 — 실험판 0.1

이 묶음은 **NA 74.00.324A 전용 실제 구현**이다. 기본 모드는 자동 관측이며,
선택적으로 GPS 위치 모드가 0일 때 오래된 속도·방향 필드를 제거한다.
DR 계산기와 OEM 송신 어댑터도 구현했지만, **실차 센서의 측정 시각·품질·보정이
확인되지 않아 차량에서 DR 좌표를 송신하는 ASSIST는 활성화할 수 없다.**
네이버 지도에서 터널 관성항법이 완성됐다는 뜻이 아니다.

## 들어 있는 것

| 구성 | 구현 범위 |
| --- | --- |
| `bundle/libmx5dr.so` | ARMv7 softfp용 CMU preload 라이브러리 |
| `bundle/install.sh` | 펌웨어 확인, 기존 touch preload 보존, 서비스 한정 설치 |
| `bundle/uninstall.sh` | 이 모듈의 preload 항목 제거와 다음 시작 OFF 설정 |
| `bundle/export_logs.sh` | 주차 후 로그를 USB 등의 지정 경로로 복사 |
| `src/core/` | 속도·yaw 기반 DR, WGS84 적분, 후진·정지·시간·오차 제한 |
| `src/adapter/` | v74 상위 함수와 송신 GOT 후크, 입력·출력 복사, 단일 송신 |
| `src/runtime/` | cold-load 설치, 자동 관측, 제한된 로그, DR 연결 경계 |
| `tools/analyze_logs.py` | PC에서 로그와 패치 불변식 분석 |
| `tests/` | 합성 데이터, fake OEM, 설치·복구, 로그 실패 검증 |
| `docs/IMPLEMENTATION_REVIEW_KO.md` | 다른 리뷰어가 바로 검토할 수 있는 구현 상태·근거·남은 조건 |
| `docs/VALIDATION.md` | 실제 수행한 빌드와 검증 결과 |

`SHADOW`는 이 버전에서 OBSERVE와 같은 출력을 내며, 센서 qualification 미완료를
기록하는 예약 모드다. 실차 DR 예측을 실행했다는 의미가 아니다. `ASSIST`는
설치 스크립트와 설정 파서 양쪽에서 거부된다.

## 설치와 기록

기존 touch 패치를 설치할 때 사용한 **접근 가능한 CMU root 셸**에서 실행한다.
USB에 복사하는 것만으로 자동 설치되는 묶음은 아니다. 명령은 모두 주차 중에
실행한다. 운전 중 명령 입력이나 CMU 조작은 필요 없다.

1. `bundle` 전체를 실제 마운트 경로에 복사한다. 그 디렉터리에서 기본 관측 설치:

   ```sh
   sh install.sh
   ```

   읽기 전용 마운트로 거부되면 같은 주차 상태에서 `sh install.sh --remount`로
   실행할 수 있다. 설치기는 자신이 바꾼 마운트만 종료 때 읽기 전용으로 돌린다.

2. 다음 정상적인 CMU 서비스 시작부터 자동 기록한다. 설치기가 재시작·재부팅을
   실행하지 않는다. 라이브러리는 내부 저장소로 복사되므로 USB를 빼고, 하나뿐인
   정상 USB 포트를 Android Auto에 사용할 수 있다.

3. 주차 후 USB를 연결하고 실제 마운트 경로를 지정해 로그를 내보낸다:

   ```sh
   sh /data_persist/mx5-aa-dr/tools/export_logs.sh /mnt/sdb1
   ```

   `/mnt/sdb1`은 예시다. 출력된 `.tar` 파일을 PC로 가져온다.

4. PC에서 분석한다:

   ```sh
   python3 tools/analyze_logs.py /path/to/mx5dr-logs.tar --json
   ```

   분석기는 CMU 입력과 송신 함수 경계의 일치 여부를 확인한다. 폰이 메시지를
   받아들였는지, 네이버 지도가 위치를 사용했는지는 이 로그만으로 증명하지 않는다.
   앱의 터널 내 동작은 동승자 기록 또는 주행 전에 시작한 화면 기록과 비교한다.

첫 관측이 정상일 때 다음 주차 상태에서 `sh install.sh --mode=SCRUB`으로
비교 실험을 구성할 수 있다. 읽기 전용 마운트에서는 `--remount`도 붙인다.
SCRUB은 좌표를 계산하지 않으며, 남아 있는 캐시 좌표나 wire timestamp도 바꾸지 않는다.

기록은 기본 최대 **8 MiB × 3개**로 순환한다. 로그에는 위치 이력이 포함되고
자동 업로드는 없다. 설정은 서비스 시작 때 읽으므로 실행 중 설정 파일 변경이
즉시 반영된다고 간주하면 안 된다.

## 되돌리기

```sh
sh /data_persist/mx5-aa-dr/tools/uninstall.sh
```

필요하면 `--remount`를 붙인다. 다음 정상 시작부터 이 모듈이 로드되지 않도록
자기 항목만 제거한다. 기존 touch 패치와 이후의 다른 설정 변경은 보존한다.
이미 로드된 코드를 실행 중 강제로 제거하지 않는다. 로그·백업·라이브러리 파일도
보존한다. 더 자세한 중단 복구 절차는 `docs/install_notes.md`에 있다.

이 패키지는 아직 CMU에서 실행하지 않았다. 호스트와 ARM 에뮬레이터 검증은 실제
서비스 시작, 기존 패치와의 공존, Galaxy S25·무선 동글·지도 앱의 수용을 대신하지
않는다. 펌웨어 해시를 고쳐 다른 버전에 억지로 설치해서는 안 된다.

## 개발자 빌드

호스트: C/C++ 컴파일러, make, Python 3, libdbus 개발 파일과 pkg-config가 필요하다.

```sh
make test
```

펌웨어 identity를 사용하는 설치 fixture는 제공된 원본 펌웨어 추출물이 있어야
전부 실행된다. 배포 ZIP에는 OEM 바이너리를 재배포하지 않는다. fixture가 없으면
해당 검사를 건너뛰었다고 표시하며, 통과로 간주하지 않는다.

ARM: `docs/VALIDATION.md`에 기록된 toolchain과 sysroot를 지정한다.

```sh
make arm ARM_PREFIX=/absolute/toolchain/bin/arm-cortexa9_neon-linux-gnueabi- ARM_SYSROOT=/absolute/toolchain/arm-cortexa9_neon-linux-gnueabi/sysroot
sh packaging/make_bundle.sh build/libmx5dr.so new-bundle
```

PC용 `build/replay`는 `tests/core/synthetic_straight.csv`를 재생할 수 있다.
CSV의 VALID와 보정 완료 값은 **합성 시험 가정**이며 실차 센서의 증거로 사용할 수 없다.
