# AA 위치 후킹과 서드파티 libpatch의 공존 — 2026-10-05

[2026-10-04 실차 시험](TRIP_RESET_2026-10-04.md)에서 AA 위치 후킹이 부팅 내내 설치되지 않은 원인(사용자의 oem-aa-mod `libpatch-blmjciaapa.so`가 BLM의 세션 슬롯을 가로챔)과, 이를 고친
`f97d13f`를 오프라인으로 검증한 기록이다. 차량에서 실행한 것은 없다.

## 방법

- 순정 루트(펌웨어 `74.00.324A` 추출본)의 `jci/aapa/blmjciaapa.so`와 `usr/lib/libaap_interface.so`를 QEMU 사용자 모드(8.2.2)에서 `proot -0`으로 `dlopen(RTLD_NOW)`하는 작은 ARM 프로브
  (`tests/adapter/aa_install_probe.c`, 순정 런처와 같은 의존 라이브러리를 링크). 순정 `sm_svclauncher`는 SM 없이 단독 실행되지 않아(`common_process.c: threadOnce` 단언) 쓰지 못했다.
- 프리로드: `LD_PRELOAD=/data_persist/mx5-aa-dr/libmx5dr.so[:/data_persist/oem-aa-mod/libpatch-blmjciaapa.so]`(차량과 같은 순서). `QEMU_SET_ENV`로 프리로드를, 호스트 `LD_LIBRARY_PATH`로 게스트 라이브러리 경로를 넘겼다.
  `/data_persist`는 순정 루트에서 `/mnt/data_persist` → `/tmp/mnt/data_persist`로 이어지는 심볼릭 링크라서 `-b DIR:/data_persist`만 바인드해야 한다(`/tmp`를 덮으면 링크가 끊긴다).
- `libpatch`: 공개 oem-aa-mod 릴리스 0.10.0(`a6b87f72…`). 저장소에 두지 않는다. 설정은 `mode=SHADOW`.
- 확인한 것은 첫 trace 행(`boot`)의 `install`, `session_hooks`, `install_diag`.

## 결과

| 조합 | `install` | `session_hooks` | `install_diag` |
| --- | --- | --- | --- |
| 이전 빌드(v0.3.12-shadow.4) + libpatch | `next_chain_mismatch_or_lazy_binding` | (필드 없음) | (필드 없음) — **차량의 실패 재현** |
| 이전 빌드, libpatch 없음 | `ok` | | 기준선 |
| 수정 빌드 + libpatch(알려진 경로) | `ok` | `declined_third_party_interposer` | stage 3, 심볼 `aap_destroy_session`, 소유자 `/data_persist/oem-aa-mod/libpatch-blmjciaapa.so` |
| 수정 빌드, libpatch 없음 | `ok` | `observing` | stage 0 |
| 수정 빌드 + 같은 libpatch를 알려지지 않은 경로에 둠 | `next_chain_mismatch_or_lazy_binding` | `none` | stage 3 — 안전 실패 |

마지막 세 경우(이전 빌드 재현 제외)는 `tests/adapter/run_aa_install_probe.sh`로 남았고 ARM 러너가 `MX5DR_AA_STOCK`, `MX5DR_LIBPATCH`를 받으면 실행한다(이번 릴리즈 검사에서 3개 통과).

## 해석

- 지연 바인딩이 아니었다: 같은 `RTLD_NOW` 경로에서 `libpatch` 유무만으로 결과가 갈렸고, 우리 `dlopen` 가로채기가 이미 `RTLD_NOW`를 강제한다(`src/runtime/loader.cpp:125`). `LD_BIND_NOW`는 효과가 없어 쓰지 않는다.
- 실패한 비교는 위치 송신 슬롯이 아니라 세션 슬롯이다(수정 빌드의 진단이 직접 `aap_destroy_session`을 가리킴).
- 수정은 소유자가 정확히 `/data_persist/oem-aa-mod/libpatch-blmjciaapa.so`(별칭 경로 포함)일 때만 세션 관측을 생략한다. 세션 관측은 정품 함수를 직접 호출해(`session_hooks.cpp`) `libpatch`의 shim을
  건너뛰게 되므로 슬롯을 그대로 인수하는 안은 택하지 않았다. 체인 방식(슬롯 값을 "다음"으로 저장)은 `libpatch`가 우리 콜백 표를 제자리에서 수정하는 상호작용을 따로 검증해야 해서 ASSIST 단계로 미뤘다.

## 한계

프로브는 순정 `sm_svclauncher`가 아니고 SM의 전체 기동 흐름이 없다. `libpatch`는 공개 0.10.0 한 버전이며 차량의 정확한 버전은 모른다(회수 도구가 이제 해시를 수집한다).
**`libpatch`의 터치·HUD·km/L 기능이 이 후킹과 함께 정상 동작하는지는 확인하지 못했다.** 설계상 그 슬롯을 건드리지 않을 뿐이다. AA 후킹이 실제 차량에서 설치되는 것은 처음이므로 새 위험이 생긴다.
이 기록은 차량 시험 승인이나 live ASSIST 활성화를 뜻하지 않는다.
