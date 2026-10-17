# 폰 → CMU 채널이 PING을 받지 못하는 원인 조사 (2026-10-17)

자료: 소유자가 같은 날 올린 폰 앱 로그(v6)와 USB 내보내기(beta.16, 메뉴 3의 CMU 기록: collector.*.jsonl, pimu.*.jsonl). 원본은 비공개 폴더에 있고 이 기록에는 주소를 쓰지 않았습니다.

## 관측 (FACT)
- 폰: 서비스 15에 RFCOMM 연결 성공(`connected`) → 15초 안에 PING 없음(`no_ping`, 폰이 만든 약 700프레임은 PING 전이라 버려짐)을 약 17~19초 간격으로 반복. 3회 실패에 폰 안전장치가 열리고(`retry_in_min=60`), 소유자가 '지금 다시 시도'를 눌러도 같은 결과로 다시 잠김. 소유자 보고: 5분 주행 중 계속 재연결을 묻는다.
- CMU: 두 부팅 모두 `pimu_boot svc=15`, `bt_watch config seen`, `bt_watch ready flag` 뒤 `pimu_link ev=poll_error why=error`(수집기가 BCA에 `QueryConnectStatus(15)`를 호출했는데 **D-Bus 오류 응답**)가 부팅 후 약 23.5초와 33.6초에 한 번씩(실패 1회째·3회째만 기록하는 규칙) 나오고, **그 뒤 `pimu_link` 행이 하나도 없습니다.** 성공한 호출은 `cs/st/path`가 처음 바뀔 때 반드시 행을 남기므로, 기록 기간 동안 **한 번도 성공하지 못했고 `ConnectionStatusResp` 신호도 한 번도 오지 않았다**는 뜻입니다. 수신기는 PTY를 열지 않았고 PING을 보내지 않았습니다.
- 오류의 이름·메시지는 기록되지 않습니다(기록 공백).

## 배제한 것 (FACT)
- D-Bus 정책: 순정 `service.d/jci_service.conf`가 모든 사용자에게 모든 목적지로의 호출을 허용(`send_destination="*"`)하고, 수집기는 같은 버스로 위치 서비스를 정상 호출 중(position_poll 2253행, 오류 13행). 접근 거부가 아닙니다.
- 설치·bds: 폰의 RFCOMM 연결이 받아들여졌으므로 bds의 서비스 15는 살아 있습니다.

## 메서드 정의 (FACT, 순정 `svcjcibca.so`의 내장 introspection)
`QueryConnectStatus`: 입력 `serviceId`(u), 출력 `connectionStatus`(`(iiii(ay))`), 주석 `async="client"`. 수집기는 `u` 하나를 보내고 350 ms 블로킹으로 기다립니다. `async=client`는 BCA가 바로 응답하지 않고 나중에 응답할 수 있음을 뜻합니다(이 점은 소스 주석 해석이며 BCA가 실제로 언제 응답하는지는 미확인).

## 가설 (INFERENCE, 아직 미확정)
1. 응답 지연: 350 ms 안에 응답이 오지 않아 `NoReply`(타임아웃)로 끝납니다. 이 경우 5초 간격 재시도가 매번 같은 결과가 됩니다.
2. BCA가 분류 코드가 없는 serialPort(서비스 15)를 이 메서드에서 오류로 거절합니다. 설계 기록(PHONE_IMU_FEASIBILITY_FABLE)은 BCA가 알려지지 않은 serialPort도 슬롯·ID·이름은 등록한다고 했지만 신호 발생 경로는 추적하지 않았고 '가능성이 높다'는 추정에 그쳤습니다.
3. 호출 형식 문제(UnknownMethod, 서명 불일치): 서명은 introspection과 일치하므로 가능성이 낮습니다.
확정하려면 (a) 오류 이름·메시지, (b) BCA의 `QueryConnectStatus` 처리기와 `ConnectionStatusResp` 발생 조건(정적 분석), (c) 서비스 15의 PTY 경로를 BCA 없이 얻는 대안이 필요합니다.

## 상태
`CONNECTED=3`을 포함한 BCA 경로 가정은 릴리스 노트의 미검증 항목이었고, 이 자료는 그 가정이 이 차에서 성립하지 않았음을 보입니다(수신기 쪽이 PTY를 얻지 못함). 제품(네이버로 가는 값)에는 영향이 없고 기록 전용 채널의 문제입니다.
