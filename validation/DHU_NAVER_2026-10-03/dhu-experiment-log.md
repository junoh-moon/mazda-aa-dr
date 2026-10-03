# Android Auto DHU 위치 주입 실험 기록

## 환경과 설치 목록

- 작업일: 2026-10-03 (Asia/Seoul)
- 노트북: macOS 15.8, Apple Silicon (`arm64`)
- 설치 위치: `work/android-sdk/` (이 문서의 상위 디렉터리 기준)
- Android SDK Command-line Tools 22.0: `work/android-sdk/cmdline-tools/latest/`
- Android SDK Platform-Tools 37.0.1 (ADB 1.0.41): `work/android-sdk/platform-tools/`
- Android Auto Desktop Head Unit Emulator 2.0-mac-arm64: `work/android-sdk/extras/google/auto/`
- SDK 사용권 기록: `work/android-sdk/licenses/`
- 내려받은 Command-line Tools ZIP: `work/downloads/commandlinetools-mac_arm64.zip`
- DHU 실험 설정: `work/dhu-location.ini`
- Command-line Tools ZIP SHA-256: `835b62a26162b229b441d1f6d4680383815a270809eb33522c0d480fa5002c4e`

설치와 실험 파일은 모두 이 작업 디렉터리 안에 있으며 시스템 전체 설정은 변경하지 않았습니다.

## DHU 설정

```ini
[general]
touch = true
resolution = 800x480
dpi = 160

[sensors]
location = true
driving_status = true
```

공식 문서: https://developer.android.com/training/cars/testing/dhu

## 실험 경과와 관찰

| 시각 (KST) | 조치 | 관찰 |
| --- | --- | --- |
| 12:43 | 도구 설치 완료 | ADB 37.0.1, DHU 2.0-mac-arm64 확인 |
| 12:45경 | 삼성 Galaxy S25 (`SM-S931N`) USB 연결 | 첫 ADB 상태는 `unauthorized`; 휴대전화에서 디버깅 승인 후 `device` 확인 |
| 12:47경 | Android Auto 헤드 유닛 서버 시작, `adb forward tcp:5277 tcp:5277`, DHU 실행 | 설정 파일 로딩, ADB 연결, TLS 협상 성공. 첫 스크린샷에서는 `Don't have video focus - nothing to screenshot.` 오류가 있었으나 이후 화면 표시와 촬영 성공 |
| 기준 | 네이버 지도 화면 확인 | 파란 위치 표시가 강남역·역삼동 부근. 사용자는 과거 Lockito 모의 위치라고 설명했고, Lockito 앱은 현재 삭제한 상태. 실제 휴대전화 위치와 일치하는지는 확인되지 않음 |
| 12:49:14 | `location 35.1587 129.1604 5` 1회 주입 | 콘솔 오류 없음. 약 10초 뒤 지도와 파란 위치 표시가 부산 해운대 해변으로 이동. 사용자가 같은 이동을 확인함. 하단의 `강남구 역삼동` 문구는 당시 남아 있었음 |
| 12:53:05~12:54:05 | 해운대해변로 588m를 따라 61개 위치를 1초 간격으로 주입 | 0·20·40·60초 스크린샷에서 지도 중심과 파란 위치 표시가 경로를 따라 이동. 주입 속도 9.80m/s, 정확도 5m, 구간별 방위각 포함 |
| 12:54:42 | 마지막 주입 후 약 37초 | 부산 마지막 위치 부근에 머묾. 위치 화살표가 회색으로 변함 |
| 12:55:29 | 마지막 주입 후 약 84초 | 여전히 부산 마지막 위치 부근. 하단 문구가 `부산시 해운대구 중동`으로 갱신됨 |

경로는 2026-10-03에 OpenStreetMap API에서 조회한 **해운대해변로**의 연속된 7개 way(1359976816, 1359976814, 1359976815, 1359976817, 1495778540, 1359976818, 509125478)를 따라 생성했습니다. 경로 좌표 출처: https://api.openstreetmap.org/api/0.6/map?bbox=129.154,35.157,129.170,35.164

정확히 주입한 전체 DHU 콘솔 명령은 [dhu-trajectory-commands.txt](dhu-trajectory-commands.txt)에 있습니다. 파일 첫 3줄은 설명 주석이며 DHU에는 입력하지 않았습니다. `help location` 콘솔 출력은 `location lat long [accuracy(m)] [altitude(m)] [speed(m/s)] [bearing(degree)]`였습니다. `help sleep`과 `help screenshot`도 확인했습니다.

증거 이미지: [기준](dhu-baseline.png), [단일 주입 약 10초 뒤](dhu-jump-10s.png), 경로 [0초](dhu-trajectory-00s.png) · [20초](dhu-trajectory-20s.png) · [40초](dhu-trajectory-40s.png) · [60초](dhu-trajectory-60s.png), 주입 종료 [37초 뒤](dhu-after-stop-37s.png) · [84초 뒤](dhu-after-stop-84s.png).

## 판단과 범위

**판정 (1): 네이버 지도가 DHU에서 주입한 차량 위치와 이동 경로를 따라갔습니다.** 이는 Android Auto 환경의 네이버 지도가 차량 제공 위치를 사용한다는 판단을 뒷받침합니다. 실제 Mazda 차량, 차량 GPS의 시각 처리, 무선 동글의 동작에 관한 증거는 아닙니다. 기준 위치가 과거 Lockito 모의 위치였으므로, 초기 화면을 실제 휴대전화 GPS의 기준값으로 해석하지 않았습니다.

## 정리 절차

DHU를 `quit`으로 종료하고, `adb forward --remove tcp:5277`로 포워딩을 삭제한 뒤 `adb kill-server`로 Mac의 ADB 서버를 중지했습니다. 사용자가 휴대전화에서 **Stop head unit server**를 선택하고, USB 케이블을 분리하고, USB 디버깅을 껐다고 확인했습니다. Android Auto 개발자 모드와 Android 개발자 옵션 자체는 사용자가 선택해 유지할 수 있습니다.

설치 파일과 임시 파일은 아래 작업 디렉터리를 삭제하면 모두 제거됩니다. **12:57 KST에 실제로 삭제하고 `work/`가 없음을 확인했습니다.** 실험 증거와 이 기록은 `outputs/`에 보존합니다.

```sh
rm -rf work
```
