# Android Auto DHU / 네이버 지도 - 정지 좌표 재전송 실험 2

**실험일:** 2026-10-03 (KST)  
**일차 자료:** [evidence.json](evidence.json) (전체 명령 원문, 관찰 분류, 스크린샷 시각 포함)  
**범위:** 삼성 Galaxy S25 + Android Auto + DHU 2.0 + 네이버 지도

## 질문과 결론

길안내가 켜진 네이버 지도에서 같은 차량 GPS 좌표를 초당 약 1회 반복할 때, 정확도와 속도 조합에 따라 표시가 어떻게 달라지는지 비교했습니다.

| 시험 | 정지 구간에 보낸 데이터 | 분류 | 화면에서 확인한 변화 |
| --- | --- | --- | --- |
| 1 | 같은 좌표, 정확도 `NAN`, 속도 14m/s, 방위각 126.7°, 60회 | **GPS-LOST** | 0초에는 파란 화살표. 10초 사진까지 회색 화살표와 위성-X / `GPS 탐색 중` 표시가 나타남. 위치와 남은 거리 약 2.3km는 이후 유지 |
| 2 | 같은 좌표, 정확도 5m, 속도 14m/s, 방위각 동일, 60회 | **FOLLOWS-AND-HOLDS** | 60초까지 파란 화살표 유지. 표시 속도 약 50km/h. 위치와 남은 거리 약 2.3km 유지 |
| 3 | 60초간 위치 명령 없음 | **GPS-LOST** | 10초 사진까지 회색 화살표와 위성-X / `GPS 탐색 중` 표시가 나타남. 위치와 남은 거리 약 2.3km 유지 |
| 4 | 같은 좌표, 정확도 `NAN`, 속도 0m/s, 방위각 동일, 60회 | **FOLLOWS-AND-HOLDS** | 60초까지 파란 화살표 유지. 10초 사진부터 표시 속도 0km/h. 위치와 남은 거리 약 2.3km 유지 |

GPS 손실 표시가 처음 나타난 정확한 초는 알 수 없습니다. 0초와 10초 사이에 바뀌었으므로 **(0, 10]초**로 기록합니다. 어떤 시험에서도 정지 구간 동안 경로를 따라 지속적으로 전진하거나 남은 거리가 계속 감소하는 모습은 관찰되지 않았습니다. 정지 구간 시작 직후 2.4km에서 2.3km로 바뀐 것은 마지막 주입 위치가 화면에 반영되는 과정으로 보이며, 그 뒤 값은 유지됐습니다.

### 가설 판정

- **H1: 정확도가 없으면 항상 버린다 - 지지되지 않음.** 시험 4에서는 정확도 `NAN`으로도 60초간 파란 위치 화살표가 유지됐습니다. 시험 1과 2의 차이만 보면 정확도가 중요해 보이지만, 시험 4가 단독 원인이라는 해석을 막습니다.
- **H2: 같은 위치의 반복 자체를 오래된 위치로 본다 - 지지되지 않음.** 시험 2와 4에서 같은 좌표를 반복해도 파란 화살표가 유지됐습니다.
- **H3: 오래된 비영 속도가 길안내 중 경로 외삽을 일으킨다 - 이 DHU 실험에서 지지되지 않음.** 시험 1은 길안내와 14m/s 속도가 모두 있었으나 GPS 손실 표시가 나타났고, 위치가 경로를 따라 지속적으로 전진하지 않았습니다.

**관찰된 조합:** `정확도 NAN + 속도 14m/s + 정지 좌표`는 GPS 손실 표시를, `정확도 5m + 속도 14m/s`와 `정확도 NAN + 속도 0m/s`는 위치 유지 표시를 만들었습니다. 이 패턴만으로 네이버 내부의 단일 검증 규칙을 특정할 수는 없습니다.

## 재현 조건과 명령

- Mac: macOS 15.8, Apple Silicon. ADB 37.0.1, DHU `2.0-mac-arm64`.
- 휴대전화: Galaxy S25 (`SM-S931N`), USB ADB 연결. Android Auto 헤드 유닛 서버 실행. 위치 권한은 유지했습니다.
- 휴대전화는 실내에 있었습니다. 사용자는 카카오 지도에서 건물 근처 위치가 보였다고 했습니다. 위성 GPS 신호 세기는 측정하지 않았습니다.
- 네이버 지도에서 **미포항** 길안내를 시작했고, 각 유효 시험의 시작 화면에 부산 약 2.6km 경로, 경로선, 남은 시간·거리가 표시됐습니다.
- 시작 구간은 [OpenStreetMap 원자료](https://api.openstreetmap.org/api/0.6/map?bbox=129.149,35.155,129.173,35.169)의 way `702600670`과 `239279167`을 따라 북쪽으로 이동한 뒤 동백로로 우회전하는 구간입니다.

DHU 설정 원문은 [dhu.ini](dhu.ini)입니다. [공식 DHU 문서](https://developer.android.com/training/cars/testing/dhu#sensors)에 따라 `[sensors]`에 `location = true`와 `driving_status = true`를 두었습니다. 콘솔의 `help location` 출력은 아래와 같았습니다.

```text
location lat long [accuracy(m)] [altitude(m)] [speed(m/s)] [bearing(degree)]
Set the location to the specified lat long values along with the optional accuracy, altitude, speed and bearing. Pass in NAN to skip optional parameters if needed.
```

정확도 자리에 `NAN`을 넣은 `location 35.1582787 129.1493314 NAN NAN 0 0` 명령은 DHU 콘솔에서 오류 없이 수락됐습니다. 다만 Android Auto 센서 메시지를 캡처하지 않아 **실제 전송된 정확도 필드가 완전히 빠졌는지, `NaN` 값이었는지는 검증하지 못했습니다.**

매 시험은 정확도 5m, 속도 14m/s의 위치 20개를 1초 간격으로 주입하는 이동 구간으로 시작했습니다. 이때 위치 표시가 경로를 따라 움직이고 남은 거리가 약 2.6km에서 2.4km로 줄었습니다. 이후 60초 정지 구간을 실행했습니다. 화면은 0·10·20·30·40·50·60초에 저장했습니다. 명령 원문: [시험 1](commands/trial1.txt), [시험 2](commands/trial2.txt), [시험 3](commands/trial3.txt), [시험 4](commands/trial4.txt). 이 파일들의 스크린샷 경로는 당시 DHU 작업 디렉터리 기준의 **상대 경로**입니다.

| 시험 | 정지 구간 0초 사진 | 10초 사진 | 60초 사진 |
| --- | --- | --- | --- |
| 1 | [0초](screenshots/trial1-tunnel-00.png) | [10초](screenshots/trial1-tunnel-10.png) | [60초](screenshots/trial1-tunnel-60.png) |
| 2 | [0초](screenshots/trial2-tunnel-00.png) | [10초](screenshots/trial2-tunnel-10.png) | [60초](screenshots/trial2-tunnel-60.png) |
| 3 | [0초](screenshots/trial3-tunnel-00.png) | [10초](screenshots/trial3-tunnel-10.png) | [60초](screenshots/trial3-tunnel-60.png) |
| 4 | [0초](screenshots/trial4-tunnel-00.png) | [10초](screenshots/trial4-tunnel-10.png) | [60초](screenshots/trial4-tunnel-60.png) |

나머지 20·30·40·50초 사진, 각 시험의 이동 구간 종료 사진과 길안내 시작 사진도 `screenshots/`에 있습니다. 각 사진의 실제 파일 저장 시각(KST)은 `evidence.json`의 `screenshot_capture_metadata`에 기록했습니다.

## 재설정 과정과 해석 한계

1. 시험 1 뒤에는 정확도 5m의 시작 좌표를 1회, 이어 5회 보내도 회색 위치 표시가 출발점으로 돌아오지 않았습니다. DHU를 다시 연결하고 미포항 길안내를 새로 시작한 뒤 시험 2를 진행했습니다.
2. 시험 2 뒤에도 시작 좌표 1회 주입이 반영되지 않아 DHU와 길안내를 다시 시작하고 시험 3을 진행했습니다.
3. 시험 4 준비 중 한 번은 부산 2.8km의 다른 경로가, 다시 검색했을 때는 서울 출발 약 401km 경로가 표시됐습니다. 두 화면은 시험에서 제외했습니다. DHU를 재연결해 앞선 시험과 같은 부산 2.6km 초반 경로를 확인한 뒤 시험 4를 진행했습니다.
4. 보조 명령 `help keycode`에는 `[E]: Command 'keycode' was not found.`가 출력됐습니다. 위치 주입 명령에는 콘솔 오류가 없었으며, `keycode`는 어느 시험에도 사용하지 않았습니다.
5. 이 결과는 **DHU와 Android Auto에 연결된 네이버 지도**의 화면 관찰입니다. 실제 Mazda 차량의 위치 메시지, 타임스탬프, 무선 동글 동작은 측정하지 않았습니다. 첨부 프로토콜의 실제 차량 동작 설명은 제공된 배경 주장으로만 취급했습니다.

## 정리

DHU를 종료하고 `adb forward --remove tcp:5277`, `adb kill-server`를 실행했습니다. 사용자는 S25에서 헤드 유닛 서버 중지, USB 케이블 분리, USB 디버깅 해제를 완료했다고 확인했습니다. 설치한 SDK와 임시 작업 파일은 보고서 묶음 검증·업로드 후 삭제했습니다.
