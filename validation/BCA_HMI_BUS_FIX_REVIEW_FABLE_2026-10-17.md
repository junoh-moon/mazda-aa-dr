# 독립 적대적 검토: 수집기의 BCA 질의를 HMI 버스로 옮긴 커밋 (2026-10-17, Fable)

대상 커밋: `73e40864d5f38928b720181637e625aa4371e597` "fix(collector): ask the stock BCA on the HMI bus". 검토 범위는 `src/collector/collector.cpp`, `src/collector/phone_imu_rx.h`와 함께 바뀐 시험 5개 파일입니다. 배경 기록 [PHONE_CMU_NO_PING_2026-10-17.md](PHONE_CMU_NO_PING_2026-10-17.md), [BCA_QUERY_CONNECT_STATUS_RE_2026-10-17.md](BCA_QUERY_CONNECT_STATUS_RE_2026-10-17.md), [BCA_HMI_BUS_FIX_2026-10-17.md](BCA_HMI_BUS_FIX_2026-10-17.md)를 먼저 읽었습니다. **차량, 폰, 동글은 쓰지 않았습니다.** 모든 실행은 호스트 스크래치 사본(`git clone`, dbus 헤더는 로컬 추출본, dbus-daemon 1.14.10, libdbus 1.14.10)에서 했고 원본 저장소는 읽기만 했습니다. 이 기록은 차량 방문, 발행, 병합을 승인하지 않습니다.

## 요약

- **CONFIRMED(오프라인 재현) 1건:** HMI 소켓이 연결은 받지만 응답하지 않으면(인증 단계에서 멈춘 데몬) 수신기 스레드가 `dbus_bus_register`에서 **25 s가 아니라 무기한(측정 140 s 이상)** 막히고, 그동안 열려 있는 PTY를 읽지 않습니다(마스터 쪽 쓰기가 EAGAIN으로 막힘 = 차량에서는 순정 bds의 쓰기가 막히는 자리). 수정 전 서비스 버스 배선에도 같은 구조가 있었으므로 이 커밋이 **새로 만든 결함은 아니지만**, 커밋 메시지와 기록의 "약 25 s" 서술은 틀렸고, 이제 수집기의 PTY 드레인이 **두 번째 데몬(HMI)의 건강에도** 묶였습니다. 상세는 1-A.
- 그 밖에 코드에서 실제 결함(잘못된 PTY 열기, 경로 손상, 버퍼 넘침, 행 예산 초과, fd 누수, 재연결 실패)은 **찾지 못했습니다.** 위조 신호 방어, fail-closed, 재연결, 행 길이와 예산, 패키징은 모두 확인했습니다(아래 각 절의 "찾지 못함" 항목).
- 시험 돌연변이 3종 모두 새 시험이 잡습니다(4절). 추가로 제가 만든 두 시나리오(HMI 데몬 재시작, 멈춘 HMI 소켓)의 결과를 1-A와 3절에 적었습니다.

## 1. 차량 안전: 순정 HMI 버스에 두 번째 D-Bus 연결

### 1-A. CONFIRMED: 응답 없는 HMI 소켓은 수신기 스레드를 무기한 막고 PTY 드레인을 멈춥니다

- 코드: `phone_imu_rx.h:454-476` `bus_open()`은 `bus_tick()`(`:550`)에서 매 틱 불립니다. 연결이 끊겨 있으면 `dbus_connection_open_private()` 뒤 `dbus_bus_register()`(`:461`)를 **수신기 스레드에서 동기적으로** 부릅니다. 이 스레드가 PTY를 읽는 유일한 스레드입니다(`run()` `:776`, 헤더 주석 "ALWAYS reads (bds blocks when its PTY is not drained)").
- 재현(DATA, 호스트): 수신기가 PTY를 열고 PING을 보내는 상태에서 HMI dbus-daemon을 끝내고 같은 경로에 **accept만 하고 아무것도 보내지 않는** unix 소켓을 두었습니다(스크래치 사본의 `tests/collector/test_rev_extra.py`, 저장소에 넣지 않음). 결과: PING은 멈춤 시점 뒤 2.9 s, 7.9 s에 두 번만 보이고 **150 s 동안 더는 없음**, 소켓이 받은 연결은 9.1 s에 **단 한 번**, 수신기 스레드의 wchan은 15 s 간격 표본 9개 모두 `do_poll`, 마스터 쪽 프레임 쓰기 EAGAIN 6322회(버퍼가 차서 막힘). 즉 `dbus_bus_register` 한 번이 140 s 넘게 돌아오지 않았습니다. 주 스레드(`hrtimer_nanosleep`)는 영향이 없었습니다(커밋의 "position polling unaffected"는 맞음).
- 왜 25 s가 아닌가(INFERENCE, libdbus 1.14.10 관측): 기본 25 s는 Hello **메서드 호출**의 pending-call 시간이고, 그 앞의 **SASL 인증 단계**는 이 시간에 묶이지 않습니다. 서버가 인증에 답하지 않으면 블로킹 반복이 끝나지 않습니다. CMU의 libdbus 1.7.2에서 다르다는 근거는 없습니다(미확인).
- 차량에서의 의미: 순정 HMI dbus-daemon이 소켓은 열어 두고 인증에 답하지 못하는 상태(멈춤, 과부하)는 드물지만, 그때 수집기는 (1) PTY를 영원히 읽지 않아 순정 bds가 우리 서비스 15의 PTY 쓰기에서 막힐 수 있고(bds가 정말 막히는지는 헤더 주석의 전제이며 차량 미확인), (2) `quiesce`/`stop`도 수신기 스레드가 돌아와야 실행되므로 `collector.stop`과 SIGTERM에도 수신기는 끝나지 않습니다(`stop()`의 `pthread_join` `:201`이 주 스레드를 막음 → 수집기 종료도 막힘). 수정 전에는 서비스 버스 데몬만 이 위험을 들고 있었고, 이제 HMI 데몬이 더해졌습니다. 발생 가능성은 낮지만 결과가 순정 BT 스택에 닿으므로 적습니다.
- 가능한 완화(검토자 제안, 구현·검증하지 않음): PTY가 열려 있는 동안(`fd_ >= 0`)에는 재연결을 시도하지 않거나, `dbus_bus_register` 대신 `dbus_connection_read_write(conn, 0)`로 틱마다 비블로킹으로 인증을 진행해 `dbus_connection_get_is_authenticated()`가 참이 된 뒤 Hello를 350 ms 상한으로 보내고 `dbus_bus_set_unique_name()`을 쓰는 방법, 또는 수신기 측 마감(예: 2 s)을 넘기면 연결을 포기하는 방법이 있습니다. 어느 쪽이든 서비스 버스 시절부터 있던 구조라 이 커밋을 되돌릴 이유는 아닙니다.
- 기록 수정 필요: `BCA_HMI_BUS_FIX_2026-10-17.md` "검증하지 않은 것"의 "약 25 s" 문장과 커밋 메시지의 같은 문장은 위 측정으로 틀렸습니다. 또 `pimu.jsonl`에는 이 상태를 알리는 행이 없습니다(`bus` 행은 실패가 **돌아와야** 쓰임).

### 1-B. 찾지 못함 (확인한 것)

- **보내는 것:** HMI 버스로 나가는 메시지는 Hello(`dbus_bus_register`), AddMatch 2건(`:470, :472`), `GetNameOwner`(`:477-493`), `QueryConnectStatus(u 15)`(`:585-619`)뿐입니다. 두 호출 모두 `dbus_message_set_auto_start(m, 0)`이라 서비스 활성화를 일으키지 않습니다. RemoveMatch는 없지만 연결이 닫히면 데몬이 규칙을 지웁니다. 순정 상태를 바꾸는 호출은 없습니다.
- **정책(FACT, 순정 `/etc/dbus-1/hmi.conf`, `hmi.d/jci_hmi.conf` 읽기 전용 확인):** HMI 버스는 `<type>system</type>`, 사용자 `hmi`, `<allow_anonymous/>`, `max_connections_per_user` 100000이며, `hmi.d/jci_hmi.conf`가 서비스 버스의 `jci_service.conf`와 **글자까지 같은** 정책(`allow user="*"`, `own="*"`, `send_destination="*"`, `eavesdrop="true"`)을 줍니다. 따라서 `service` 계정의 수집기가 `com.jci.bca`를 호출하는 것은 정책상 허용됩니다. 소켓 파일 권한: libdbus는 unix 경로 소켓을 만든 뒤 `chmod 0777`을 시도하므로 서비스 소켓과 같은 방식입니다(INFERENCE, 차량 미확인은 기록대로 남김). HMI 버스는 `tcp:port=3001,bind=*`도 듣고 있어(순정) 우리 연결이 더하는 노출은 없습니다.
- **연결 한도:** 연결은 하나를 유지하고, 실패 시 `dbus_connection_close`+`unref`로 즉시 정리합니다(`:461`, `:453`). 10 s마다 실패를 반복해도 fd와 메모리는 늘지 않습니다(open 실패는 NULL 반환, register 실패는 닫음). `DBusError`는 두 경로 모두 해제됩니다(`:475`, `:618`).
- **종료 시 exit:** `dbus_connection_open_private`의 기본 `exit_on_disconnect`는 거짓이고(`:469`에서 다시 0), 수신기는 `dispatch`가 아니라 `pop_message`를 쓰므로 연결 끊김으로 프로세스가 종료될 길이 없습니다.
- **도청:** 매치 규칙 둘 다 `eavesdrop`이 없고 인터페이스·멤버(·sender·arg0)로 좁혀져 있습니다. 데몬 정책이 도청을 허용해도 요청하지 않았습니다.
- **신호 폭주·메모리:** HMI 버스에서는 이제 다른 서비스(HFP, A2DP 등)의 `ConnectionStatusResp`가 실제로 옵니다. `svc != 15`는 `sig_other_`만 올리고 돌아갑니다(`:520-534`). 틱당 64개 팝(`:555`)이라 1,280 msg/s까지 소화하며 BCA가 그 이상 보낼 이유는 없습니다. `NameOwnerChanged`는 `arg0='com.jci.bca'`로 좁혀 있어 350 ms 블로킹 `refresh_owner`가 폭주할 길은 BCA 이름이 요동칠 때뿐입니다.
- **스레드 안전:** `collector.cpp:395` `dbus_threads_init_default()`가 어떤 D-Bus 호출보다 앞서 있으므로 주 스레드(서비스 버스)와 수신기 스레드(HMI 버스)가 libdbus 전역(메시지 캐시 등)을 같이 써도 됩니다(수정 전과 같음).
- **Fail-closed:** `bt_active()`(`:551`)가 거짓이면(순정 인자 bds, quiesce, not_ready) HMI 버스에 연결조차 하지 않습니다. 소켓이 없거나 거절하면 `conn_ == 0`이라 `bus_tick`이 돌아가며 질의도 PTY도 없습니다. `quiesce()`는 `bus_close()`를 부르고 이후 재연결하지 않습니다. 다른 서비스 id의 응답·신호는 어떤 경로로도 `open_link`에 이르지 못합니다(`:520-534` 조기 반환).
- **위조 신호:** `ConnectionStatusResp`는 `dbus_message_get_sender`를 `GetNameOwner(com.jci.bca)`의 결과(`owner_`)와 비교합니다(`:560-562`). `GetNameOwner` 응답은 버스 드라이버가 serial로 짝지어 돌려주므로 다른 클라이언트가 가로챌 수 없습니다. HMI 버스에서 `com.jci.bca`를 가질 수 있는 것은 BCA(`DO_NOT_QUEUE`, 1차 소유자)뿐이므로 다른 HMI 클라이언트의 유니캐스트 위조 신호는 `unexpected_`로 셉니다. 그래도 통과한 경로는 `pts_path()`(`/dev/pts/<1~4자리 숫자>`)와 `open_slave()`의 문자 장치 major 136~143·uid 0 검사(`:627-641`)를 지나야 하므로 "임의 `/dev/pts` 경로"는 열 수 없고, 열 수 있는 최악은 root 소유의 다른 PTY 슬레이브입니다(수정 전과 같은 한계, 이 커밋이 바꾼 것 없음). 시험 `test_signal_from_another_sender_is_ignored`가 이제 HMI 버스에서 이 경우를 덮습니다.
- **인증:** `hmi.conf`에 `<auth>` 제한이 없어 EXTERNAL(uid `service`)로 통과합니다. 익명 허용도 켜져 있습니다.

## 2. 폴링 흐름

- **응답 구문 분석 `(iiii(ay))`(`:600-610`):** 구조체 안 int32 4개를 확인하며 읽고 `read_path()`가 `(ay)`를 첫 NUL까지 최대 31바이트로 복사합니다(`:495-507`). 잘못된 형태는 `ok=false` → `poll_error why=signature`. 찾지 못함.
- **'다른 서비스 id' 분기(`:520-534`):** `saved[32]`와 `path_[32]`가 같은 크기이고 `memcpy` 왕복으로 복원됩니다. 이 분기는 `fd_ < 0`일 때(질의는 PTY가 닫혔을 때만, `:570`)만 들어오고 `close_link`가 `path_`를 비우므로 실제로는 빈 경로를 저장·복원합니다. `path_`가 빈 채 남거나 묵은 경로가 쓰이는 경우는 없습니다. (svc, cs)가 바뀔 때만, 부팅당 4행. `st`는 가공 없이 넘깁니다. 찾지 못함.
- **`err_row_due()`(`:96-101`):** `last`는 `ERR_NAME+1`(65)바이트, `strncpy(…, 64)` 뒤 `last[64]=0`, 비교는 64바이트까지입니다. 카운터는 999999에서 멈추고(10의 배수가 아님) 그 뒤에는 이름이 바뀔 때만 행이 납니다. 이름이 A/B로 번갈아 바뀌는 최악에도 행은 5 s당 1건이 되지만 오류 버킷(300 s당 1, 묶음 4, `:83`)이 막고 초과분은 `rate_drop_[R_LINK]`에 셉니다. 성공 시 `query_fail_=0`, `last_query_err_[0]=0`(`:609`)이라 다음 실패는 "새 이름"으로 n=1 행이 납니다. 찾지 못함.
- **`link_row()`(`:260-275`):** `nm[65]`, `ms[81]`, `extra[192]`(최대 173바이트 사용), `row[768]`. `clean()`은 `"`·`\`·비인쇄를 `_`로, `clean_msg()`는 `"`→`'`, `\`→`/`, 비인쇄→`_`로 바꾸므로 D-Bus가 허용하는 어떤 UTF-8 메시지도 JSON을 깨지 못합니다(비ASCII는 모두 `_`). 최장 행을 손으로 세면 오류 행 321바이트(svc/cs/st를 20자리로 가정해도 375) ≤ 384, `other` 행 140 ≤ 200, 일반 pimu_link 187 ≤ 200입니다. `test_phone_imu.cpp`도 극값으로 같은 것을 확인합니다. `name`이 있으면 `emit()` 대신 오류 버킷 `take()` 뒤 `push()`, 실패 시 `rate_drop_[R_LINK]` 증가: `pimu_drop` 스키마 불변. 찾지 못함.
- **예산:** `budget_bph()` = 이전 377,342 + 오류 버킷 (12+4)행/h × 384 = 6,144 → **383,486 B/h**(호스트 `test_phone_imu` 출력 `budget_bph=383486`으로 확인) < 400 KB/h. `other` 행은 R_LINK 버킷 안에서 나갑니다. 찾지 못함.
- **CS 해석:** 질의 응답은 3/6만 온다는 RE 사실과 코드 주석이 맞고, 신호의 2(CONNECTING)는 기존 처리 그대로입니다.

## 3. 연결·재연결

- **데몬이 늦게 뜨는 경우(부팅 순서):** 소켓 없음 → `FileNotFound`(또는 연결 거절 `NoServer`) `bus` 행 n=1, 이후 n=10, 20, …(100 s마다, 오류 버킷이 다시 300 s당 1건으로 제한). 10 s마다 재시도, 연결되면 `bus_fail_=0`, `last_bus_err_[0]=0`, 매치 2건 재등록, `refresh_owner()`, `next_query_`는 지나 있으므로 즉시 질의. 시험 `test_hmi_socket_missing_fails_closed_and_retries`가 덮습니다. 찾지 못함.
- **HMI 데몬 재시작(DATA, 제 시나리오):** PTY가 열린 상태에서 HMI dbus-daemon을 SIGTERM으로 끝내고 1 s 뒤 같은 경로에 새 데몬과 새 가짜 BCA를 띄우고 새 PTY 경로의 신호를 보냈더니 **5.5 s 만에 `replace`와 PING**이 나왔습니다. `bus` 행은 0건이었는데, 끊김 뒤 첫 재시도가 이전 시도의 `next_bus_`(+10 s, `:458`)까지 기다렸다가 성공했기 때문입니다. 즉 끊김 뒤 재연결은 최대 10 s 늦을 수 있고(설계 범위), 매치와 `owner_`는 새 연결에서 다시 세워집니다. `dbus_connection_get_is_connected()`는 `dbus_connection_read_write(conn_, 0)`(`:554`)가 EOF를 본 뒤 거짓이 됩니다. 찾지 못함.
- **`opened_once_`·질의 주기 배가:** 이 커밋이 건드리지 않았고 재연결과 독립입니다(`close_link`가 5 s로 되돌림). 찾지 못함.
- **1-A의 예외:** 끊긴 뒤 소켓은 있는데 데몬이 인증에 답하지 않는 경우만 무기한 블로킹입니다.

## 4. 시험

스크래치 사본에서 `build/test_collector`(testing 빌드)와 `fake_bca`를 로컬 dbus 헤더로 빌드하고 `tests/collector/test_phone_imu_rx.py` 28개를 돌렸습니다: **28 OK(284.8 s)**. 그 뒤 돌연변이 3종을 적용해 새 시험 6개 + `test_signal_from_another_sender_is_ignored`(7개)를 돌렸습니다.

| 돌연변이 | 내용 | 결과 |
|---|---|---|
| M1 | `collector.cpp:436`에서 수신기에 `bus_address`(서비스 버스)를 넘김 = 수정 전 배선 | 7개 중 **6개 FAIL**, `test_service_bus_bca_gives_unknown_method_and_never_opens`만 통과(서비스 버스 동작 자체를 고정한 시험이라 예상대로). 커밋의 "5 of 6" 주장과 일치하고, `another_sender`도 추가로 실패. |
| M2 | `query()`의 실패 행에서 `name/msg/n`을 빼고 옛 `link_row` 호출로 되돌림 | `test_service_bus_bca_…`와 `test_poll_error_rows_carry_…` **ERROR(KeyError 'name')**, 나머지 5개 통과(`hmi_socket_missing`은 `bus` 행의 name을 그대로 둬서 통과). 잡힘. |
| M3 | `handle_status`의 `svc != SERVICE_ID` 분기 끝 `return;` 제거(다른 서비스의 CONNECTED 응답이 PTY를 염) | `test_reply_for_another_service_is_logged_once_and_never_opened`와 `test_signal_from_another_sender_is_ignored` **FAIL**, 나머지 통과. 잡힘. |

- 덮는 범위: 소유자 위조(HMI 버스의 `--no-name` 발신자)와 적대 문자열(`"`, `\`, 120자 초과)은 기존·새 시험이 덮습니다. 비ASCII/제어문자 메시지와 64자 넘는 오류 이름은 Python 시험에 없고 C++ 단위 시험(`err_row_due` 200자 이름)과 코드 읽기로만 확인했습니다(작은 공백, 위험 낮음).
- 취약성(flakiness) 검토: 시간 여유는 모두 넉넉합니다(`bus` 행 1건 단언은 다음 행이 100 s 뒤, `poll_error` 1건 단언은 50 s 뒤, 늦은 데몬 연결 25 s 한도 대 최대 ~11 s). 한 번의 전체 실행과 세 번의 부분 실행에서 흔들림은 없었습니다. 참고: `test_rev_extra.py`를 서브클래스로 만든 탓에 기본 시험 28개가 한 번 더 돌았고 그것도 모두 통과했습니다(총 58개, 1개는 제 시험의 오타로 오류 → 고친 뒤 통과).
- 상호운용 리그(`make test-phone-imu-interop`)는 지시대로 돌리지 않았습니다.

## 5. ARM·패키징

- 릴리스 바이너리 인자: `--bca-bus-address`는 `#ifdef MX5_COLLECTOR_TESTING` 안(`collector.cpp:359`)이라 차량 바이너리의 인자 집합은 바뀌지 않았고, `packaging/start_collector.sh`는 `--session-seconds`만 넘깁니다. 기본값 `unix:path=/tmp/dbus_hmi_socket`은 순정 `hmi.conf`의 `<listen>`과 같습니다.
- `bus_address`/`bus-address`를 쓰는 곳(grep, src·tests·packaging·tools·docs): `tests/collector/test_collector.py:35`, `tests/packaging/test_trial_status.py:822`는 `--bus-address`만 넘기고 `bt/BdsConfiguration.xml`이 없어 수신기가 뜨지 않으므로 영향 없음. `tests/packaging/oem_guest_init.sh:274`는 dbus-send 진단용. `tests/adapter/`에는 없음. **바꿔야 할 스크립트는 없습니다.**
- 주의(PLAUSIBLE, 시험 위생): 수신기가 뜨는 호스트 시험에서 `--bca-bus-address`를 빠뜨리면 개발 호스트의 실제 `/tmp/dbus_hmi_socket`에 붙습니다. 현재 저장소의 수신기 시험은 모두 넘기고 있고, 이 호스트에는 그 소켓이 없습니다(RE 기록의 재현처럼 호스트에 두 데몬을 띄워 두었다면 생길 수 있었음).
- `tools/analyze_logs.py:4383-4386`은 `poll_error`/`bus` 행을 `ev:why`로만 세고 `name`은 보여 주지 않습니다(커밋이 밝힌 한계). `why=other` 행은 `links["poll"]`에 섞여 들어가 **다음 차량 기록에서 "표에 15 없음"을 요약에서 못 봅니다**: 원시 행을 직접 봐야 합니다(작은 공백, 분석기만의 문제).
- ARM 빌드·qemu 실행은 다시 하지 않았습니다(커밋 기록 인용에 그침).

## 6. 그 밖의 관찰 (결함 아님)

- 수신기는 `pop_message`만 쓰므로 HMI 버스의 다른 클라이언트가 우리 고유 이름으로 `Peer.Ping`/`Introspect`를 보내면 답이 없어 25 s 시간 초과를 봅니다. 순정 HMI가 그런 열거를 한다는 근거는 없고 서비스 버스 시절과 같습니다.
- `other` 행은 `via == "poll"`만 기록하고 신호 쪽 다른 서비스는 `sig_other_` 카운터뿐입니다. 의도된 설계입니다.

## 판정

- 이 커밋이 고치려는 문제(잘못된 버스)의 수정 자체는 정확하고, 새 코드에서 잘못된 PTY를 열거나 순정 버스 상태를 바꾸는 길은 찾지 못했습니다.
- 병합 전 권장: (1) 1-A의 "25 s" 서술을 고치고 무기한 블로킹을 알려진 위험으로 적을 것, 가능하면 PTY가 열린 동안의 재연결 시도를 비블로킹 또는 금지로 바꿀 것(별도 커밋·시험); (2) `analyze_logs.py`에 `other`와 `name` 요약을 더할 것. 둘 다 이 커밋을 막을 이유는 아니며, 차량 검증(실제 BCA의 HMI 응답·신호, 서비스 15의 표 존재)은 기록대로 미완입니다.

## 실행 증거 (호스트, 스크래치 사본)

- `build/test_phone_imu`: `PASS`, `budget_bph=383486`.
- `test_phone_imu_rx.py` 기준선: `Ran 28 tests in 284.755s OK`.
- 돌연변이: M1 6 FAIL/1 ok, M2 2 ERROR/5 ok, M3 2 FAIL/5 ok (각 7개).
- 추가 시나리오: 데몬 재시작 → `replace`까지 5.5 s, `bus` 행 0; 멈춘 소켓 → PING 2.9 s, 7.9 s 뒤 150 s 동안 없음, accept 9.1 s 1회, 수신기 스레드 wchan `do_poll` 9/9 표본, 마스터 EAGAIN 6322.
- 남긴 프로세스 없음(검토 뒤 `dbus-daemon`은 데스크톱 세션 2개만, `test_collector`/`fake_bca` 0).
