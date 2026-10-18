# 릴리즈 게이트 시험 (tests/gates)

v1.0.0 안정판의 사전 등록 게이트(`validation/V1_STABLE_DECISIONS_2026-10-18.md`, 감사 4절의 G5~G11, G16) 중 저장소 밖 임시 스크립트로만 돌던 시험을 추적되는 스크립트로 옮긴 것입니다. 실행 기록과 첫 결과는 `validation/V1_C_GATES_2026-10-18.md`에 있습니다. 릴리즈 절차에서 부르는 방법은 `docs/RELEASING_KO.md`와 `tools/release/`를 보십시오.

`make test`에는 들어가지 않습니다. 비공개 입력(순정 펌웨어 트리, 차량 내보내기, 비공개 부팅 목록), PRoot, qemu-arm, 고정 ARM 도구체인이 필요하기 때문입니다. 입력이 없으면 각 스크립트는 이유를 적고 종료 코드 77(SKIP)로 끝납니다. **SKIP은 통과가 아닙니다.** 릴리즈에서 SKIP을 허용하려면 `REL_ALLOW_SKIP`에 게이트 이름을 적고 릴리즈 기록에 이유를 남기십시오.

## 공통 환경 변수

| 변수 | 뜻 | 기본값 |
|---|---|---|
| `MX5DR_STOCK_ROOT` | 순정 펌웨어 트리(읽기 전용, 저장소 밖) | 없음(필수) |
| `MX5DR_PROOT` | PRoot 실행 파일 | PATH의 `proot` |
| `QEMU_ARM` | qemu-arm 사용자 모드 에뮬레이터 | PATH의 `qemu-arm`, 그다음 `qemu-arm-static` |
| `MX5DR_GATE_WORK` | 복제 루트·화면 기록을 둘 작업 디렉터리 | 새 `mktemp -d` |
| `MX5DR_GATE_NICE` | 게스트 명령의 nice 값 | 19 |
| `MX5DR_LIBPATCH_DIR` | 공개된 oem-aa-mod 폴더(libpatch DSO 3개) | 없음(없으면 oem-aa-mod 없이 복제) |
| `MX5DR_VEHICLE_EXPORT` | 소유자 차량 내보내기 트리(비공개) | 없음(터치·메뉴 6 시험은 SKIP) |
| `MX5DR_REPLAY_CHAIN` | 비공개 부팅 목록(`<logs 디렉터리> <boot id>` 한 줄씩, 주행 순서) | 없음(G11은 SKIP) |
| `MX5_TOOLCHAIN`, `CROSS_COMPILE`, `QEMU_SYSROOT` | 고정 GCC 4.9.1 도구체인 | `저장소/tools/m3-toolchain` |

순정 트리는 읽기만 합니다. 복제 루트는 `cmu_emulation.make_root`가 순정 BusyBox 1.19.2·libc·펌웨어 해시 대상 파일을 작업 디렉터리로 **복사**해서 만듭니다. 비공개 자료는 경로로만 참조하며 저장소나 요약 출력에 복사하지 않습니다.

## 복제 루트가 하는 것과 하지 않는 것 (`emu.py`)

- 게스트 명령은 PRoot(가짜 UID 0)와 qemu-arm 아래에서 순정 ARM BusyBox로 실행합니다. 숫자 메뉴(`trial`)는 USB 위치(`/tmp/mnt/sda1`)에서 실제로 돌고, 설치된 ARM 가드는 설치된 `/usr/bin/autostart`의 소유 블록을 통해 실행됩니다(확인 대기 90 s를 3 s로, collector 경로만 바꿈).
- 부팅은 작성한 boot_id 변경과 `/tmp` 비우기, SM은 작성한 `/proc/266` 항목, SM 리셋은 작성한 `/data/*.out`입니다. **OEM PID 1, SM, AA, VBS, LDS, collector는 실행하지 않습니다.**
- 그래서 메뉴 2의 GO/NO-GO 블록에서 런타임 증거가 필요한 줄(POLL, HLTH, 서비스, BETA, HOOK, FENCE)은 이 복제에서 'wait'일 수밖에 없습니다. 게이트는 "NO 줄 없음, 판정이 GO 또는 WAIT"(`install_side_go`)를 봅니다. 실제 GO는 차량에서만 나옵니다.

## 게이트별 설명

### G5/G6 — `g5g6_zipcheck.py` (ZIP 수준 검사)
- 증명: `unzip -t`, 최상위 폴더 없음, `SHA256SUMS`가 검증되고 모든 파일을 나열, 진입 파일(`mp3/` 4개, `js/run.js`, `USB_ENTRY_NOTICE.md`), `INSTALL_KO.md`, build-info의 `default_mode`(기본 BETA)·`install_policy=persistent`·`source_modified=false`·고정 커밋·산출물 해시, `bundle-default-mode`, 바깥 `.sha256`. 여섯 ARM 산출물마다 ELF32 ARM EABI5, softfp(하드 플로트 플래그 없음, VFP 인수 전달 없음), TEXTREL 없음, 심볼 버전은 GLIBC_2.4만, 동적 libstdc++/libgcc_s 없음, `.sha256` 일치, `mx5dr-sha256`은 정적.
- 실행: `python3 tests/gates/g5g6_zipcheck.py --zip Z.zip --commit SHA40` 또는 `make gate-zip GATE_ZIP=Z.zip`. 수 초.
- 증명하지 않는 것: 바이너리 동작, 차량 로더 호환(G3/G4가 봄), 진입 파일의 출처·라이선스.

### G7 — 순정 BusyBox 하네스
- `g7_cmu_emulation.py`: 추적된 `tests/packaging/cmu_emulation.py`의 주 흐름을 UID 0 컨테이너 없이 PRoot로 실행합니다. 바꾸는 것은 게스트 실행 방법뿐입니다(D1 chroot→PRoot, 시간 제한 900 s; D2 `/dev/null` 자리 파일; D3 PRoot 가짜 루트; D4 아카이브 소유자 비교에서 호스트 사용자를 0으로). 계정·소유권 회귀 모드는 실제 소유자 변경이 필요해 실행하지 않습니다. 약 10~20분.
- `g7_multiboot.py`: beta.17 릴리즈의 다중 부팅 시험(persist_sim)을 옮긴 것입니다. 설치·첫 부팅 차단, 확인된 제품 부팅, 같은 부팅 재선택 거부, 연속 SM 리셋 2회 차단, 메뉴 3 내보내기·메뉴 1 재활성화와 증거 보존, 수습 차단, 확인 없는 짧은 부팅 3회 차단, 다른 도구의 sm.conf 편집 표시와 순정 전환, 멈춘 가드의 15 s 상한, 메뉴 4의 바이트 단위 복원. 약 10분.
- `g7_menus.py`: 메뉴 7~12 토글(목록은 `--toggles`/`MX5DR_GATE_TOGGLES`, 기본은 beta.17 목록). 미설치 시 변화 없음, 다른 숫자 입력은 취소, 같은 숫자 두 번이면 표식이 뒤집히고 임시 파일이 남지 않음, 다시 하면 원래대로, 디렉터리 표식은 보고하고 그대로 둠. 표식의 뜻(어느 상태가 켬인지)은 여기서 보지 않습니다(G8이 설치기 규칙을 봄). 약 5분.
- `g7_touch.py --scenario A|B`: 터치 모드(libpatch) 호환 시험을 옮긴 것입니다. 차량의 sm.conf·sm_WCP.conf·autostart·version.ini와 oem-aa-mod로 복제를 만들고 서비스별 preload 사슬, 우리 토큰 3개를 빼면 차량 파일과 바이트 동일, 일반·WCP 분기, 터치 모드 편집 뒤 순정 전환과 메뉴 1 재채택, 차단, 메뉴 4 복원을 봅니다. `MX5DR_VEHICLE_EXPORT`와 `MX5DR_LIBPATCH_DIR`가 없으면 SKIP. 각 약 5분.
- 메뉴 6 복제(`tests/packaging/replica_clean_uninstall.py`)는 이미 추적되어 있어 `tools/release/stock.sh`가 그대로 부릅니다.
- 증명하지 않는 것: 실제 SM이 trial 설정을 받아들이는지, 실제 마운트·전원·플래시 동작, 실제 기동 순서.

### G8 — `g8_upgrade.py` (직전 공개 ZIP → 후보)
- 증명: 직전 ZIP(기본 beta.17, sha256 확인)으로 메뉴 1·5·제품 부팅·메뉴 2, 직전 ZIP 자신의 메뉴로 표식 설정, 작성한 직전 부팅 토큰·워커 부팅 파일·40 MiB 시절의 옛 trace(17 MiB × 3과 trace.3) 위에 후보 ZIP 메뉴 1 → 메뉴 5 → 제품 부팅(일반·WCP) → 메뉴 2. 설치 rc 0, pending 없음, 설치 파일이 후보, `--cleared` 표식 해제, 실험 표식(`--exp-markers`), 토큰 제거(`--token-cleared`), trace 정리(`--trace-cleanup`), 순정 파일 바이트 동일(autostart는 소유 블록 제거 후), 수습 상태, 세 preload를 가진 trial, 확인, NO 줄 없음.
- 아직 결정되지 않은 규칙은 플래그로 둡니다. **워크스트림 B의 설치기 변경(실험 표식 `g3-garage-off`·`bt-off`를 새 설치기가 만들고, 상한 넘은 trace를 지움)이 들어오기 전까지 기본값은 `--exp-markers skip --trace-cleanup skip`** 입니다. B의 변경이 들어오면 `--exp-markers present --trace-cleanup yes`로 실행하십시오(`REL_GATE_FLAGS`). 토큰 규칙의 기본은 `yes`(master 설치기가 지움)이고 beta.17 자신은 지우지 않으므로 beta.17을 후보로 넣을 때는 `--token-cleared no`입니다.
- 실행: `make gate-g8 GATE_CANDIDATE=후보.zip GATE_G8_FLAGS='...'`. 약 5~8분.
- 증명하지 않는 것: 직전 버전이 실제 차량에서 남긴 상태 전부(실제 trace 내용, collector 상태), 런타임의 첫 부팅 동작.

### G9 — `g9_powerloss.py` (전원 차단)
- 방법: 게스트 PATH 맨 앞의 `/test-bin`에 변경 명령(mv cp rm ln mkdir rmdir chmod chown touch sync mktemp dd tar) 감싸개를 둡니다. 끊지 않는 기준 실행으로 단계에 번호를 매기고, 차단점 k마다 k번째 명령 직전에 `/powercut`를 쓰고 멈춘 뒤 드라이버가 게스트 프로세스 그룹 전체(PRoot, qemu, 모든 셸)를 SIGKILL합니다. 그다음 새 부팅(`/tmp` 비움, 새 boot ID, 설치된 블록과 실제 ARM 가드)을 합니다.
- 차단점마다 증명: SM이 순정 설정 또는 세 preload를 모두 가진 trial을 받음(빠지거나 반쯤 된 설정 없음), autostart가 순정 BusyBox `sh -n` 통과, 순정 파일 바이트 동일, 숫자만으로 복구(메뉴 4 rc 0 → 순정 autostart(끝 개행만 허용)와 순정 부팅, `--recover-every`마다 메뉴 1 → 5 → 완전한 제품). 단계: 메뉴 1(설치)과 실행 중 제품의 메뉴 4(제거).
- 실행: `python3 tests/gates/g9_powerloss.py --candidate Z --stride 1`(전체, 차단점당 약 1분, 설치·제거 합계 수 시간) 또는 `--stride 10`(빠른 확인). `make gate-g9 GATE_G9_STRIDE=1`.
- 증명하지 않는 것: 한 명령 **안**에서의 차단(셸 리디렉션이 쓰던 파일이 반쯤 남는 경우, ARM 가드·해시 도구 내부), fsync 없이 최근 쓰기를 잃는 저장 장치, C 워커의 토큰·S 저장 쓰기(단위 시험 `test_g3`, `test_worker_beta g3_*`가 봄), 메뉴 6 중 차단.

### G10 — `g10_soak.sh` + `soak_worker.cpp` (soak)
- 방법: (1) 하나의 오래 사는 ARM(qemu-arm) 프로세스에서 실제 BETA 런타임 워커 스레드·저널 기록 스레드·모션 소켓·어댑터 POSITION/SEND를 제품 옵션(모드 5, persistent 로그 프로파일, 세션 관측 거절)으로 벽시계에 맞춰 돌립니다. `tests/runtime/test_worker_beta.cpp`의 작성된 OEM 끝점과 요청 판독기를 포함으로 재사용합니다. 36 km/h 직진, 12 s GPS fix(앵커는 10 s 필요) 뒤 5~29 s GPS 상실(BETA가 교체)과 복귀를 반복하고, 작은 trace 고리(256 KiB × 3)로 회전을 계속 일으킵니다. (2) 짧은 부팅 N회: 정확한 ARM `worker-beta-test` 시나리오(main budget nofix silence contradict fault no_anchor withdrawn_class cadence_gap nofix_rearm disable)를 각각 새 프로세스로.
- 증명: 저널 지연 가드 작동(가장 오래된 미기록 행 > 1.5 s, `journal_current` 1→0) 0회와 보존된 `beta_journal_lag lagging` 행 0개, 첫 앵커 뒤 모든 터널에서 교체 송신, trace 파일 3개 이하·각 파일 상한 이하, 힙 사용량(mallinfo)이 실행 25 % 시점 대비 끝에서 1 MiB 이하 증가, 단언 실패(중단) 없음, 짧은 부팅 모두 종료 0(77은 시간 단언 불확정으로 실패에 셈).
- 실행: `sh tests/gates/g10_soak.sh --fast`(긴 실행 7분 + 12부팅, 약 10분: 연기 시험이지 게이트가 아님) 또는 기본(4 h + 100부팅, 약 5 h). `SOAK_SECONDS`, `SOAK_BOOTS`로 조정. `SOAK_HOST=1`은 개발용 네이티브 빌드이며 게이트 증거가 아닙니다.
- 증명하지 않는 것: 실제 CMU의 CPU·메모리·플래시 속도, 실제 AA/VBS 입력 시점, collector와 LDS, 차량 재부팅 사이의 trace 이어 쓰기(그 규칙은 `test_journal`이 봄). qemu RSS는 에뮬레이터 자신을 포함하므로 보고만 합니다.

### G11 — `g11_replay.sh` (비공개 재생 바이트 동일)
- 방법: 비공개 부팅 목록(`MX5DR_REPLAY_CHAIN`)의 모든 부팅을, 하나의 G3 저장소를 이어 가며, 후보 체크아웃의 `replay_beta`와 직전 릴리즈 커밋(기본 beta.17 `2a154eeb…`을 `git archive`로 작업 디렉터리에 풀어 빌드)의 `replay_beta`로 실험 기능을 끈 채(`--g3-garage off`, 공통 `--real-only --g3 on --g3-send on`; 바꾸려면 `G11_*_ARGS`) 재생하고, 부팅마다 report·csv·journal·g3·sends·stdout·stderr를 비교합니다. G3 저장 스레드 시각 필드(`lat_ms`, `start_ns`, `end_ns`)만 정규화합니다.
- 출력: `G11: X/N files identical`과 다른 파일 이름. 20개 부팅(위치 행 없는 6개는 stdout/stderr만)이면 N = 110입니다.
- 증명하지 않는 것: 실험 기능을 켰을 때 허용 바이트만 바뀐다는 것(감사 G11의 후반; 기존 `g3_send_identity.py`와 단위 시험이 봄), persistent 기록의 원시 창 밖 구간(재생에서 `wheel_stale`), 차량에서의 화면 효과.

### G16 — `g16_rollback.py` (롤백 증명)
- 증명: 후보 메뉴 1·5·제품 부팅 뒤 숫자만으로 메뉴 4 → 5 → 다음 부팅에서 SM이 순정 sm.conf/sm_WCP.conf를 받고, 메뉴 2가 'stock this boot', autostart가 설치 전과 바이트 동일(끝 개행 하나만 허용), 소유 블록·persist 없음, 나머지 순정 파일 동일. 이어 직전 ZIP을 그 위에 다시 설치(메뉴 1·5·제품 부팅)하고 다시 제거.
- 실행: `make gate-g16 GATE_CANDIDATE=후보.zip`. 약 5분.
- 증명하지 않는 것: 소유자 차량에서의 롤백(감사 G16의 "소유자의 다음 주차 때 1회"), 메뉴 6 완전 삭제(메뉴 6 복제 시험이 봄).

## 한꺼번에

- `make gates-stock GATE_CANDIDATE=후보.zip`: G7 → G8 → G16 → G9.
- 릴리즈에서는 `tools/release/stock.sh`, `soak.sh`, `replay.sh`가 같은 스크립트를 부르고 결과를 `WORK/evidence/`에 남깁니다.
- 무거운 시험은 1분 부하가 4 이상이면 기다립니다(`REL_MAX_LOAD`, `G11_MAX_LOAD`). 다른 작업과 함께 돌릴 때 시간 계열 단언이 흔들릴 수 있으니 부하를 기록하십시오.
