# 릴리즈 생성 절차

이 문서는 유지보수자와 자동화 에이전트가 **검증한 커밋으로 설치 ZIP을 만들고 GitHub Release에 게시**하는 절차다. 사용자의 설치 방법은 [첫 OBSERVE 시험](FIRST_TRIAL_KO.md)과 [한 줄 설치](ONE_COMMAND_INSTALL_KO.md)를 따른다.

2026-10-01 사용자는 v1.0 전 한 번의 실차 설치·시험을 허용했습니다. 해당
통합 시험용 SHADOW 후보를 준비하되, 기존 공개 ZIP이나 소스 변경만으로 새
후보의 준비 완료를 선언하지 않습니다. 아래 검증과 [통합 시험 준비](FIELD_TRIAL_KO.md)를
따르며 실제 관성항법 적용이라는 [v1.0 완료 조건](V1_READINESS_KO.md)은 유지합니다.

## 배포 단위와 적용 범위

- 배포 단위는 **태그 + 해당 커밋에서 만든 설치 ZIP + SHA-256 + 릴리즈 노트**다. master 병합만으로 기존 릴리즈 파일이 바뀌지 않는다. GitHub가 자동 생성하는 `Source code (zip)`은 설치 묶음이 아니다.
- 리뷰, Ready 전환, LGTM, 병합, 릴리즈 발행은 각각 다른 작업이다. 요청받은 범위만 수행한다. 이미 릴리즈 발행을 지시받았다면 같은 승인을 반복해서 묻지 않는다.
- 이 절차 작성 시점(2026-09-29)의 master는 PR #12의 실제 센서 기반 SHADOW 계산을 포함한다. 기존 `v0.2.0-observe.2` 설치 파일은 이 변경을 포함하지 않는다. 새 릴리즈는 선택한 커밋의 전체 변경 범위를 설명해야 한다.
- 현재 설치 기본값은 OBSERVE, live ASSIST는 코드에서 차단된 상태다. SHADOW 지원 코드가 포함돼도 기본 설치에서 SHADOW가 켜지는 것은 아니다. 릴리즈 생성 과정에서 설정이나 차단 조건을 바꾸지 않는다.
- 예외적으로 [드문 실차 기회의 통합 시험](FIELD_TRIAL_KO.md)을 준비할 때는 의도적으로 선택한 `--default-mode=SHADOW` 묶음을 만들 수 있다. 일반 묶음의 OBSERVE 기본값은 유지한다. 이 경우 `bundle-default-mode`, build-info의 `default_mode`, 설치 안내와 릴리즈 노트가 모두 SHADOW로 일치해야 한다. 아래 일반 OBSERVE 예시를 그대로 복사해 잘못 표시하지 않는다. ASSIST 차단은 그대로다.
- 아래 명령은 **PR #12 이후의 다섯 바이너리 구성** 기준이다. 예전 태그를 재현할 때는 그 태그의 Makefile/packaging을 사용하며, 현재 스크립트나 바이너리를 섞지 않는다.
- 실차 미검증 개발판은 pre-release로 발행한다. 빌드·호스트·QEMU 성공을 실제 차량 복구, 위치 정확도, 폰/지도 앱 수용 검증으로 표현하지 않는다. [현재 상태](STATUS_KO.md)가 기능·실차 시험 범위를 정한다.

## 1. 환경과 배포 대상 고정

x86-64 Linux와 Bash를 기준으로 한다. 고정 도구체인의 컴파일러 자체가 Linux x86-64 실행 파일이므로 Apple Silicon macOS에서 직접 실행하는 절차가 아니다.

필요 도구: Git, Python 3, make, C/C++ 컴파일러, pkg-config, D-Bus 개발 파일, qemu-user, zip/unzip, file, sha256sum, GitHub CLI(`gh`). Debian/Ubuntu의 빌드 의존성은 `build-essential python3 pkg-config libdbus-1-dev qemu-user zip unzip file git`이며 `gh`는 별도 준비한다. GitHub 인증은 기존 승인된 계정을 사용한다.

먼저 [릴리즈 목록](https://github.com/junoh-moon/mazda-aa-dr/releases)과 변경 내용을 확인하여 **새 버전명과 배포할 40자리 커밋 SHA**를 정한다. `v0.2.0-observe.N`은 관측 시험판 명명 예시이지 다음 번호의 자동 결정 규칙이 아니다. 센서 SHADOW 기능 추가를 단순 설치 편의 변경으로 설명하지 않는다.

아래 자리표시자 두 개를 바꾼 뒤, 이후 블록을 **같은 Bash 세션에서 순서대로** 실행한다. 명령이 실패하면 원인을 해결한 뒤 해당 단계부터 진행한다.

```bash
set -euo pipefail
RELEASE_REPO='junoh-moon/mazda-aa-dr'
RELEASE_TAG='REPLACE_WITH_NEW_TAG'
RELEASE_COMMIT='REPLACE_WITH_40_CHARACTER_COMMIT_SHA'
[[ "$RELEASE_TAG" =~ ^v[0-9][A-Za-z0-9._-]*$ ]]
[[ "$RELEASE_COMMIT" =~ ^[0-9a-f]{40}$ ]]

gh auth status
RELEASE_WORK=$(mktemp -d /tmp/mazda-aa-dr-release.XXXXXX)
git clone "git@github.com:$RELEASE_REPO.git" "$RELEASE_WORK/source"
cd "$RELEASE_WORK/source"
git remote set-url --push origin "git@github.com:$RELEASE_REPO.git"
git fetch origin master --tags
git cat-file -e "$RELEASE_COMMIT^{commit}"
git merge-base --is-ancestor "$RELEASE_COMMIT" origin/master
git checkout --detach "$RELEASE_COMMIT"
test "$(git rev-parse HEAD)" = "$RELEASE_COMMIT"
test -z "$(git status --porcelain)"
if git show-ref --verify --quiet "refs/tags/$RELEASE_TAG"; then
  echo 'Existing tag: select a new version; do not overwrite it.' >&2
  exit 1
fi

mkdir "$RELEASE_WORK/evidence" "$RELEASE_WORK/dist"
RELEASE_NAME="mazda-aa-dr-$RELEASE_TAG"
RELEASE_BUNDLE="$RELEASE_WORK/dist/$RELEASE_NAME"
```

한 작업 디렉터리에서 다른 커밋으로 전환하고 빌드 결과를 재사용하지 않는다. 버전이나 소스를 바꾸면 새 작업 디렉터리에서 다시 만든다.

## 2. 고정 ARM 도구체인으로 빌드

[도구체인 상세](toolchain.md)의 고정 upstream 커밋은 `61ec0343de84f6fc7c46840056df1d600d44be8a`, GCC 4.9.1, ARMv7 Cortex-A9 NEON **softfp**다. 임의 최신 ARM 컴파일러로 만든 결과를 정식 빌드와 혼동하지 않는다.

```bash
python3 tools/fetch_m3_toolchain.py --jobs 16
MX5_TOOLCHAIN="$PWD/tools/m3-toolchain"
RELEASE_ARM_PREFIX="$MX5_TOOLCHAIN/bin/arm-cortexa9_neon-linux-gnueabi-"
RELEASE_SYSROOT="$MX5_TOOLCHAIN/arm-cortexa9_neon-linux-gnueabi/sysroot"
RELEASE_BUILD="$RELEASE_WORK/arm"
python3 tools/build_arm.py --toolchain "$MX5_TOOLCHAIN" --build-dir "$RELEASE_BUILD" \
  2>&1 | tee "$RELEASE_WORK/evidence/arm-build.txt"
sh packaging/make_bundle.sh "$RELEASE_BUILD/libmx5dr.so" "$RELEASE_BUNDLE"
```

묶음에는 같은 빌드의 `libmx5dr.so`, `libmx5dr-vimtap.so`, `mx5dr-collector`, `mx5dr-guard`, 정적 `mx5dr-sha256`, 각각의 `.sha256`, 설치·제거·로그 회수 helper와 기본 설정이 들어간다. `make_bundle.sh`는 ZIP, 전체 파일 manifest, 릴리즈 노트, GitHub Release를 생성하지 않는다. 출력 디렉터리가 이미 있으면 실패하므로 기존 묶음 위에 덮어쓰지 않는다.

릴리즈 빌더는 새 디렉터리에서만 컴파일하고 도구체인 blob, 컴파일 전후 입력,
실제 ARM ELF와 의존성, 다섯 결과물 해시를 `arm-build.json`에 기록한다.
ZIP 빌더는 이 기록과 현재 소스·바이너리를 대조한다. 과거 빌드 디렉터리에
새 소스의 정보를 덧붙여 릴리즈로 표시하지 않는다. 개발용 `make arm`은
계속 사용할 수 있지만 릴리즈 기록을 대신하지 않는다.

## 3. 실제 배포 바이너리와 테스트 확인

독립적으로 확보한 해당 펌웨어 fixture를 저장소 밖에 둔다. `packaging/firmware.sha256`에 나열된 파일 외에 `jci/version.ini`, `jci/sm/sm.conf`, `jci/sm/sm_WCP.conf`, `usr/bin/autostart`가 필요하다. 이 파일들은 호스트 fixture로만 읽고 복사하며 OEM 실행 파일은 실행하지 않는다.

```bash
MX5DR_STOCK_ROOT='/absolute/path/to/private/stock_reference'
test -d "$MX5DR_STOCK_ROOT"
MX5DR_STOCK_ROOT="$MX5DR_STOCK_ROOT" \
MX5DR_RELEASE_BUNDLE="$RELEASE_BUNDLE" \
  make test 2>&1 | tee "$RELEASE_WORK/evidence/host-tests.txt"

CROSS_COMPILE="$RELEASE_ARM_PREFIX" QEMU_SYSROOT="$RELEASE_SYSROOT" \
MX5DR_ARM_BUILD="$RELEASE_BUILD" \
  sh tests/run_arm_all.sh 2>&1 | tee "$RELEASE_WORK/evidence/arm-tests.txt"

for artifact in libmx5dr.so libmx5dr-vimtap.so mx5dr-collector mx5dr-guard mx5dr-sha256; do
  file "$RELEASE_BUILD/$artifact"
  "${RELEASE_ARM_PREFIX}readelf" -h -A -d -V "$RELEASE_BUILD/$artifact"
done > "$RELEASE_WORK/evidence/elf.txt"
```

확인 사항:

- `set -o pipefail`로 `tee` 성공이 빌드/테스트 실패를 가리지 않게 한다.
- `make test` 종료 코드만으로 판정하지 않는다. fixture 누락에 따른 packaging skip과 소켓 금지 환경의 exit 77은 전체 명령에서 성공처럼 보일 수 있다. 실제 PASS/FAIL/SKIP과 이유를 기록한다. 이번 다섯 바이너리를 사용하는 packaging 시험이 생략된 채 차량용 설치 ZIP의 검증이 끝났다고 하지 않는다.
- ELF32 little-endian ARM, softfp 호출 규약, TEXTREL 부재, GLIBC 버전/의존성을 확인한다. 현재 기대값은 GLIBC_2.4만 필요하고 동적 libstdc++ 의존성이 없는 것이다. D-Bus는 collector에 필요하며 AA preload로 돌아가면 안 된다. 과거 elf.txt를 새 바이너리의 결과로 재사용하지 않는다.
- ARM 로그 처음과 끝의 `ARM_TEST_INPUTS`에서 `release_verified=true`, 다섯 artifact 해시, 도구체인과 sysroot를 확인한다. 릴리즈 검사는 `MX5DR_ARM_BUILD`가 필수다. 개발용 `MX5DR_ARM_LIBRARY` 검사에는 입력 해시만 기록하며 릴리즈 검증으로 표시하지 않는다.
- ARM runner는 명시한 compiler/sysroot/preload를 사용한다. 상속된 GCC 검색 경로와 `LD_LIBRARY_PATH`, `LD_PRELOAD`, QEMU guest 환경 덮어쓰기는 제거하고 실제 loader 시험에서 지정한 preload만 적용한다.
- 변경 부분에 따른 추가 ARM 로더/guard 시험은 [통합 검증](../validation/INTEGRATION_2026-09-28.md), SHADOW 범위는 [기능 검증](../validation/LIVE_SHADOW_2026-09-29.md)을 참고한다.
- 원본 펌웨어, 개인 경로, 실차 위치 로그를 공개하지 않는다. 실행 로그는 먼저 비공개 evidence에 보관하고, 공개 검증 요약에 실행 환경·커밋·생략·미검증을 적는다.

## 4. USB 최상위 ZIP과 전체 체크섬

다음 빌더는 MP3/JS 진입 파일, 정적 해시 도구, 한국어 안내, 소스/바이너리
해시가 포함된 build-info, 전체 SHA256SUMS와 ZIP 외부 체크섬을 만듭니다.
SHADOW 통합 시험은 아래 모드를 `SHADOW`로 명시하십시오. 기존 3절에서
검사한 바이너리와 동일한 build 디렉터리를 사용하십시오.

```bash
python3 tools/make_usb_zip.py --build-dir "$RELEASE_BUILD" --default-mode OBSERVE \
  --output "$RELEASE_WORK/dist/$RELEASE_NAME.zip"
unzip -t "$RELEASE_WORK/dist/$RELEASE_NAME.zip"
mkdir "$RELEASE_WORK/unpacked"
unzip -q "$RELEASE_WORK/dist/$RELEASE_NAME.zip" -d "$RELEASE_WORK/unpacked"
(cd "$RELEASE_WORK/unpacked" && sha256sum -c SHA256SUMS)
```

ZIP에는 상위 폴더가 없어야 합니다. `install.sh`, `mp3/`, `js/`가 최상위에
있어야 MP3 태그의 고정 USB 경로가 동작합니다. `INSTALL_KO.md`를 과거
OBSERVE 문서로 덮어쓰지 마십시오. 묶음 내 파일을 고치면 다시 빌드하십시오.

가능하면 실제 펌웨어 rootfs와 ARM binfmt/QEMU가 있는 격리된 Linux 컨테이너에서
압축을 푼 최종 파일로 전체 설치 경로를 실행하십시오. 이 명령은 컨테이너
UID 0으로 실행하며 호스트를 재마운트하지 않습니다.

```bash
python3 tests/packaging/cmu_emulation.py \
  --stock "$MX5DR_STOCK_ROOT" --bundle "$RELEASE_WORK/unpacked"
```

SHA-256은 손상/동일성 검사이며 서명이 아닙니다. `build-info.json`의
`source_modified`는 정식 릴리즈에서 false여야 합니다. 로컬 수정 후보를
배포 커밋 그대로의 빌드로 표시하지 마십시오. ZIP에 원본 펌웨어, 위치 로그,
개인 공유 링크가 없음을 확인하십시오. MP3/JS 출처는 묶음의
`USB_ENTRY_NOTICE.md`에 보존합니다.

## 5. 릴리즈 노트와 게시

`$RELEASE_WORK/release-notes.md`를 작성한다. 최소 내용:

- 대상 펌웨어, 배포 커밋의 전체 SHA, 이전 릴리즈 대비 변경 사항과 포함 PR.
- 기본 모드, 실제 켜지는 기능, ASSIST 차단과 실차 미검증 범위.
- 실행한 호스트/ARM/패키징 검사와 생략한 검사·이유. 과거 실행 결과를 이번 결과처럼 쓰지 않는다.
- 다운로드할 정확한 ZIP 이름·SHA-256, 설치 `sh ./install.sh`, 정상 전원 주기 및 주차 중 회수·제거 안내. 설치 문서 링크는 배포 커밋 또는 태그에 고정한다.
- 이전 릴리즈와의 명령 차이와 알려진 제한. 예를 들어 `observe.1`은 `--remount`가 필요했고 `observe.2`부터 기본 설치가 이를 처리한다.

준비 완료 후 명시적으로 선택한 커밋에 태그를 만들고 **초안 릴리즈**에 정확한 두 파일만 첨부한다. 태그가 중간에 다른 곳에서 만들어졌다면 push 실패를 해결하기 위해 강제 갱신하지 않는다.

```bash
test -s "$RELEASE_WORK/release-notes.md"
test "$(git rev-parse HEAD)" = "$RELEASE_COMMIT"
test -z "$(git status --porcelain)"
git tag -a "$RELEASE_TAG" "$RELEASE_COMMIT" -m "Release $RELEASE_TAG"
git push origin "refs/tags/$RELEASE_TAG"
gh release create "$RELEASE_TAG" --repo "$RELEASE_REPO" \
  --verify-tag --draft --prerelease --latest=false \
  --title "$RELEASE_TAG" --notes-file "$RELEASE_WORK/release-notes.md" \
  "$RELEASE_WORK/dist/$RELEASE_NAME.zip" \
  "$RELEASE_WORK/dist/$RELEASE_NAME.zip.sha256"
gh release view "$RELEASE_TAG" --repo "$RELEASE_REPO"
```

릴리즈 화면에서 태그의 커밋, 노트, 첨부 파일 이름·크기를 확인한다. 이번 작업 범위에 발행이 포함되어 있고 검증이 끝났으면 게시한다. 초안 작성만 요청받았다면 여기서 멈춘다.

```bash
gh release edit "$RELEASE_TAG" --repo "$RELEASE_REPO" \
  --draft=false --prerelease --latest=false
```

`gh`를 사용할 수 없는 경우에도 GitHub UI/API로 **동일한 태그·커밋, pre-release 설정, ZIP·체크섬 첨부**를 적용한다. 릴리즈 제목만 만들거나 소스 ZIP만 있는 상태를 배포 완료라고 하지 않는다.

## 6. 게시된 파일을 다시 받아 검증

```bash
mkdir "$RELEASE_WORK/downloaded"
gh release download "$RELEASE_TAG" --repo "$RELEASE_REPO" \
  --pattern "$RELEASE_NAME.zip" --pattern "$RELEASE_NAME.zip.sha256" \
  --dir "$RELEASE_WORK/downloaded"
(cd "$RELEASE_WORK/downloaded" && sha256sum -c "$RELEASE_NAME.zip.sha256")
cmp "$RELEASE_WORK/dist/$RELEASE_NAME.zip" "$RELEASE_WORK/downloaded/$RELEASE_NAME.zip"
unzip -t "$RELEASE_WORK/downloaded/$RELEASE_NAME.zip"
gh release view "$RELEASE_TAG" --repo "$RELEASE_REPO"
```

공개 상태와 pre-release 표시, 다운로드 파일 동일성을 확인한 뒤 **릴리즈 URL·태그·소스 커밋·ZIP 해시·검증 범위**를 보고한다. SHA/태그/파일이 다르거나 다운로드가 실패하면 완료로 보고하지 않는다. Git에 옮길 검증 기록은 새 날짜의 파일로 남기고, 이전 결과를 덮어쓰지 않는다.

이미 게시한 태그를 다른 커밋으로 옮기거나 같은 이름의 ZIP을 조용히 교체하지 않는다. 코드나 첨부 파일 수정은 새 버전으로 발행하고 이전 버전의 알려진 문제를 설명한다. 사용자의 차량에 설치한 내용을 제거하는 절차는 [첫 시험 문서](FIRST_TRIAL_KO.md)를 따르며 GitHub 릴리즈 삭제와는 별개다.

## 참고

- [고정 도구체인](toolchain.md), [기여·테스트](../CONTRIBUTING.md), [패키징 스크립트](../packaging/make_bundle.sh)
- GitHub CLI 공식 문서: [create](https://cli.github.com/manual/gh_release_create), [edit](https://cli.github.com/manual/gh_release_edit), [download](https://cli.github.com/manual/gh_release_download)
- 이 문서는 수동 발행 절차다. 작성 시점에 자동 릴리즈 GitHub Actions는 없다.
