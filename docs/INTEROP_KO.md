# 다른 도구와의 연동 계약 (contract 1)

이 문서는 mazda-aa-dr 밖의 도구가 기대어도 되는 인터페이스만 정합니다. 여기 없는 파일·출력·메뉴 문구는 예고 없이 바뀔 수 있습니다. 첫 사용자는 mazda-cmu-tune입니다. 이 도구는 `/jci/sm/sm.conf`와 `sm_WCP.conf`를 바꾸므로, 바꾼 뒤 mazda-aa-dr를 다시 등록해야 합니다. 근거와 시험 기록은 `validation/REBASELINE_2026-11-01.md`에 있습니다.

## 왜 다시 등록이 필요한가

guard는 설치할 때 두 파일의 원본 해시(`guard/normal.source.sha256`, `guard/wcp.source.sha256`)를 저장합니다. 상시 BETA의 `guard/persist`는 이 파일들과 trial 템플릿의 해시를 묶어 둡니다. 다른 도구가 두 파일 중 하나라도 바꾸면 다음 부팅에서 guard는 `baseline_edited`로 판정하고 순정으로 부팅합니다(fail closed). 차에는 해가 없지만 제품은 꺼진 채가 됩니다. `rebaseline.sh`는 이 상태를 메뉴 1 재설치 없이 되돌립니다.

## 1. rebaseline.sh

- 경로: `/data_persist/mx5-aa-dr/tools/rebaseline.sh`. 메뉴 1(설치)이 함께 설치합니다. 이 파일이 없으면 rebaseline 이전 판(v1.0.3 이하)입니다.
- 실행: `sh /data_persist/mx5-aa-dr/tools/rebaseline.sh [--check|--version]`. root 권한이 필요합니다. 인자 없이 비대화식으로 돌고 stdin을 읽지 않습니다.
- 주차 중에만 실행합니다. CMU를 재시작하지 않습니다. OEM 파일(`sm.conf`, `sm_WCP.conf`, `autostart`)을 바꾸지 않습니다.
- 바꾸는 것: trial 템플릿 두 개와 원본 해시 두 개, 그리고 guard `enable`이 새로 쓰는 `persist`와 `persist-state`. 그래서 다음 부팅은 메뉴 1 직후와 같이 **probation 부팅**(첫 부팅 확인)이 됩니다.
- 바꾸지 않는 것: guard trip, 런타임 중지 요청(`logs/disable-next-start`), 소유자 스위치 표식(`*-off`), 휴대폰 채널 설정. 메뉴 1이 설치한 것과 다른 preload 바이트(`installed.txt`의 해시)나 설정(`mx5dr.conf`)은 승인하지 않습니다.
- 잠금: mount lock `/tmp/.mx5dr-mount.lock`(`flock -n`)과 설치 lock(`/data_persist/.mx5dr-install-lock`)을 쓰며 기다리지 않습니다. 이미 잡혀 있으면 바로 `precheck_failed:locked`로 끝납니다. **호출하는 쪽은 mount lock을 쥔 채 부르면 안 됩니다.**
- 실행 시간: 셸 처리, 해시 몇 번, guard 호출 4회(guard 한 번은 10 s 상한)입니다. 별도 타이머는 없습니다.
- 출력: 등록과 `--check`에서 stdout의 마지막 줄은 언제나 `result=` 한 줄입니다. 진단 문구는 그 앞 줄과 stderr에 나옵니다. 형식을 보장하는 것은 `result=` 줄뿐입니다.

### 등록 (인자 없음)

| 종료 코드 | 마지막 줄 | 의미 |
|---|---|---|
| 0 | `result=done` | 다시 등록함. 이미 일치했으면 아무것도 바꾸지 않고 0 |
| 1 | `result=precheck_failed:<reason>` | 아무것도 바꾸지 않음 |
| 2 | `result=tripped:<guard 이유>` | guard가 trip된 상태라 거부. 메뉴 3(내보내기) → 메뉴 1 |
| 3 | `result=partial:<stage>` | 교체 도중 실패. guard는 순정으로 부팅함. 다시 실행하거나 메뉴 4 → 1 |

`<reason>`: `usage`, `storage`, `not_installed`, `unsupported_policy`(상시 BETA가 아닌 설치), `pending`, `guard`, `not_enabled`, `runtime_disabled`, `baseline_has_token`, `guard_<코드>`, `locked`, `firmware`, `payload`, `config`, `template`, `concurrent_edit`, `evidence`, `error`.
`<stage>`: `publish`, `check`, `enable`, `verify`, `cleanup`.

### --check (읽기 전용, 잠금 없음)

| 종료 코드 | 마지막 줄 | 의미 |
|---|---|---|
| 0 | `result=match` | 두 원본 해시가 현재 파일과 일치하고 guard 검증이 ok |
| 4 | `result=mismatch:<baseline_edited:…\|binding_changed:…>` | 다시 등록이 필요함 |
| 2 | `result=tripped:<reason>` | trip 상태. 불일치보다 우선함 |
| 1 | `result=unknown:<reason>` | 판단할 수 없음(미설치, 상시 BETA 아님, 런타임 중지 요청 등) |

### --version

`contract=1` 한 줄만 출력하고 0으로 끝납니다. 이 경우에만 `result=` 줄이 없습니다. 계약을 넓힐 때는 `result=` 형식을 유지하고 contract 번호를 올립니다.

## 2. 버전 표시

- USB 묶음: 루트의 `bundle-version` 한 줄(`X.Y.Z` 또는 `X.Y.Z-tag.N`, `v` 없음)과 `build-info.json`의 최상위 `"version"`(같은 값). 차 안의 셸에서는 `bundle-version`을 읽으십시오.
- 차: `/data_persist/mx5-aa-dr/installed.txt`의 `version=` 줄. 줄 위치는 고정하지 않으니 `^version=`로 찾으십시오.
- 비교: 점으로 나눈 숫자 세 개를 비교하고, `-tag.N`이 붙은 판은 같은 숫자의 정식판보다 낮습니다.
- 표시가 없을 때: 차 쪽에 `version=`이 없으면 v1.0.3 이하 설치본입니다. USB에 `bundle-version`이 없으면 rebaseline 이전 묶음이거나 개발용 묶음입니다.
- 릴리즈 ZIP 이름은 `mazda-aa-dr-v<버전>.zip` 형식을 유지합니다.

## 3. USB 묶음과 메뉴

- ZIP 형식: 루트에 `SHA256SUMS`가 있고, 스캐너 진입 파일(`.up`) 하나는 `SHA256SUMS`에 들어 있지 않습니다(v1.0.3부터). 다른 묶음과 합칠 때 `.up`을 빼도 `SHA256SUMS`는 그대로 검증됩니다.
- 루트의 `trial`(숫자 메뉴)과 `touch.sh`(터치 화면 진입)입니다. `MX5DR-TOUCH.ID`는 `.up` 진입 식만 쓰므로 계약에 넣지 않습니다.
- `trial` 메뉴 번호: `1` 설치, `3` 로그 마무리·내보내기, `4` 제거. 첫 화면의 이 번호는 바꾸지 않습니다.
- 설치 여부: `/data_persist/mx5-aa-dr/installed.txt`가 있으면 설치된 상태입니다.

## 계약 밖이지만 당분간 유지하는 것

contract 1로 옮기기 전의 mazda-cmu-tune 0.1.x는 `printf '1\n0\n' | sh trial`의 `^Installation finished` 줄과 `trial` 메뉴 5(재부팅)를 씁니다. 이 둘은 계약에 넣지 않지만, 그 판이 쓰이는 동안 바꾸지 않습니다.
