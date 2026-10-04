# 2026-10-04 실차 시험: 주행 중 CMU 자체 재부팅과 AA 후킹 미설치 — 2026-10-05

차량 시험 한 번(설치 `v0.3.12-shadow.3`)의 회수 자료를 분석한 기록이다. 위치 좌표 등 차량 정보는 싣지 않는다. 시계는 시험 부팅 후 초이며
순정 OEM 로그의 시계와 trace/collector의 단조 시계가 일치한다.

## 무엇이 있었나

- 설치(메뉴 1) → CMU 재부팅(메뉴 5) → 시험 부팅(boot `07987d33`). 약 12분 정차 뒤 약 776초부터 이동, 최고 약 60 km/h.
- 시험 부팅 약 1,459초에 **CMU가 스스로 재부팅**됐다(지하주차장 진입 직전). 사용자는 그 부팅에서 주차해 메뉴 3·4로 회수했다.
- trace는 크래시 표식 없이 1,459.2초에 끊겼다. 센서 기록은 쓸 만했다: 휠 약 13,900건, yaw 약 13,900건, 간격 중앙값 101.5 ms, 250 ms 넘는 공백 3회(66, 105, 816초 부근), 주행 전체가 하나의 연속 구간.

## 재부팅의 직접 원인(순정 SM 로그, 1,446–1,452초)

- 1,406초: `jciblmVdt`(VDM)가 SM heartbeat에 마지막으로 응답. 1,416.6초 이후 무응답.
- 1,446.67초: `jciblmVdt didn't respond in 30000 milliseconds` → SIGTERM으로 종료 실패.
- 1,451.67초: 5초 안에 정지되지 않음. 1,452.75초: `reset_board="yes"` 서비스가 종료돼 **watchdog 핑을 중단**("5초 안에 재부팅").
- 같은 시간대 우리 기록: collector가 3초마다 순정 `smdb-read`를 띄우고 250 ms 안에 끝나지 않으면 SIGKILL했다(이 부팅에서 poll 1,033건 중 175건 타임아웃). 1,405.69초가 마지막으로 세 값이 모두 정상인 poll이었고
  1,407.34초에 yaw·gear가 타임아웃으로 바뀐 뒤 **1,458.19초까지 자식 86개가 연속 타임아웃**(회복 없음). 우리 VBS 탭(10 Hz), LDS 응답, 로그 쓰기는 끝까지 정상이었다.

## 원인 후보와 증거 수준

가장 유력한 후보는 collector가 SMDB 잠금을 쥔 `smdb-read`를 SIGKILL해 잠금이 영구히 남는 것이다. 순정 `libjcismdb.so`는 DB마다 이름 있는 POSIX 세마포어(`/smdb_<db>`, 초기값 1)를 `sem_open`하고
`SMDB_Lock`은 타임아웃 없는 `sem_wait`(EINTR만 재시도, 그 밖의 오류는 abort), 소유자 복구가 없다. `smdb-read` 한 번에 잠금 구간이 Open/Read/Close로 세 번이다. VDM(`libjcivdm.so`)은 같은 DB에 `SMDB_WriteIntData`로
쓴다. SIGTERM이 듣지 않고 SIGKILL만 듣는 것도 EINTR 재시도 루프와 맞는다.

독립 분석 6건(Opus 4, Fable 2, 서로의 결론을 보지 않음)이 모두 이를 1순위로 꼽았다: 60%, 70%, 70%, 75%, 80%, 80%. 2순위는 순정 VDM 자체 결함(6~15%), 나머지 가설(CPU/메모리, 플래시, USB/동글, GPS 상실, 프리로드)은 각 5% 이하이고 GPS 상실(1,454초)은 VDM 정지(≤1,416초)보다 뒤라서 반증된다.
VDM 프로세스에는 우리 프리로드가 들어가지 않는다(sm.conf 차이는 jciVBS, jciAAPA, jciLDS의 `LD_PRELOAD` 세 줄뿐).

**입증되지 않았다.** 리셋 순간의 세마포어 값(`/dev/shm/sem.smdb_*`), VDM 스레드 상태, 그 시점 `/data/*.out`(SM 보고서)이 수집되지 않았다. "잠금을 쥔 자식이 죽었다"와 "VDM이 먼저 멈춰 잠금을 쥐고 있었다"는 현재 자료로 구분되지 않는다.
지난 시험(v0.3.9)에서도 주행 중 재부팅이 있었다는 정황이 같은 방향이지만 그 로그는 없다.

## 조치

- `9c98a2f`: collector가 SMDB를 읽지 않고 fork/exec/kill/wait를 하지 않는다. 정적 검사 테스트가 소스와 배포 바이너리를 지킨다(옛 collector에서는 실패). 호스트 collector 테스트 11개 통과.
- `9c98a2f`: 서비스 판정이 실제 형식(comm `L_jciXXX`, argv `-l jciXXX`)을 읽는다. 회수 도구가 세 서비스 정보를 수집한다.
- `4748f09`: 회수 도구가 SM의 리셋 직전 보고서(`/data`의 `thread_info.out`(커널 스택), `ps_info.out`, `top_info.out`, `meminfo.out`, `free_info.out`, `df_info.out`, `dmesg.out`)와 수정 시각을 수집한다.
- `5089b7c`: `libmx5dr-ldstap.so`와 `libmx5dr-vimtap.so`가 정적 libstdc++/libgcc 심볼을 노출하지 않는다(독립 분석이 찾은 잠재 위험. `jciLDS`는 `libjciusbmgr_client.so`, `libjcisystem_client.so`로 실제 libstdc++를 올린다). 리셋 원인으로 보지 않는다.
- 릴리즈 [v0.3.12-shadow.4](RELEASE_V0312_SHADOW4_2026-10-05.md). 이전 `v0.3.12-shadow.1~.3`과 `v0.3.11-shadow.2` 릴리즈 노트에 사용 중단 안내를 붙였다.

## AA 위치 후킹이 설치되지 않은 원인(`install=next_chain_mismatch_or_lazy_binding`, `hook_installed=false` 부팅 내내)

**지연 바인딩이 아니었다.** 이 결과 이름이 지연 바인딩을 연상시켜 처음에는 `LD_BIND_NOW` 누락으로 오해했으나 다음 사실이 배제한다.

- 우리 `dlopen` 가로채기(`src/runtime/loader.cpp:125`)가 BLM을 `RTLD_NOW`로 열도록 이미 강제한다. 순정 `sm_svclauncher`도 `dlopen(path, 2)`(0x25d54 `mov r1,#2`)이다. 부팅 행이 이 결과를 남겼다는 것은 그 경로가 실행됐다는 뜻이다.
- 실패한 비교는 위치 송신 슬롯(`0xf88bc`)이 아니라 세션 슬롯 두 개다. `session_plan`(`src/adapter/v74_install.cpp`)은 BLM의 `aap_destroy_session`(`0xf7f6c`)과 `aap_create_session`(`0xf8988`) 슬롯이 정품 `libaap_interface.so`를 가리켜야 통과시킨다.
- 사용자가 설치한 서드파티 `libpatch-blmjciaapa.so`(oem-aa-mod)는 **공개된 모든 버전(0.1.0~0.10.0 바이너리 12개)이 `aap_create_session`과 `aap_destroy_session`을 export**해 BLM의 PLT를 가로챈다. 어느 버전도 `aap_send_vehicle_data`를 export하지 않으며, 우리 위치 후킹 지점(`0xc7460`, `0xf88bc`, 인터페이스의 `0x1a538`)을 건드리지 않는다. 프리로드 순서(`libmx5dr.so:libpatch-blmjciaapa.so`)상 전역 검색에서 `libpatch`가 정품보다 앞서므로 슬롯이 `libpatch`로 묶인다. 차량에서 터치가 동작한다는 사실이 이 슬롯이 `libpatch`를 가리킨다는 정황 증거이다.
- 세션 슬롯 검사는 2026-09-30(`20bf583`)에 추가됐고, 그 뒤 `libpatch`와 같이 올린 시험이 한 번도 없었다. 그전의 `install=ok`는 위치 후킹 슬롯만 검사하던 때였고 `libpatch`가 그 슬롯을 건드리지 않아 통과했다.
- 같은 오류 문자열을 내는 검사가 일곱 곳이라 차량 trace만으로는 어느 슬롯인지 구분되지 않았다. 차량의 `libpatch` 파일 자체는 수집되지 않아 정확한 버전은 모른다(공개 버전은 모두 같은 심(shim)을 가진다).

독립 분석 4건(Opus 3, Fable 1)이 이 결론에 일치했다. 한 건은 처음에 `aap_send_vehicle_data`를 가로챈다고 추정했고(오류), 다른 한 건은 지연 바인딩을 지지했으나(전제인 `RTLD_NOW` 강제를 확인하지 않음) 실제 바이너리와 코드로 반박됐다.
세션 후킹을 그대로 `libpatch` 위에 얹으면 우리 코드가 정품 함수를 직접 호출해 `libpatch`의 shim을 건너뛰어 사용자의 터치·HUD·km/L 기능이 꺼질 수 있다(`session_hooks.cpp:172`). 그래서 설치기는 지금처럼 안전하게 거부했다. 수정 방향(세션 관측만 생략하고 위치 후킹은 설치)은 다음 릴리즈에서 QEMU로 검증한 뒤 넣는다.

## 한계

차량의 `/proc`/`/dev/shm` 스냅숏과 리셋 직전 SM 보고서가 없어 원인은 추론이다. 시험 부팅의 앞 12분 OEM 오류 로그는 회수 도구의 128 KiB 꼬리 제한으로 잘렸다. 지하 GPS 단절 구간은 리셋 때문에 약 5초만 기록됐다.
`position_poll`의 mode=1과 고정 좌표는 939초까지 LDS의 저장 위치이고 GPS 고정이 아니다(`utc_s>0`로 걸러야 한다). 이 기록은 설계 변경이나 live ASSIST 활성화, 차량 시험 승인을 뜻하지 않는다.
