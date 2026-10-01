# 원본 LDS의 합성 NMEA 경로와 재현성 한계 — 2026-10-01

NA 74.00.324A 원본 커널과 원본 LDS를 격리 VM에서 직접 실행했습니다. 같은
진단용 initrd의 `pass`/`mirror` 두 부팅에서 원본 LDS의 USB 전원 GPIO
되읽기만 비교했고, 작성한 PTY로 합성 NMEA를 공급했습니다. `mirror`는
원본 LDS가 여는 정확한 GPIO 파일의 `fread`를 가로채 MMIO의 출력 latch와
방향을 확인한 뒤 `0`을 `1`로 전달하는 **진단용 preload**입니다. 원본 커널·LDS 바이너리,
제품 DSO와 출시 USB를 바꾸지 않았습니다. 이는 실제 GPIO 전기 상태, 물리
GNSS 수신이나 차량 동작을 입증하는 시험이 아닙니다.

SM은 원본 서비스 9개와 원본 의존성 4개를 골라 쓴 부분 그래프입니다.
원본 `jciLDS`의 serial 설정만 PTY로 바꾸고, 선택된 출력값에 따라
원본 D-Bus `ReadControl(0)`을 한 번 요청했습니다. 정상 전체 차량 기동이나
정상 AA 세션·폰 수용의 재현이 아닙니다. QEMU 7.2.22에는 네트워크·호스트
장치·공유 디렉터리를 연결하지 않았습니다.

## 완주한 동일 이미지 비교

두 실행의 원본 커널 SHA-256은
`57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240`,
진단 initrd SHA-256은
`8be1617cbe81a69282b4f2780952fde92165c142242c1892696bc6d36176d1f9`로
같습니다. 커널 인자 `mx5gpio=pass|mirror`만 다릅니다. 두 guest 모두 질의
30회, 공급기 종료 0, 완료 표식과 콘솔 해시 일치를 확인했습니다. helper의
시간 제한 종료 자체를 통과 근거로 세지 않았습니다.

| 조건 | 원본 GetPosition 응답 29건 | 원본 선택·상태 | PTY |
| --- | --- | --- | --- |
| 원래 되읽기 전달 | mode 0 29건, UTC·좌표 0 | selected 0 / READ_NOT_READY 5 | LDS 미개방 |
| latch/방향에 따른 진단 되읽기 | mode 1 13건, mode 0 16건 | selected 2, READ_READY 4 뒤 `ReadControl(0)` 반환 0, READ_STARTED 2 | LDS 개방 |

`pass`에서는 latch 1·출력 방향 1인데 원본 sysfs 값 0을 LDS에 그대로
전달한 표본이 90개였습니다. `mirror`에서는 같은 조합의 원본 값 0을
1로 전달한 표본 9개와 원본 LDS의 PTY 열린 fd를 확인했습니다. 수신기
명령 바이트 이벤트는 `mirror`에서 57건, `pass`에서 0건입니다. 두 조건의
`jciLDS`가 응답한 원본 API 값을 비교한 것이며, `mirror`는 하드웨어
되읽기를 대체했으므로 원본 QEMU의 성공 결과로 부르지 않습니다.

`mirror`의 질의 9–12는 합성 유효 위치 A의 mode 1·좌표, 14–16은
무효 위치 B의 mode 0·좌표, 17–25는 재획득 위치 C의 mode 1·좌표를
보였습니다. 이 16건의 UTC는 공급기가 실제 작성했고 PTY 대기열이
비어 있던 각각의 합성 NMEA 주기와 일치합니다. 질의 13은 **경계 혼합**입니다.
mode 0·B 좌표로 이미 바뀌었지만 UTC는 바로 전 A 주기의 값입니다.
따라서 한 응답의 모든 필드가 원자적으로 같은 NMEA 문장에서 왔다고
주장하지 않습니다. 공급 종료 뒤 질의 26–30은 mode 0이지만 마지막
C 좌표가 남아 있어 별도의 무효 profile로 세지 않았습니다.

완주 로그를 사후 독립 파서로 다시 검사할 때 처음에는 질의 13의
UTC·좌표를 같은 공급 주기에 대응시킨 assertion이 실패했습니다. 해당
경계 응답을 별도 검사항으로 고정하고 나머지 구간의 기록된 UTC·mode·
좌표·작성 여부를 대조한 검증은 통과했습니다. 비공개 원본 콘솔 해시는
`pass` `e5fa41555add076cc5e244d3ec686c8dfc31bcc1e27ba0514672c5b82db7b10a`,
`mirror` `32e71a900160c0105d89c63ccb84aaae2691375c97b832d772d1a8eb5114ef8c`입니다.

## 더 엄격한 fixture 재시도와 한계

이 완주 fixture의 진단 interposer에는 프로세스 공유 `strtok` 상태를
건드리는 결함이 있었고, NMEA의 A/B/C 시간표가 READ_STARTED에 묶이지
않았습니다. 후속 진단 코드에서 토큰 처리를 고치고 공급 시작을 원본 상태
2 뒤로 옮겼습니다. 그 수정만 적용한 초기 실행은 완주하지 못했습니다.
완주 당시의 initrd·컴파일 바이너리·해시 기록은 남았으나, 변경 전 진단
소스 파일의 별도 스냅샷은 남기지 못했습니다. 동일 소스 재빌드 가능성까지
주장하지 않습니다.

- 9서비스 후속 실행 두 번은 질의 23 근처에서 guest 진행이 멈췄습니다.
- `SYSTEM` 제거와 추가 로그 축소 뒤의 실행은 질의 3 근처에서 멈췄습니다.
- `SYSTEM` 없는 8서비스 구성의 첫 실행에서는 원본 SM이 `ECHILD`로
  종료하여 LDS가 시작하지 않았습니다. 셸 질의 루프의 완료는 LDS 성공이
  아닙니다.
- 진단용 keepalive를 둔 8서비스 동일 이미지 비교에서도 `pass`는 질의
  10, `mirror`는 질의 36 이후 멈췄고 guest 완료 표식이 없습니다. 둘 다
  GDB에서 두 vCPU가 동일 커널 WFI 경로 PC `0x80054af0`, LR
  `0x800528dc`에 있었습니다. `SYSTEM` 단독 원인이라는 가설은 지지되지
  않습니다. WFI 정지의 정확한 원인은 미분리입니다.

## 수정 fixture의 조건부 완주

9개 원본 서비스로 돌아가고, 진단용 CPU 유휴 방지 루프를 한 vCPU에 고정한
새 동일 이미지 비교를 수행했습니다. 루프가 타이밍·CPU 부하를 바꾸므로
기본 QEMU나 차량의 정상 동작 재현은 아닙니다. 새 진단 initrd SHA-256은
`fb90dfdea76735487d25149a04052ef7bde454d9c5e56170bb4ef35903e5312c`이며,
원본 커널은 위와 같은 해시입니다. 두 실행은 `mx5gpio=pass|mirror` 외에
같은 QEMU 명령·원본 서비스·진단 바이너리를 사용했습니다.

| 조건 | 위치 질의와 원본 응답 | 공급·GPIO 관측 | 결과 |
| --- | --- | --- | --- |
| `pass` | 30회 중 응답 29건 모두 mode 0, selected 0 / status 5 | latch·방향 1이지만 되읽기 0인 70표본, 공급기 START 없음, LDS PTY 미개방 | guest 완료 |
| `mirror` | 37회 중 응답 36건, READ_READY 뒤 원본 `ReadControl(0)`·READ_STARTED | 원본 GPIO `0`→진단 전달 `1`인 9표본, LDS PTY 개방·공급기 START | guest 완료 |

`mirror`에서 질의 10–16은 유효 A의 mode 1, 17–21은 무효 B의 mode 0,
22–37은 재획득 C의 mode 1입니다. 공급기는 READ_STARTED가 확인된
질의 9 이후에만 이 profile을 시작했습니다. A 7건, B 4건, C 16건의
UTC·좌표는 PTY에 실제 작성했고 공급 대기열이 비어 있던 고유 NMEA
주기와 맞았습니다. 첫 B 응답인 질의 17은 mode 0·B 좌표지만 UTC가
직전 A 주기에 남았습니다. 이전 fixture와 같은 유형의 경계 혼합이며,
원본 LDS 캐시 갱신·읽기 순서와 진단 타이밍 중 어느 것이 원인인지는
분리하지 못했습니다.

원본 D-Bus monitor의 `ReadControl(0)` 호출자·serial은 작성 질의의
method return 목적지·reply serial과 일치했고 호출은 한 번뿐이었습니다.
원본 LDS PID의 PTY fd, 공급 명령 바이트 이벤트 95건, 공급 종료 0,
두 guest 완료 표식과 콘솔·initrd·컴파일 산출물 해시를 사후 검증기가
함께 검사했습니다. 기본 QEMU에서 반복한 r3·r4·r6의 정지 원인을 이
루프로 규명한 것은 아닙니다. 가상 시간 `-icount auto` 진단도 원본
initramfs 진입 전에 중단되어 WFI 판정에 쓰지 않았습니다.

수정 비교의 비공개 콘솔 SHA-256은 `pass`
`89e92ca68300bc3a773bb33f51ff334d30b7e61a00ec2ea4970dd97e761163df`,
`mirror` `f91c692b2e2f62f237b0763b878de18340b1dc4a9c44929babddd13b5f0e5426`입니다.
진단 루프 없는 수정 fixture의 완주·재현성, 여러 부팅의 성공률과
정상 전체 SM 기동은 여전히 미확인입니다.

따라서 완주한 이전 비교는 **원본 LDS가 합성 NMEA와 진단 GPIO 되읽기
조건에서 상태를 전환한 관측**으로 사용합니다. 후속 수정 비교도 진단용
CPU 루프 아래 같은 전이를 보인 별개 관측입니다.
실제 수신기·센서의 측정 시각/품질, AA 요청별 provider/receiver/session
자격, 정상 전체 기동·복구와 폰/앱 위치 수용은 여전히 미검증입니다.
다른 작업 브랜치 `feat/session-observation`의 QEMU GPIO 모델 실행과
합성 ASSIST 선택 실행은 각각 외부 실행 기록으로 유지하며 이 직접 실행에
합산하지 않습니다. 제품 live ASSIST와
공개 USB ZIP은 변경하지 않았습니다.

비공개 진단 소스·완주/미완주 로그·사후 검증기와 실패 assertion은
`evidence/lds-path-20261001/`에 보존했습니다. OEM 바이너리나 전체
로그는 게시하지 않습니다. 이번에는 제품 코드를 바꾸지 않아 전체
`make test`와 ARM suite를 다시 실행하지 않았습니다. 사후 검증기 실행,
메타데이터/해시 대조와 원본 VM 실행을 이번 검증 범위로 기록합니다.
