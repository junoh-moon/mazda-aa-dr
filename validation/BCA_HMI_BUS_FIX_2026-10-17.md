# 수집기의 BCA 질의를 HMI 버스로 옮김 (2026-10-17)

대상: 1세대 Mazda Connect NA 74.00.324A, 휴대폰 IMU 채널 1단계(기록 전용) 수신기(`src/collector/phone_imu_rx.h`, `src/collector/collector.cpp`). **차량, 폰, 동글, DHU는 사용하지 않았습니다.** 이 기록은 차량 방문, 주행, 발행을 승인하지 않습니다. 제품이 네이버/AA로 보내거나 결정하는 값은 하나도 바뀌지 않습니다(수집기의 기록 전용 채널만 바뀝니다).

## 문제

- 차량 관측([PHONE_CMU_NO_PING_2026-10-17.md](PHONE_CMU_NO_PING_2026-10-17.md)): 두 부팅 모두 `pimu_link ev=poll_error why=error`만 있고 그 뒤 `pimu_link` 행이 없습니다. 수신기는 PTY를 열지 않았고 PING을 보내지 않았으며, 폰은 `no_ping`을 반복했습니다.
- 원인(정적 분석과 순정 라이브러리 오프라인 재현, [BCA_QUERY_CONNECT_STATUS_RE_2026-10-17.md](BCA_QUERY_CONNECT_STATUS_RE_2026-10-17.md)): 순정 BCA는 `com.jci.bca` 이름을 서비스 버스와 HMI 버스 양쪽에서 소유하지만, `/com/jci/bca` 객체(`QueryConnectStatus`)는 HMI 버스 연결에만 등록하고 `ConnectionStatusResp` 신호도 HMI 버스로만 보냅니다. 수집기는 서비스 버스(`/tmp/dbus_service_socket`)에 물었으므로 호출은 매번 `UnknownMethod`였고 신호는 한 번도 오지 않았습니다.
- 기록 공백: `poll_error` 행에 D-Bus 오류 이름이 없었고(실패 1·3회째만 기록), 응답의 serviceId가 15가 아니면 행 없이 버려졌습니다.

## 바꾼 것

1. `RxOptions`의 `bus_address`를 `bca_bus_address`로 바꿨습니다. 수신기의 private 연결 `conn_`은 BCA 관련 D-Bus(`GetNameOwner(com.jci.bca)`, `NameOwnerChanged`·`ConnectionStatusResp` 매치, `QueryConnectStatus`)에만 쓰이며 다른 용도는 없습니다(bds 감시는 `/proc`만 읽음). 그래서 이 연결 전체가 HMI 버스로 갑니다. `collector.cpp`의 기본값은 `unix:path=/tmp/dbus_hmi_socket`(순정 `hmi.conf` 주소)이고, 시험 빌드에만 `--bca-bus-address` 인수를 더했습니다. 수집기 자신의 위치 폴링은 주 스레드의 서비스 버스 연결(`bus_address`, `--bus-address`)을 그대로 씁니다.
2. `pimu_link ev=poll_error`와 `ev=bus why=unavailable` 행에 `name`(D-Bus 오류 이름, 64자 이하), `msg`(메시지 앞 80자 이하, `"`는 `'`, `\`는 `/`, 그 밖의 비인쇄 문자는 `_`), `n`(연속 실패 횟수, 9999까지 표시)을 넣었습니다. 기록 규칙은 "1·3회째"에서 "오류 이름이 바뀔 때마다 한 행, 같은 이름이면 10회째마다 한 행"으로 바꿨습니다(카운터는 999999에서 멈추며 10의 배수가 아니므로 행이 쏟아지지 않음). 오류 행은 자체 토큰 버킷(300 s마다 1, 묶음 4)을 쓰고, 버린 행은 `pimu_drop`의 R_LINK 칸에 셉니다(행 형식 불변). 최악 기록량은 383,486 B/h(이전 377,342 B/h)로 400 KB/h 상한 안입니다.
3. 질의 응답의 serviceId가 15가 아니면(예: 0 = BCA 표에 15가 없음) `pimu_link ev=poll why=other svc=<n> cs=<n>` 행을 (svc, cs)가 바뀔 때만, 부팅당 최대 4행 남기고, 그 PTY는 절대 열지 않습니다.
4. 타임아웃 350 ms와 CONNECTED(3) 판단은 그대로입니다. 질의 응답은 3 또는 6만 담고 CONNECTING(2)은 신호에서만 온다는 주석을 넣었습니다.
5. HMI 소켓이 없거나 거절하면 연결하지 않고(PTY 없음, PING 없음) 기존대로 10 s마다 다시 시도하며, 오류 이름이 담긴 `bus` 행은 위 규칙으로 한 번 남습니다.

bds `SignalConnected_cb` 구독(선택 진단)은 구현하지 않았습니다.

## 시험 구성

- `tests/collector/fake_bca.c`: `--no-object`(이름은 소유하지만 객체가 없어 libdbus 기본 처리가 `UnknownMethod`로 답하고 신호를 보내지 않음, 순정 BCA의 서비스 버스 연결 모양), `raw <svc> <cs> <st> <path>`(질의와 무관한 고정 응답, 순정의 표 없음 응답 0,6 재현), `error [이름 [메시지]]`를 더했습니다.
- `tests/collector/test_phone_imu_rx.py`와 상호운용 리그 `tests/phone_imu/interop/rig.py`: private dbus-daemon을 두 개(서비스 버스, HMI 버스) 띄웁니다. 서비스 버스에는 `--no-object` 가짜, HMI 버스에는 응답·신호 가짜를 둡니다. 수집기는 `--bus-address`(서비스)와 `--bca-bus-address`(HMI)를 따로 받습니다.
- 새 시험 6개: HMI 경로 폴링으로만 PTY와 PING(서비스 버스에는 질의 0건, 위치 폴링 계속), HMI 신호로 PTY와 PING, BCA를 서비스 버스로 가리키면(수정 전 배선) `UnknownMethod` 행과 PTY 없음, 오류 이름·메시지 정제와 80자 상한·서로 다른 이름의 두 번째 행, serviceId 0과 14 응답의 `other` 행(각 1행, PTY 없음), HMI 소켓 없음(한 개의 `bus` 행, 나중에 데몬이 뜨면 10 s 재시도로 연결해 PTY와 PING, 위치 폴링 행 10개 이상). `tests/collector/test_phone_imu.cpp`에는 오류 행 최장 길이(384 B 이하), 서명 있는 svc의 최장 행(200 B 이하), 오류 행 규칙(1, 10, 이름 변경, 포화 시 폭주 없음) 검사를 더했습니다.

## 증거 (DATA, 호스트·QEMU)

- 호스트 `make test-collector`(dbus 헤더는 로컬 추출본): `PASS test_phone_imu`, 수집기 저널 시험과 저장소 5개 시나리오 통과, Python `unittest discover -s tests/collector` **43개 OK**(이전 37개 + 새 6개, 건너뜀 0).
- 새 시험이 옛 코드에서 실패하는지(되돌림 증거):
  - HEAD 바이너리(수정 전 수집기와 가짜 BCA)에 새 시험 파일: 6개 모두 실패(`--bca-bus-address`를 몰라 수집기가 64로 종료).
  - 배선만 되돌림(새 수신기에 서비스 버스 주소를 넘김, 즉 수정 전 동작): 6개 중 **5개 실패**. 통과한 1개는 서비스 버스 동작 자체를 고정한 시험입니다.
  - 배선은 HMI, 수신기 헤더만 HEAD: HMI 경로 시험 2개는 통과하고 나머지 **4개 실패/오류**(서비스 버스 시험, 정제 시험, HMI 소켓 없음 시험은 `name` 필드가 없어 오류, `other` 행 시험은 실패).
- 실제 행 예(합성 시험): `{"ev":"poll_error","why":"error","name":"org.freedesktop.DBus.Error.UnknownMethod","msg":"Method 'QueryConnectStatus' with signature 'u' on interface 'com.jci.bca' doesn'","n":1}`, `{"ev":"bus","why":"unavailable","name":"org.freedesktop.DBus.Error.FileNotFound",...}`, `{"ev":"poll","why":"other","svc":0,"cs":6}`.
- 정확한 ARM(고정 m3 툴체인, GCC 4.9.1, DBus 1.6.30 sysroot): `make arm` 깨끗한 빌드 성공(경고 0), `mx5dr-collector`에 `unix:path=/tmp/dbus_hmi_socket` 문자열 포함. qemu-arm에서 수집기 저널 시험과 저장소 5개 시나리오 통과, ARM으로 빌드한 `test_phone_imu` 통과(budget_bph=383486). `MX5DR_RELEASE_BUNDLE`을 이 ARM 빌드로 준 `tests/packaging/test_collector_no_smdb.py` 3개 OK.
- 상호운용 리그(`make test-phone-imu-interop`, 실제 폰 Java 코어와 실제 수신기, 두 버스): 시나리오 7개(normal, firstping, firstping_poll, crash, flap, garbage, slow) **68 검사, 실패 0**(약 30분, 시작 시 1분 부하 0.72).

## 검증하지 않은 것

- **차량.** 실제 BCA가 HMI 버스에서 이 질의에 답하는지, 신호를 보내는지는 순정 라이브러리 오프라인 재현과 합성 시험에만 근거합니다. 다음 차량 기록의 `poll`/`poll_error` 행(오류 이름 포함)이 판정합니다.
- **실제 BCA 표의 서비스 15.** BCA가 우리 설정의 15번 행을 갖고 연결 시 상태 3과 PTY 경로를 채우는지는 미확인입니다. 표에 없으면 이제 `why=other svc=0 cs=6` 행으로 보입니다. NotAllowed 상태처럼 신호가 생략되는 경우도 미확인입니다.
- **HMI 소켓 권한.** 수집기는 `service` 계정으로 실행됩니다. 순정 `hmi.conf`는 모든 사용자와 익명을 허용하지만 차량의 `/tmp/dbus_hmi_socket` 파일 모드는 확인하지 않았습니다. 거절되면 오류 이름이 담긴 `bus` 행으로 보이고 수신기는 닫힌 채 남습니다.
- ~~HMI 데몬이 소켓은 있는데 응답하지 않으면 `dbus_bus_register`가 libdbus 기본 시간(약 25 s) 동안 수신기 스레드를 막을 수 있습니다.~~ **정정:** 이 서술은 틀렸습니다. 독립 검토([BCA_HMI_BUS_FIX_REVIEW_FABLE_2026-10-17.md](BCA_HMI_BUS_FIX_REVIEW_FABLE_2026-10-17.md) 1-A)가 재현한 대로, 소켓이 연결을 받고 SASL 인증에 답하지 않으면 `dbus_bus_register`는 **무기한**(측정 140 s 이상) 돌아오지 않고 그동안 PTY를 아무도 읽지 않습니다. 25 s는 Hello 호출의 기본 시간일 뿐 인증 단계에는 상한이 없습니다. 아래 "후속 수정"에서 고쳤습니다.
- ~~`tools/analyze_logs.py`는 새 `name`/`msg` 필드를 요약에 표시하지 않습니다.~~ 후속 수정에서 요약에 넣었습니다.
- QEMU/합성 시험 성공은 차량 검증이 아닙니다.

## 후속 수정 (2026-10-17): HMI 연결 시도에 상한을 둠

독립 검토 1-A의 지적을 고쳤습니다. **차량, 폰, 동글은 쓰지 않았습니다.** 기록 전용 수집기 수신기만 바뀌고, 제품이 네이버/AA로 보내거나 결정하는 값은 바뀌지 않습니다. 이 기록은 차량 방문, 병합, 발행을 승인하지 않습니다.

### 측정한 사실 (DATA)

- 정정 대상: 위 "약 25 s" 서술. 응답 없는 소켓(연결은 받고 아무것도 보내지 않음)에 대해 `dbus_bus_register()`는 돌아오지 않습니다. 검토자 측정(호스트 libdbus 1.14.10): 140 s 이상, 그동안 PING 없음, 마스터 쪽 쓰기 EAGAIN. 이번 측정: **순정 CMU libdbus 1.7.2**(qemu-arm, 순정 루트를 `-L`로 지정, 같은 함수를 부르는 작은 ARM 프로브)에서도 60 s 안에 돌아오지 않았습니다(60 s에 강제 종료). libdbus의 25 s 기본값은 Hello **메서드 호출**에만 적용되고 SASL 인증 단계에는 상한이 없습니다. SASL에는 답하고 Hello에 답하지 않는 상대에게는 같은 1.7.2 `dbus_bus_register`가 Hello마다 약 25 s를 막습니다(기본 시간 그대로, 수정 전 코드의 회귀 시험에서 PTY 쓰기가 막혀 실패).
- 수정 전 수신기(HEAD 헤더로 만든 `test_collector`)에 새 시험 2개를 돌리면 둘 다 **실패**합니다: 멈춘 소켓으로 바꾼 뒤 수신기가 PTY를 읽지 않아 폰 쪽 쓰기가 2 s 넘게 막힘(`master write blocked: the receiver stopped reading`).

### 바꾼 것

1. `bus_open()`의 `dbus_bus_register()`를 상한이 있는 순서로 바꿨습니다(`Receiver::bus_register`). `dbus_connection_open_private()` 뒤 `dbus_connection_read_write(conn, 50)`을 반복해 인증을 진행하고 `dbus_connection_get_is_authenticated()`가 참이 되면 멈춥니다. 단조 시계로 **2 s**가 지나면 연결을 닫고 오류 이름 `mx5dr.AuthTimeout`으로 기존 `bus` 행(`err_row_due`/`link_row`)을 남깁니다. 인증 중 연결이 끊기면 `mx5dr.AuthFailed`입니다. 인증 뒤 `org.freedesktop.DBus.Hello`를 `dbus_connection_send_with_reply_and_block(…, 1000, …)`로 직접 보내고, 돌려받은 고유 이름을 `dbus_bus_set_unique_name()`으로 저장합니다(`dbus_bus_register`가 안에서 하는 일과 같음). Hello 시간 초과는 libdbus의 `org.freedesktop.DBus.Error.NoReply`로 행에 남습니다. 어떤 실패든 연결을 닫고 해제하므로 반쯤 등록된 연결은 남지 않습니다.
2. 인증을 기다리는 50 ms 단계 사이마다 PTY를 비블로킹으로 읽고 PING·침묵 처리(`link_tick`)를 합니다(`service_link`). 그래서 인증 대기 2 s 동안에도 PTY 읽기와 PING이 멈추지 않습니다. 남은 블로킹은 Hello 1 s(그리고 기존 `refresh_owner` 350 ms)입니다.
3. `dbus_bus_add_match` 두 번에 `DBusError`를 넘기지 않습니다. 이때 libdbus는 AddMatch를 보내기만 하고 기다리지 않습니다(오류를 넘기면 Hello에는 답하고 그 뒤 조용해진 데몬에게 매번 25 s 기본값만큼 막힘). 결과는 원래도 쓰지 않았고, 응답은 `bus_tick`의 `pop_message`가 버립니다. 바로 뒤 `refresh_owner()`의 호출이 두 AddMatch를 먼저 내보내므로 NameOwnerChanged 매치가 소유자 조회보다 앞섭니다.
4. API: `dbus_connection_get_is_authenticated`, `dbus_connection_read_write`, `dbus_bus_set_unique_name`, `dbus_connection_send_with_reply_and_block`은 툴체인 sysroot의 DBus 1.6.30 헤더에 있고 순정 `libdbus-1.so.3.7.2`의 동적 심볼로도 있음을 확인했습니다(readelf).
5. `tools/analyze_logs.py`: `phone_imu` 절에 `dbus_errors`(ev·오류 이름별 행 수, 최대 `n`, 처음·마지막 시각, 메시지 앞부분)와 `other_services`(`poll why=other` 행의 svc/cs/시각)를 더했습니다. `why=other` 행은 이제 `links["poll"]`이 아니라 `links["poll_other"]`로 셉니다. 글 보고서에도 두 줄이 나옵니다.

연결 시도 한 번의 최악은 인증 2 s + Hello 1 s(+ 기존 GetNameOwner 350 ms)로 약 3.4 s이고, 그중 PTY를 읽지 않는 구간은 최대 약 1.35 s입니다. 재시도 주기는 그대로 10 s입니다.

### 증거 (DATA, 호스트·QEMU)

- 새 시험(`tests/collector/test_phone_imu_rx.py`): PTY가 열려 있고 폰이 50 Hz로 쓰는 중에 HMI 데몬을 끝내고 같은 경로에 (a) 연결만 받는 소켓, (b) SASL에는 답하고 Hello에는 답하지 않는 소켓을 둡니다. 32 s 동안 PING 간격 최대 7.5 s 미만, PING 5개 이상, 연결 시도 2회 이상이 각각 (a) 2.6 s, (b) 1.6 s 안에 소켓을 닫음, 오류 이름이 담긴 `bus` 행 정확히 1개((a) `mx5dr.AuthTimeout`, (b) `org.freedesktop.DBus.Error.NoReply`)를 확인하고, 진짜 데몬과 가짜 BCA가 같은 경로에 돌아오면 수신기가 다시 연결·등록해 새 신호로 PTY를 `replace`하고 PING을 보내는지 확인합니다. 측정값(전체 실행): (a) 시도 길이 2.01 s(10 s 간격 4회), (b) 1.01 s, 두 경우 모두 PING이 5.0 s 간격 그대로(최대 간격 5.02 s).
- 같은 순서를 옮긴 ARM 프로브를 순정 libdbus 1.7.2로 qemu-arm에서 실행: 진짜 dbus-daemon에는 고유 이름 `:1.0`으로 등록되고 이어지는 호출도 성공(0.034 s), 연결만 받는 소켓은 2.048 s에 `mx5dr.AuthTimeout`, Hello 무응답은 1.027 s에 `NoReply`. 이 프로브는 수집기 바이너리가 아니라 같은 호출 순서의 복사본입니다.
- 호스트 `make test-collector`(로컬 dbus 헤더): 종료 0, `PASS test_phone_imu`(budget_bph=383486, 불변), 수집기 저널 시험과 저장소 5개 시나리오 통과, Python `unittest discover -s tests/collector` **45개 OK**(43 + 새 2, 건너뜀 0, 405 s).
- `make test-tools`: **293개 OK, 건너뜀 6**(선택 의존성·개인 자료 없음: build/replay, livi 드라이버, 개인 주행 2종, 공개 ND CAN 로그, pyosmium). 새 분석기 시험 `test_bca_error_names_and_other_service_rows`는 수정 전 분석기에서 실패합니다.
- 정확한 ARM(고정 m3 툴체인 GCC 4.9.1, DBus 1.6.30 sysroot): `make arm` 깨끗한 빌드 종료 0, 경고 0, `mx5dr-collector`에 `mx5dr.AuthTimeout`과 `unix:path=/tmp/dbus_hmi_socket` 문자열 포함. qemu-arm에서 수집기 저널 시험과 저장소 5개 시나리오 통과, ARM `test_phone_imu` 통과(budget_bph=383486), `MX5DR_RELEASE_BUNDLE`을 이 빌드로 준 `tests/packaging/test_collector_no_smdb.py` 3개 OK.
- 상호운용 리그(`make test-phone-imu-interop`)는 이번에 돌리지 않았습니다(별도 실행 예정).

### 남은 위험 (검증하지 않음)

- **듣기만 하고 받지 않는 소켓.** 데몬의 주 반복이 통째로 멈추면 `accept`도 하지 않습니다. libdbus 1.7의 unix 연결은 블로킹 `connect()`이므로, 듣기 대기열이 차면 `dbus_connection_open_private()` 자체가 무기한 막힙니다. 호스트 측정(DATA): 대기열 1인 받지 않는 소켓에 대해 처음 두 시도는 즉시 연결되고 세 번째 시도는 8 s 동안 돌아오지 않았습니다(닫은 시도도 대기열에 남음). 차량에서는 순정 데몬의 대기열 크기(미확인)만큼 우리 10 s 재시도와 다른 HMI 클라이언트의 시도가 쌓인 뒤에야 생기는 일이지만, 이번 수정이 막지 못하는 경로입니다. 완화 후보(구현하지 않음): 연결 시도 전 수신기 스레드 대상 시간 제한 신호로 `connect`를 끊기, 연속 실패 시 재시도 간격 늘리기.
- **주 스레드의 서비스 버스 연결**(`collector.cpp` `connect_bus()`)도 같은 `dbus_bus_register()`를 씁니다. PTY와는 무관하지만 서비스 버스 데몬이 인증에서 멈추면 위치 폴링과 pimu 행 기록(주 스레드의 `drain`)이 멈춥니다. 수정 전부터 있던 구조이고 이번에 바꾸지 않았습니다.
- 인증 대기 중 `service_link`가 PTY 끊김을 `read`의 EIO로 먼저 보면 `disconnect` 행의 `why`가 `hangup` 대신 `read_error`가 됩니다(동작은 같음).
- 차량: 순정 HMI 데몬이 이 순서(직접 보낸 Hello)를 `dbus_bus_register`와 똑같이 받아들이는지는 순정 libdbus 1.7.2와 호스트 dbus-daemon 1.14.10의 조합으로만 확인했습니다. QEMU/합성 시험 성공은 차량 검증이 아닙니다.
