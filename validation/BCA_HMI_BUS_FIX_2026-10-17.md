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
- HMI 데몬이 소켓은 있는데 응답하지 않으면 `dbus_bus_register`가 libdbus 기본 시간(약 25 s) 동안 수신기 스레드를 막을 수 있습니다(주 스레드의 위치 폴링과 기록은 별도 스레드라 영향 없음). 이 위험은 서비스 버스 때도 같았습니다.
- `tools/analyze_logs.py`는 새 `name`/`msg` 필드를 요약에 표시하지 않습니다(행은 그대로 읽힘).
- QEMU/합성 시험 성공은 차량 검증이 아닙니다.
