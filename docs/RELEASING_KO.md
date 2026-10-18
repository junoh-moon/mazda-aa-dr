# 릴리즈 생성 절차

이 문서는 유지보수자와 자동화 에이전트가 **검증한 커밋으로 설치 ZIP을 만들고 GitHub Release에 게시**하는 절차입니다. 2026-10-18부터 절차의 각 단계는 저장소에 추적된 스크립트 `tools/release/*.sh`로 실행하고, 안정판 게이트(G7~G11, G16)는 `tests/gates/`의 스크립트로 재현합니다(설명: [게이트 시험 안내](../tests/gates/README_KO.md), 첫 실행 기록: `validation/V1_C_GATES_2026-10-18.md`). 게이트의 정의와 통과 기준은 `validation/V1_STABLE_DECISIONS_2026-10-18.md`와 `validation/V1_STABLE_AUDIT_2026-10-18.md` 4절이 정합니다.

릴리즈 발행을 추가 차량 방문·주행 승인으로 해석하지 마십시오. 현재 기능·실차 범위는 [현재 상태](STATUS_KO.md)가 정합니다.

## 배포 단위와 적용 범위

- 배포 단위는 **태그 + 해당 커밋에서 만든 설치 ZIP + SHA-256 + 릴리즈 노트**입니다. master 병합만으로 기존 릴리즈 파일이 바뀌지 않습니다. GitHub가 자동 생성하는 `Source code (zip)`은 설치 묶음이 아닙니다. **소스 병합만으로 배포가 끝난 것이 아닙니다.**
- 리뷰, Ready 전환, LGTM, 병합, 릴리즈 발행은 각각 다른 작업입니다. 요청받은 범위만 수행합니다. 이미 릴리즈 발행을 지시받았다면 같은 승인을 반복해서 묻지 않습니다. `tools/release/all.sh`는 초안·발행을 하지 않으며, 발행(`publish.sh`)은 `REL_CONFIRM_PUBLISH=<태그>`를 명시해야만 실행됩니다.
- 공개 묶음은 **BETA, 상시 실행(persistent)** 입니다: `tools/make_usb_zip.py --default-mode BETA`, build-info `default_mode=BETA`, `install_policy=persistent`, `bundle-default-mode`·설치 안내·릴리즈 노트가 모두 BETA로 일치해야 합니다. live ASSIST(검증 도메인)는 코드에서 차단된 채로 둡니다. 릴리즈 과정에서 설정이나 차단 조건을 바꾸지 않습니다. OBSERVE·SHADOW 묶음은 소유자가 명시적으로 요청할 때만 만들고 같은 일치 규칙을 적용합니다.
- **설치 ZIP은 그 자체로 완결적이어야 합니다(2026-10-04, 소유자 지시).** 새 공개 묶음에는 `mp3/`(4개)와 `js/run.js`의 셸 진입 파일과 `USB_ENTRY_NOTICE.md`를 **항상 포함합니다**(`make_usb_zip.py`를 `--shell-only` 없이 실행). 진입 파일이 빠진 셸 전용 ZIP(`v0.3.11-shadow.2`, `v0.3.12-shadow.1`·`.2`)은 이 기준을 충족하지 못했습니다. `--shell-only`는 소유자가 명시적으로 요청할 때만 씁니다. 진입 파일의 출처와 라이선스 상태는 `USB_ENTRY_NOTICE.md`에 있는 그대로 보존합니다. `tests/gates/g5g6_zipcheck.py`가 이를 검사합니다.
- **커밋을 고정합니다.** 분리된 새 체크아웃에서 40자리 커밋으로 빌드하고 `source_modified=false`여야 합니다. 한 작업 디렉터리에서 다른 커밋으로 전환하거나 빌드 결과를 재사용하지 않습니다.
- **게시된 ZIP을 다시 받아 검증합니다**(`verify.sh`, G18). 다운로드 파일이 빌드한 ZIP과 바이트 동일하지 않으면 완료로 보고하지 않습니다.
- 버전 이름: 태그에 `-`가 붙으면(`-beta.N`, `-rc.N`) pre-release로, 붙지 않은 `vX.Y.Z`는 정식(latest)으로 게시합니다(`REL_PRERELEASE`로 바꿀 수 있음). 실차 미검증 기능은 정식 릴리즈에서도 노트의 검증 상태 절에 그대로 적습니다(결정 기록의 라벨 규칙). 빌드·호스트·QEMU 성공을 실제 차량 복구, 위치 정확도, 폰/지도 앱 수용 검증으로 표현하지 않습니다.
- 예전 태그를 재현할 때는 그 태그의 Makefile·packaging·스크립트를 사용하며 현재 스크립트나 바이너리를 섞지 않습니다.

## 0. 준비물과 환경 변수

x86-64 Linux 기준입니다(고정 도구체인 컴파일러가 Linux x86-64 실행 파일). 필요 도구: Git, Python 3, make, C/C++ 컴파일러, pkg-config, D-Bus 개발 파일, qemu-user(qemu-arm), PRoot, zip/unzip, file, jq, sha256sum, GitHub CLI(`gh`, 기존 승인된 계정으로 인증). Debian/Ubuntu: `build-essential python3 pkg-config libdbus-1-dev qemu-user proot zip unzip file jq git`.

저장소 밖에 두는 비공개 입력(경로는 환경 변수로만 전달하며 저장소·공개 기록에 넣지 않습니다):

| 변수 | 내용 | 쓰는 단계 |
|---|---|---|
| `MX5DR_STOCK_ROOT` | 순정 펌웨어 트리(`packaging/firmware.sha256`의 파일, `jci/version.ini`, `jci/sm/sm.conf`, `jci/sm/sm_WCP.conf`, `usr/bin/autostart`, 원본 runtime). **읽기 전용** | arm, host, pack, stock |
| `MX5DR_LDS_STOCK`, `MX5DR_AA_STOCK` | 원본 LDS runtime, AA 프로브용 순정 루트 | arm (기본: `MX5DR_STOCK_ROOT`) |
| `MX5DR_LIBPATCH`, `MX5DR_LIBPATCH_091` | 공개된 `libpatch-blmjciaapa.so` 0.10.0과 0.9.1(차량 버전) | arm (G4) |
| `MX5DR_LIBPATCH_DIR` | 공개된 oem-aa-mod 폴더(0.9.1) | stock |
| `MX5DR_VEHICLE_EXPORT` | 소유자 차량 내보내기 트리 | stock (터치, 메뉴 6 복제) |
| `MX5DR_REPLAY_CHAIN` | 비공개 부팅 목록 | replay (G11) |
| `MX5DR_PROOT`, `QEMU_ARM` | PRoot, qemu-arm | arm, pack, stock, soak (기본: PATH) |
| `MX5_TOOLCHAIN` | 이미 받은 고정 도구체인(복사해서 씀) | build (없으면 `tools/fetch_m3_toolchain.py`) |
| `HOST_DBUS_FLAGS`, `HOST_DBUS_LIBS` | 호스트 D-Bus 헤더·라이브러리 | host (기본: pkg-config) |
| `REL_PREVIOUS_TAG`, `REL_PREVIOUS_SHA256` | 직전 공개 ZIP(G8, G16) | stock |
| `G11_BASE_COMMIT` | 직전 릴리즈 태그의 커밋 | replay |
| `REL_MAX_LOAD`(4), `REL_NICE`(10) | 무거운 단계는 1분 부하가 이 값 미만일 때 시작, nice 값 | 전부 |

## 1. 고정과 전체 실행

```bash
set -euo pipefail
TAG='vX.Y.Z'                               # 새 태그(이미 있으면 setup이 거부)
COMMIT='<40자리 커밋 SHA, origin/master 위>'
sh tools/release/setup.sh "$TAG" "$COMMIT"  # G0: 새 클론, 분리 체크아웃, 깨끗한 트리, 태그·릴리즈 없음
nohup sh tools/release/all.sh > /dev/null 2>&1 &   # build → zip → arm → host → pack → stock → soak → replay
sh tools/release/status.sh                  # 단계별 exit, 증거 목록
```

`setup.sh`는 `WORK=$(mktemp -d ${REL_TMP:-/tmp}/mazda-aa-dr-release.XXXXXX)`를 만들고 `WORK/release.env`(TAG, COMMIT, REPO, NAME)와 포인터 파일(`REL_POINTER`)을 씁니다. 이후 스크립트는 첫 인수, `REL_WORK`, 포인터 순으로 WORK를 찾습니다. 단계마다 `WORK/jobs/<단계>.log`와 `.exit`이 남고, 증거 파일은 `WORK/evidence/`와 해시 목록 `WORK/evidence/INDEX`에 모입니다. 실패하면 원인을 고친 뒤 **해당 단계만** 다시 실행합니다(`sh tools/release/arm.sh` 등). 소스나 버전을 바꿨다면 setup부터 새 WORK로 다시 합니다.

`all.sh`는 오래 걸리므로(전원 차단 G9 전체와 4 h soak 포함 시 하루 가까이) 추적되는 백그라운드 작업으로 띄우고 `status.sh`로 봅니다. 프로세스를 남기지 않도록 끝난 뒤 `WORK/jobs/all.exit`을 확인하십시오.

## 2. 단계와 게이트

| 단계 스크립트 | 게이트 | 하는 일 · 실패 조건 |
|---|---|---|
| `setup.sh TAG COMMIT` | G0 | 태그 형식, 40자리 커밋, origin/master 조상, 깨끗한 트리, 태그·같은 이름 릴리즈 없음 |
| `build.sh` | G1 | 고정 도구체인(커밋 `61ec0343de84f6fc7c46840056df1d600d44be8a`, GCC 4.9.1, ARMv7 Cortex-A9 NEON **softfp**)으로 `tools/build_arm.py`; 새 빌드 디렉터리만, 여섯 산출물 해시와 `arm-build.json` |
| `zip.sh` | G5, G6 | `make_usb_zip.py --default-mode BETA`(`--shell-only` 없음), `unzip -t`, `SHA256SUMS`, `g5g6_zipcheck.py`, `elf.txt` |
| `arm.sh` | G3, G4 | `tests/run_arm_all.sh`(LDS 순정·AA 순정·libpatch 0.10.0) + AA 설치 프로브 libpatch 0.9.1; 종료 0, `ARM_TIMING_FAILED=0`, 프로브 SKIP 없음 |
| `host.sh` | G2 | `make -k test`(순정 루트·릴리즈 묶음 지정); 종료 0, unittest 합계·SKIP·`WORKER_BETA_TIMING_RETRY` 기록 |
| `pack.sh` | G2(패키징) | `tests/packaging` 전체; 실패·SKIP 없음(`REL_ALLOW_PACK_SKIP=1`이 아니면), 원본 `bds` 파싱 두 줄 |
| `stock.sh` | G7, G8, G9, G16 | 순정 BusyBox 하네스(`g7_cmu_emulation`, `g7_multiboot`, `g7_menus`, `g7_touch` A/B, 메뉴 6 복제), 업그레이드(`g8_upgrade`, 직전 공개 ZIP을 gh로 받아 sha256 확인), 롤백(`g16_rollback`), 전원 차단(`g9_powerloss`, `REL_G9_STRIDE` 기본 1 = 모든 차단점) |
| `soak.sh` | G10 | `g10_soak.sh`: QEMU-ARM 4 h 터널 교체 반복 + 100 짧은 부팅(`REL_SOAK=fast`는 연기 시험일 뿐 게이트가 아님) |
| `replay.sh` | G11 | `g11_replay.sh`: 비공개 전 부팅, 실험 끔, 직전 릴리즈와 바이트 동일(`G11: X/N`) |
| `draft.sh` | — | 노트·ZIP·체크섬으로 **초안**(태그는 만들지 않음, 같은 태그 릴리즈가 있으면 거부) |
| `publish.sh` | 발행 | `REL_CONFIRM_PUBLISH=<태그>` 필수, 앞 단계 모두 exit 0, 초안 정확히 1개; 주석 태그 생성·push, 초안 공개 |
| `verify.sh` | G18 | 게시 파일 재다운로드: sha256, 빌드 ZIP과 `cmp`, `unzip -t`, 내부 `SHA256SUMS`, build-info 커밋, 태그→커밋, 공개·pre-release/latest 상태, ZIP 검사 |

G13(열린 CONFIRMED 결함 0), G14(독립 검토 2라운드), G15(문서)는 사람이 판단하는 게이트이므로 스크립트가 없습니다. 결과는 새 날짜의 릴리즈 기록 한 건에 모읍니다.

### SKIP과 재시도 규칙

- 어느 게이트든 SKIP(종료 77)은 통과가 아닙니다. 비공개 입력이 없는 기계에서 일부를 생략해야 하면 `REL_ALLOW_SKIP="g7-touch-A g7-touch-B g7-menu6-replica"`처럼 이름을 적고 릴리즈 기록에 생략과 이유를 적습니다.
- `make test` 종료 코드만으로 판정하지 않습니다. fixture 누락에 따른 packaging skip과 소켓 금지 환경의 exit 77은 전체 명령에서 성공처럼 보일 수 있습니다. 실제 PASS/FAIL/SKIP과 이유를 기록합니다.
- **시간 계열 ARM 시험과 flaky 보고**: 벽시계로 제품 시간 계약(기록 스레드 250 ms 자체 flush·50 ms 이하 잔여, 1.5 s 지연 가드, 워커 턴·idle 게이트 기한)을 단언하는 시험만 `tests/run_arm_timing.sh`로 최대 3회 실행합니다. 한 번이라도 통과하면 통과, 세 번 모두 실패하면 스위트 실패입니다. 임계값은 바꾸지 않으며 다른 ARM 시험은 재시도하지 않습니다. 로그 끝의 `== ARM timing-class flaky report` 블록과 `ARM_TIMING_RETRIES=n`, `ARM_TIMING_FAILED=n`을 확인하고, 릴리즈 노트에는 `ARM_TIMING_RETRIES` 값을 항상 적고 **1 이상이면** 재시도한 시험 이름·첫 실패 단언·부하를 함께 적습니다. 같은 시험이 연속 릴리즈에서 재시도되거나 부하 약 2 이하에서 재시도되면 실제 경쟁 조건일 수 있으므로 릴리즈 전에 원인을 조사합니다. 4코어에서 nice 0 작업이 코어를 모두 점유한 채 nice 10으로 돌리면 재시도해도 결정적으로 실패합니다(`journal_lag_bound` 미회복, `worker-session-test` 20 s alarm). 이때는 임계값을 바꾸지 말고 호스트가 조용할 때 다시 실행합니다. 근거: [ARM 시간 시험 안정화 기록](../validation/ARM_TIMING_STABILISATION_2026-10-10.md). 호스트 `test_worker_beta`의 같은 부류(`g3_garage`, `g3_cancel`)는 `tests/runtime/run_worker_beta_timing.sh`가 같은 규칙으로 재시도합니다.
- G10 soak의 짧은 부팅에서 종료 77(시간 단언 불확정)은 실패로 셉니다. 부하가 높았던 실행은 부하 기록과 함께 다시 실행합니다.

### 단계별 확인 사항

- **ARM 입력 고정**: ARM 로그 처음과 끝의 `ARM_TEST_INPUTS`에서 `release_verified=true`, 여섯 artifact 해시, 도구체인과 sysroot를 확인합니다. 릴리즈 검사는 `MX5DR_ARM_BUILD`가 필수입니다. 개발용 `make arm`이나 `MX5DR_ARM_LIBRARY` 검사는 릴리즈 검증으로 표시하지 않습니다. ARM runner는 명시한 compiler/sysroot/preload만 사용합니다.
- **ELF**: ELF32 little-endian ARM, softfp, TEXTREL 없음, GLIBC_2.4만, 동적 libstdc++ 없음. D-Bus는 collector에만 필요하며 AA preload로 돌아가면 안 됩니다. 과거 `elf.txt`를 새 바이너리의 결과로 재사용하지 않습니다.
- **AA 설치 프로브(G4)**: 실제 BLM에 제품 preload와 서드파티 `libpatch`를 차량과 같은 순서로 올려 (1) 패치 없음: 설치 ok와 세션 관측, (2) 알려진 경로의 패치: 위치 후킹 설치와 세션 관측만 생략, (3) 알려지지 않은 경로: 안전 실패를 확인합니다. 0.10.0과 0.9.1 두 버전 모두 4/4여야 합니다. 순정 런처 대신 프로브를 쓰므로 차량 검증이 아닙니다.
- **원본 `bds` 파싱 검사(휴대폰 IMU 채널)**: `pack.sh`가 돌리는 `tests/packaging/test_bds_parse_qemu.py`의 6개 시험이 모두 실행·통과해야 하며 SKIP은 통과가 아닙니다. `bds parse check: radio_open count=8 ids=5,8,9,10,11,12,14,15; exit 1`과 `bds parse check (wcp): …` 줄을 릴리즈 기록에 옮깁니다. 라디오·SDP·RFCOMM·페어링은 이 검사로 확인되지 않습니다. 근거: [휴대폰 IMU 채널 기록](../validation/PHONE_IMU_CHANNEL_2026-10-14.md).
- **순정 BusyBox 하네스의 한계**: 순정 BusyBox를 사용했다는 사실만으로 파일시스템·mount·전체 기동까지 실제라고 판단하지 않습니다. `/proc/mounts -> self/mounts`처럼 실제 파일 형태를 보존하고, 실 Linux procfs의 직접 확인과 작성한 모델의 결과를 구분합니다(v0.3.9-shadow.1은 이 전제를 놓쳐 실차 회수가 실패했습니다). 메뉴 2의 런타임 증거 줄은 복제에서 'wait'일 수밖에 없으므로 게이트는 "NO 줄 없음"을 봅니다.
- **G8 플래그**: 새 설치기의 실험 표식·trace 정리 규칙이 들어간 후보는 `REL_GATE_FLAGS='--exp-markers present --trace-cleanup yes'`로 실행합니다(기본은 skip, `tests/gates/README_KO.md`).
- 원본 펌웨어, 개인 경로, 실차 위치 로그를 공개하지 않습니다. 실행 로그는 비공개 WORK에 두고, 공개 검증 요약에는 실행 환경·커밋·생략·미검증만 적습니다. `replay.sh`는 개수와 파일 이름만 evidence로 옮깁니다.
- SHA-256은 손상/동일성 검사이며 서명이 아닙니다. ZIP에 원본 펌웨어, 위치 로그, 개인 공유 링크가 없음을 확인합니다(진입 파일은 의도적으로 포함). `INSTALL_KO.md`는 `packaging/SHELL_START_KO.md`와 일치해야 합니다. 묶음 내 파일을 고치면 다시 빌드합니다.

## 3. 릴리즈 노트

`WORK/release-notes.md`를 작성합니다. 최소 내용:

- 대상 펌웨어(NA 74.00.324A), 배포 커밋의 전체 SHA, 직전 릴리즈 대비 변경과 포함 커밋/PR.
- 기본 모드(BETA, persistent), 기본 켬·기본 끔(실험) 기능과 끄는 메뉴, ASSIST 차단, **검증 상태 절**(기능 × 라벨 × 근거 기록; 라벨은 결정 기록의 규칙).
- 게이트별 결과(G0~G18): 실행한 검사와 생략한 검사·이유, `ARM_TIMING_RETRIES`, G9 차단점 수, G10 수치, `G11: X/N`. 과거 실행 결과를 이번 결과처럼 쓰지 않습니다.
- 다운로드할 정확한 ZIP 이름·SHA-256, 주차 중 메뉴 `1` 설치 → `5`의 CMU 재부팅 → `2` 확인, 회수(메뉴 3)·제거(메뉴 4)·롤백 안내. 차량 점화 OFF/ON을 CMU Linux 재부팅으로 대신 설명하지 않습니다.

## 4. 초안, 발행, 재다운로드 검증

```bash
REL_TITLE="$TAG — ..." sh tools/release/draft.sh      # 초안(태그 없음)
gh release view "$TAG" --repo junoh-moon/mazda-aa-dr  # 커밋·노트·첨부 이름·크기 확인
REL_CONFIRM_PUBLISH="$TAG" sh tools/release/publish.sh  # 발행이 요청된 경우에만
sh tools/release/verify.sh                             # G18
```

초안 작성만 요청받았다면 `draft.sh`에서 멈춥니다. `publish.sh`는 태그를 고정 커밋에 만들고 push한 뒤 기존 초안을 공개로 바꿉니다. 두 번째 릴리즈를 만들지 않으며(beta.11에서 중복 초안이 생긴 적 있음) 태그를 강제 갱신하지 않습니다. `gh`를 쓸 수 없으면 GitHub UI/API로 **같은 태그·커밋, pre-release/latest 설정, ZIP·체크섬 첨부**를 적용합니다.

`verify.sh`가 통과한 뒤 **릴리즈 URL·태그·소스 커밋·ZIP 해시·검증 범위**를 보고합니다. SHA/태그/파일이 다르거나 다운로드가 실패하면 완료로 보고하지 않습니다. Git에 옮길 검증 기록은 새 날짜의 파일로 남기고 이전 결과를 덮어쓰지 않습니다.

이미 게시한 태그를 다른 커밋으로 옮기거나 같은 이름의 ZIP을 조용히 교체하지 않습니다. 코드나 첨부 파일 수정은 새 버전으로 발행하고 이전 버전의 알려진 문제를 설명합니다. 차량에 설치한 내용을 제거하는 절차(메뉴 4, 메뉴 6)는 GitHub 릴리즈 삭제와 별개입니다.

## 수동 절차와의 관계

스크립트는 이 문서의 이전 판에 있던 명령 블록(클론·고정, `build_arm.py`, `make test`, `run_arm_all.sh`, `make_usb_zip.py`, `gh release create/edit/download`)을 그대로 감싼 것입니다. 스크립트를 쓸 수 없는 환경에서는 `tools/release/*.sh`의 본문을 같은 순서로 실행하되, 같은 실패 조건(위 표)을 지키십시오.

## 참고

- [게이트 시험 안내](../tests/gates/README_KO.md), [고정 도구체인](toolchain.md), [기여·테스트](../CONTRIBUTING.md)
- GitHub CLI 공식 문서: [create](https://cli.github.com/manual/gh_release_create), [edit](https://cli.github.com/manual/gh_release_edit), [download](https://cli.github.com/manual/gh_release_download)
- 자동 릴리즈 GitHub Actions는 없습니다(비공개 입력이 필요하기 때문).
