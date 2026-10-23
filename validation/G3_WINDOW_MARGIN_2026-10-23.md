# garage carry 90° 열기 한계 + 15° 히스테리시스(소유자 수정 결정)와 정지 부팅 사슬 재확인 (2026-10-23)

상태: 소스 변경 + 호스트 시험만 했습니다. 차량·폰·DHU에서는 실행하지 않았고, 이 기록은 차량 방문·주행·발행 요청이 아닙니다. 좌표·주소는 적지 않았습니다. 표시: **FACT**(로그·코드·시험에서 직접 계산), **INFERENCE**(해석·모델).

**결론:** G3_UNC_LIMIT(2026-10-18)가 "판정 불가"로 남긴 두 레버(열기 한계 60°→90°, 열린 창의 `unc_grew` 히스테리시스)를 **소유자의 수정 지시**에 따라 후보로 상향합니다. 사전 등록 최소 표본(사례 5개·부팅 3개)은 여전히 충족되지 않으므로 이것은 자료에 의한 판정이 아니라 소유자 결정이며, 라벨은 "재구성 자료 기반, 실차 미시험" 그대로입니다. 또한 **정지 부팅 사슬 단절(`not_prev_boot` 2/6 사슬)은 v1.0.0의 lazy token에서 이미 수정이 반영돼 있고, 비공개 저널 재확인 결과 실제 관측 정지 부팅 두 개 모두 현재 빌드에서 토큰을 교체하지 않습니다** — 이 기록의 FACT 1절. 따라서 이 변경으로 다음 것이 실제로 가능해집니다: **2026-10-10→11 집 나선 출구 사슬**(첫 FIX B 80.5°, 순정 평균 49.9°·최대 171° 틀림 대 운반 −34.3°)과 **이마트 출구의 창 유지**(순정 평균 43.3° 대 운반 6.7°, 48개 추가 송신).

## 아래를 본 뒤의 배경 (채팅마다 순서 기록)

이전 세션에서 "정지 부팅 결함 수정 구현"을 다음 수정본의 핵심으로 잡았습니다(작업 메모). 이번에 코드와 실측을 다시 읽어 그 계획이 **무효**(이미 v1.0.0에 구현·시험 완료)임을 확인하고, 남은 레버(한계·히스테리시스)가 정확히 G3_UNC_LIMIT가 기각한 것들임을 확인한 뒤 소유자에게 상태를 보고했습니다. 소유자 응답(2026-10-23 채팅): **"방법은 상관 없고, 내가 설정한 목표에 더 다가간 수정본이 난 그저 필요할 뿐이야."** 이 문장이 두 레버의 소유자 결정 근거입니다. 발행·설치·주행은 이 기록으로 요청하지 않습니다.

## FACT 1: 정지 부팁 사슬 단절은 v1.0.0에서 이미 수정됨

- 코드: `src/runtime/g3_heading.h`의 lazy token. 시험 `tests/runtime/test_g3.cpp` `token_chain()` 첫 시나리오: "A가 주행·주차, B가 시동만 켜고 바퀴 정확히 0을 2분 섭취 → C 시작: `chain_depart` (HEAD의 eager swap: `not_prev_boot`, 결함)" — 현재 빌드는 통과합니다. HEAD 결함 동작은 마커 부팅(g3-garage-off·no-BETA) 전용으로 보존돼 있습니다. **FACT**(시험 통과).
- 실측 재확인(비공개 내보내기 `phone-imu/cmu-20261017` `logs/trace.1.jsonl`, 저널 행 순서로 부팅 귀속):
  - 094c62b5(30 s급 정지 부팅, 집 나선 사슬 사이): raw 바퀴 이벤트 420건, **첫 이벤트 부팅 후 4.2 s**, 이벤트 간 최대 간격 **0.29 s**, 바퀴 수치 전부 영(=10000 캘리브레이션 영). **FACT**.
  - 07ea46fa(2026-10-17 #12, 112 s급 정지 부팅): 이벤트 550건, 첫 이벤트 0.1 s, 최대 간격 **0.59 s**. **FACT**.
- 따라서 현재 규칙(`G3TokenWatch`: 30 s 무이벤트 `blind_silent`·2 s 간격 종료 `blind_gap`·2 m 이동) 어느 것도 이 부팅들에서 작동하지 않고, 토큰은 교체되지 않으며, 다음 부팅은 기록을 `chain_depart`로 이월합니다. G3_UNC_LIMIT의 "부수 발견 1"(`작업자 시작마다 g3-last-start는 바뀌므로`)은 그 당시 빌드 분석이고 현재 v1.0.0 규칙에는 해당하지 않습니다. **INFERENCE**(실측 → 현재 코드 규칙 적용).
- 남은 잔류(문서화된 fail-closed): raw 바퀴 이벤트를 **하나도 받지 못한** 정지 부팅(바퀴 스트림 고장)은 30 s 후 스왑을 하므로 여전히 사슬을 끊습니다. 이는 본래 문서화된 안전쪽 잔류이고 본 변경의 대상이 아닙니다.

## FACT 2: 소스 변경 (커밋 9a3dfff, 2026-10-24)

- 열기·적재 관문: `G3_GARAGE_UNC_MAX_DEG` 60.0→**90.0**(플래그 4 기록의 적재 `stored.unc_cdeg ≤ 9000`, 그 외(플래그 3 등)는 기존 30° 유지).
- 닫힘 관문: `unc_grew` 경계를 `unc_max() + G3_GARAGE_UNC_GREW_MARGIN_DEG(15.0)`로(플래그 4 운반만; 플래그 3은 마진 없이 30° 그대로). 즉 열린 창은 운반 불확실도가 105°를 넘을 때만 `unc_grew`로 닫힙니다. 마진값은 G3_UNC_LIMIT가 관측한 이마트 출구 성장분(58.0→64.4°, +6.4)과 집 나선(80.5→89.3, +8.8)의 상한이 아닌 문서 제안값 15°입니다. **FACT**(수동 반영).
- 기본값/끄기: 이 변경은 garage carry 내부이므로 거는 스위치는 `g3-garage-off` 표식(메뉴 12)과 `g3-off`(메뉴 8) 그대로이며 새 메뉴·표식은 없습니다.
- 다른 것은 하나도 바꾸지 않았습니다: 전파식(2 %·0.0023 rad/s), 레일 여유, 기록 저장 관문, token 규칙, 송신 창의 나머지 끝 조건(신뢰 4회·충돌 8회·`motion_60s`·900 s 캡·`m_course`), 후진 래치, ATTRIBUTE 계약.

## DATA: 소유자 자료에서의 예상 효과(G3_UNC_LIMIT 표 그대로 재게시)

| 사례 | 기존 v1.0.0 | 변경 후(90 + 마진) | 운반 오차 대 순정 오차 |
|---|---|---|---|
| 이마트 출구(2026-10-17 #13→#14) | 창 열림 뒤 12개 FIX에서 `unc_grew`로 닫힘 | **60개 FIX**, 첫 3.6 km/h FIX 뒤 60 s(`motion_60s`)에 종료 | 평균 +6.7° 대 43.3°(이동 구간 18개는 순정 3.9°로 조금 나음; 정지·서행 30개는 순정 평균 67°) |
| 집 나선 출구(2026-10-10 322800ef → 2026-10-11 e5584f62) | 열리지 않음(80.5° > 60°) | **62개 FIX**, `motion_60s` 종료 | −34.3° 대 49.9°(최대 171°) |

- 집 나선 사슬은 FACT 1 덕에 현재 빌드에서 기록이 실제로 이월됩니다(같은 개선이 2026-10-18 시점 빌드라면 한계와 무관하게 여전히 `not_prev_boot`).
- 이 순간들 대부분은 후진·정지·서행(재획득 직후)입니다. 네이버가 정지·서행 중 순정 진행방향(주로 0)을 화면에 쓰는지는 알 수 없으므로, 화면 효과는 여전히 미측정입니다. **INFERENCE**.

## 시험 (FACT)

- `tests/runtime/test_g3.cpp` 갱신: 적재 관문 경계(9000 cdeg 수용, 9001 거절), 전파·닫힘 경계(62.02° 유지 — 구 60° 규칙에서의 거절, 105.0° 유지, 106.02° `unc_grew`), 창 종료 경계(106.02°에서 `rejected/unc_grew` — 이전엔 62.02°), 비-플래그-4(30°, 마진 없음)·플래그 3·마커 부팅 회귀는 그대로 통과.
- `build/test_g3`: 1874 checks passed. `test_worker_beta` 개별 시나리오 26종 + 타이밍 하네스(`g3_garage`, `g3_cancel`) + `g3_send` 보내기 동일성(29 calls, bearing 바이트만 변경, 나머지 바이트 동일) 전부 통과. 전체 `make test`: **EXIT=0**(2026-10-24 rerun, unittest 1,261개, 스킵은 선택·비공개 환경; 이전 전체 실행 de637a3의 1,261과 같은 셋). `g3-last-start`·기록 저장 I/O 경로와 PHI 계약은 무변경.

## 한계·잔류 (정직 라벨)

- 표본 2 사례 < 사전 등록 최소 5. 이 변경은 판정이 아니라 소유자 위험 수용입니다. 꼬리 재점검: 열기 한계 90에서 60<B≤90 구간 창의 5 %가 실오차 > 60°(G3_UNC_LIMIT, 판정에 쓰지 않음). 마진은 열린 뒤 추가 성장분으로 위험을 묶는 것이지 오차를 줄이지 않습니다.
- 열린 창이 105°까지 열려 있는 동안 순정이 더 나은 순간(이동 중이면 그런 사례가 있음: 이마트 3.9° 대 6.7°)에 더 보냅니다. 손익 구조는 출구 직후 정지·서행 위주에서 이득, 이동 중 일부에서 소폭 손실. 900 s 캡과 신뢰/충돌 관문이 이전과 같이 묶습니다.
- 플래그 3(마커·beta.16 기록) 규칙은 30° 그대로 — 마진이 걸리지 않게 코드로 분리해 뒀습니다.
- 실차·폰에서는 시험하지 않았습니다. 화면 효과(출구 화살표)는 소유자 관찰 항목(G3_UNC_LIMIT "다음 실차 시" 항목)으로만 남습니다. G3 해결 주장을 하지 않습니다.
- v1.0.0(발행본)은 무변경입니다. 이 변경은 미발행 작업 트리의 내부 커밋이며, 발행은 별도 소유자 요청입니다.

## 절차 기록 (FACT)

1. 작업 메모의 "정지 부팅 결함 수정 구현" 계획 검증 → v1.0.0 시험·코드에서 이미 구현 확인 → 소유자 보고.
2. 실측 재확인: trace.1.jsonl 부팅별 행 수·첫 이벤트·최대 간격 계산(위 FACT 1).
3. 상수·히스테리시스 반영, 시험 갱신, 전체 시험.
4. 본 기록 작성. 날짜 2026-10-23.

## 후보 번들 (2026-10-24, FACT; 발행 안 함)

- ARM 빌드: 고정 도구체인(m3-toolchain, `.m3-toolchain/SOURCE_COMMIT` 핀 검증)으로 현재 HEAD(`b9f4f94b99e14b2b871cfefc024bd5c887a19280`, TAN 후보 + 본 G3 수정 모두 포함)를 `build/release-arm-g3-20261024`에서 새로 빌드 — **검증 통과**. (낡은 `release-arm-tan-20261023b`는 본 수정 이전이므로 쓰지 않았다.)
- 번들: `tools/make_usb_zip.py --default-mode BETA --tunnel-md5 56271d9c2d466d239ea81b840148fe6d`로 `/tmp/opencode/mazda-aa-dr-v1.1.0-beta.1-candidate-20261024.zip` 작성. SHA-256 `0f6066f9036436b815b11e0b4d2fcb80ed8f55797d3ea291cd91fe864b18408a`.
- 게이트: `tests/gates/g5g6_zipcheck.py --zip … --commit b9f4b99e14b2b871cfefc024bd5c887a19280 --sha256 … --mode BETA --tunnel-md5 56271d9c…` → **67/67 PASS**(`source_modified false`, `default_mode BETA`, `install_policy persistent`, 터널 md5 핀 포함). 파일명의 `v1.1.0-beta.1`은 후보 식별명이고 태그·발행은 아니다.
- 미실행이었던 것 → **2026-10-24 실제 실행으로 해소 (FACT, EXIT=0)**: `qemu-arm`/`proot`을 루트 권한 없이 확보(`apt-get download` qemu-user-static 8.2.2+ds-0ubuntu1.18 [정적 qemu-arm-static]·proot 5.1.0 .deb를 `/tmp/opencode/qemu-tools`에 풀고 `bin/qemu-arm` shim; 시스템 상태 변경 없음)하고 실제 순정 루트 `/tmp/jcidlg-clean/full`과 소유자 libpatch `/tmp/hookexp/B_new_libpatch/dp/oem-aa-mod/libpatch-blmjciaapa.so`로 3개 스크립트를 실행했다.
  1. `tests/adapter/run_aa_install_probe.sh`(현재 HEAD `libmx5dr.so`): **4/4 PASS** — 패치 없음(`install=ok`, 세션 관측 `observing`), libpatch 알려진 경로(위치 훅 설치 ok, 세션 `declined_third_party_interposer`, stage 3, owner 지목), 같은 라이브러리 낯선 경로(`next_chain_mismatch_or_lazy_binding`, fail-closed), BETA(모드 5, `session_fence=declined_send_storage_counter`, 워커 `ARMED`).
  2. `tests/adapter/run_lds_install.sh`(실제 순정 LDS 스택): message-caller·native-wrapper·generation-pointer·input-close/parser/select-prefix·input-parser/select/dispatch-call·lease-chain-change·preoccupied-bus **전부 PASS**.
  3. `tests/adapter/run_stock_unwind.sh`(실제 순정 로더·런타임 + 현재 HEAD 생산 DSO): 위치 스위트 18케이스, 순정 로더 정체성·AA unwind **PASS**.
  이로써 종전 기록의 "qemu-arm/proot 부재로 스킵" 사유가 사라졌다. 단, 이것은 빌드·QEMU 성공이며 차량 설치·복구 검증이 아니다.
- **ARM 전체 스위트(`tests/run_arm_all.sh`)는 오늘 qemu에서 판정 불가 (FACT)**: 시간 민감 클래스 `worker-beta-test g3_unadmitted`가 3/3 실패(같은 단언: 토큰 행 `reason=moved` `result`가 기대 `written` 대 실제 `sync`, `token_mid`이 이전 부팅 토큰)로 스위트가 종료 134. 추가 소구 8회(부하 0.8~1.9)와 taskset 2-3 코어 고정 3회 모두 실패. **판별 실험(FACT)**: 발행된 v1.0.0(`bc9e5f3`)을 같은 고정 도구체인·같은 방식으로 워크트리에서 재빌드해 같은 qemu로 돌리면 **동일 단언으로 3/3 실패**한다. 즉 오늘의 실패는 TAN·본 G3 수정 이전·이후 모두에서 재현되므로(이 환경에서는) HEAD의 무엇도 원인이 아니고, 같은 게시 바이너리가 10-19 발행 스위트에서는 `ARM_TIMING_RETRIES=0`으로 68회 전부 첫 시도 통과였다(V1_A §6: 시작 부하 3.71에서도). 호스트 `make test`는 HEAD·v1.0.0 모두 통과. INFERENCE: qemu-arm 8.2.2 최신포인트가 10-19 당시 PATH의 qemu와 스레드 실행 타이밍이 다르고, g3_unadmitted의 성립 운반(게으른 토큰 요청→기록 스레드 결과)이 그 경계 위에 있다(v1.0.0 지체 게이트: 기록 스레드 지연 100 ms 내 요청). 근거 계보: validation/ARM_TIMING_STABILISATION_2026-10-10.md(분류), validation/V1_C_GATES_2026-10-18.md(같은 시험 서명을 qemu 기록 스레드 굶주림으로 본 선례). **라벨**: 이 환경에서 ARM 전체 스위트는 통과 판정이 아니라 판정 불가이며, 어댑터 프로브 3건만 새 판다. 후보에 ARM 스위트 통과 주장을 하지 않는다.
