# 원본 LDS reader 입력·종료 실행 — 2026-10-02

NA 74.00.324A의 원본 Driver가 작성한 PTY 입력을 직접 읽고 NMEA parser와
등록된 원본 callback을 실행하여 위치 캐시를 갱신했습니다. 원본 `Close`의
정상 반환과 프로세스 정상 종료까지 확인했습니다. 이전
[제품 응답 실행](LDS_PRODUCT_RUNTIME_2026-10-02.md)의 수동 parser/callback
호출 경계를 줄이기 위한 별도 시험입니다. 이 실행에는 제품 preload와 AA
worker를 넣지 않았으며, 해당 제품까지의 결합은 후속 작업입니다.

## 실행과 작성한 조건

고정 GCC 4.9.1 ARM 하니스를 QEMU와 원본 공유 runtime에서 실행했습니다.
원본 서비스·Driver·NMEA·DBus·UBX 다섯 모듈의 파일 해시를 전후 대조했습니다.
Driver가 연 serial fd와 작성한 PTY slave의 장치 identity도 대조했습니다.
parser, callback 및 GPIO 동작을 성공 stub으로 대체하지 않았습니다.

입력 포트·설정 XML·기동 순서는 작성한 조건입니다. 원본 cache 초기화,
서비스 validity mutex 초기화, Driver Open과 원본 진단 초기화를 호출한 뒤
열 개의 원본 handler를 등록했습니다. 원본 reader pthread가 자신의 작업
버퍼에서 문장을 조립하고 parser와 callback을 호출합니다. 하니스는 결과를
원본 cache getter로 읽으며, 직접 parser/callback을 부르지 않습니다.

- GSA → GGA → RMC 세 문장으로 DOP, 좌표·고도, heading·속도를 채웠습니다.
- 다음 GGA를 분할하여 앞부분만 들어간 약 100ms 동안 캐시가 그대로인지
  검사하고, 나머지를 받은 뒤 좌표·고도 갱신과 이전 RMC 값의 유지를 확인했습니다.
- 다음 RMC에서 heading·속도가 바뀌고 UTC가 1초 증가하며 고도·DOP가
  유지되는지 확인했습니다. 첫 UTC는 0이 아님을 검사한 기준값이며,
  절대 UTC와 물리 시각의 독립 검증은 아닙니다. mode는 원시 관측값입니다.
- 원본 Close가 reader와 진단 thread를 정리하고 serial fd를 `-1`로
  바꾸어 반환한 뒤, 작성한 cache·mutex·PTY 자원 정리와 프로세스 종료를 확인했습니다.

## 먼저 드러난 실패와 수정 범위

| 실행 | 결과와 후속 대조 |
| --- | --- |
| `run-r1` | Open과 등록은 성공했지만 GSA 효과 대기에서 종료 91. 실제 read까지 도달했으나 마지막 LF가 다음 바이트 전까지 보류됐습니다. |
| `run-r2` | 다음 문장의 첫 바이트를 함께 공급한 연속 스트림에서 다섯 문장의 캐시 효과를 확인했습니다. Close가 reader를 join한 뒤 SIGSEGV로 종료했습니다. 빠진 원본 진단 초기화를 보완한 후 정상 Close를 확인했습니다. |
| `run-r3` | 원본 진단 초기화를 추가하여 Close 반환·fd 정리까지 진행했습니다. 하니스가 새 session의 controlling terminal을 얻어 PTY 정리 중 SIGHUP로 종료했습니다. |
| `run-r4` | 같은 `build-r3` 바이너리를 별도 process group·기존 session에서 실행하여 전체 종료 0을 확인했습니다. |

첫 실패들을 통과로 바꾸거나 지우지 않았습니다. 원본 명령어·분기·callback
table을 수정하지 않았으며, 원본 초기화 함수와 작성한 입력·실행 환경만
변경했습니다. 원본 Open이 생략했던 진단 초기화를 하니스가 명시적으로
호출한 조건이므로 전체 ServiceInit/ServiceTerm 성공으로 세지 않습니다.
`run-r2`의 fault PC는 보존하지 않았으므로 정확한 충돌 명령의 원인은
정적 경로와 후속 대조에 따른 추론입니다.

성공 하니스의 SHA-256은
`3c36a9c4517c54b1e96e4c99902a49e9cc665677ad190a797ddd914ec96a1952`입니다.
최종 실행 후 원본 모듈과 하니스 파일이 그대로이며 잔여 process group은
없었습니다. 비공개 `evidence/lds-assist-source-20261002/driver-read/`에
각 실행의 입력·표준 출력·syscall trace·결과와 실패 근거를 보존했습니다.

## v1.0에 남은 연결

이 결과는 작성한 바이트가 원본 입력 처리와 부분 갱신을 통과한다는 근거입니다.
제품 입력 owner, 실제 LDS 응답과 AA worker, 송신 전 요청별 출처의 연결은
추가로 검사해야 합니다. 물리 receiver·생산 시각·센서 보정·입력 품질·폰
수용의 근거를 부여하지 않으며 live ASSIST는 비활성입니다.

원본 하드웨어 제어 설정이 0이어도 내부 reset 경로가 모두 꺼진다고
가정하지 않았습니다. 실행 환경에는 차량 장치가 없고 `/sys`가 읽기
전용입니다. 최종 syscall trace에 GPIO/차량 장치 경로 접근은 없었지만,
이것만으로 모든 내부 helper 진입이 없었다고 주장하지 않습니다.
공개 제품 코드는 이 단위에서 변경하지 않았습니다. 기존 전용 컨테이너를
후속 검증에 재사용하므로 추가 도구의 최종 제거는 아직 진행 중입니다.
