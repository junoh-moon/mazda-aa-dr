# LDS cold installer 검증 — 2026-10-02

NA 74.00.324A 원본 LDS를 cold `dlopen`한 뒤 새 `install_lds_v74`의
**ARM 71개·호스트 1개 검사**를 생략 없이 통과했습니다. 제품 설치기 소스를
작성한 검사 실행 파일에 링크한 결과입니다. 실제 제품 DSO의 자동 기동,
AA/LDS preload 공존과 정상 전체 SM 부팅은 이 결과에 포함하지 않습니다.
차량·폰 수용과 live ASSIST 자격도 추가하지 않습니다.

이 기록은 아래 소스 해시로 고정한 단독 검증입니다. 제품 DSO·패키징과
전체 통합 검사는 별도 작업이며, 기존 공개 ZIP에 LDS 공급부가 설치된다는
뜻이 아닙니다. [현재 상태](../docs/STATUS_KO.md)와
[v1.0 조건](../docs/V1_READINESS_KO.md)을 함께 확인하십시오.

## 원본과 설치 범위

고정 GCC 4.9.1 도구체인 `61ec0343de84f6fc7c46840056df1d600d44be8a`와
QEMU user, 제공 펌웨어의 원본 loader·공유 runtime을 사용했습니다.
원본 바이너리는 비공개 fixture에서 읽습니다. 매 사례는 별도 프로세스이며
이전 검사의 immutable binding이나 설치 상태를 재사용하지 않습니다.

설치기는 svcjcilds, LDS D-Bus·driver, JCIDBUS, libdbus, jcicommon의 여섯
파일 해시와 실제 로딩된 의존성을 확인합니다. 원본 함수·호출 위치,
읽기/실행 및 읽기/쓰기 영역, cache literal과 descriptor를 검증한 뒤
25개 데이터 슬롯을 한 transaction으로 바꿉니다. 원본 실행 코드와
GetPosition descriptor는 유지합니다.

실제 LOCAL/GLOBAL cold load에서 LDS D-Bus가 참조하는 mutex provider는
svcjcilds였습니다. 같은 이름의 다른 모듈 export를 원본이라고 가정하지
않았습니다. 독립 로딩 조사의 다른 선행 의존성 순서는 미해결 원본 심볼로
실패했으며, 이를 지원되는 정상 기동 순서로 표시하지 않습니다.

성공 후 실제 svc GOT를 통해 원본 cache Initialize→read/update→Clear,
MEM_Copy·mutex·등록 API를 호출했습니다. 작성한 cache 값의 정확한 복사와
원본 반환값·errno를 대조했습니다. 호출자가 service handle을 `dlclose`한
뒤에도 원본과 wrapper가 호출 가능함을 확인했습니다. 이는 하드웨어에
의존하는 DriverOpen·ServiceInit·startOperation 실행이 아닙니다.

## 실제 등록 ID 누락을 먼저 재현

최초 계약은 세 handler의 등록 ID 0·2·4만 포함했습니다. 원본
`lds_startOperation`의 등록 인자, 실제 분기 대상과 relocation literal을
다시 대조하니 다음 10개 ID가 같은 세 handler에 연결되어 있었습니다.

| 원본 handler | 등록 ID | 관측하는 할당 필드 |
| --- | --- | --- |
| RMC/location | 0, 6, 1, 7 | mode·UTC·좌표·방향·속도 |
| GSA/satellite | 2, 3, 5, 9 | mode·horizontal·vertical |
| GGA/elevation | 4, 8 | mode·좌표·고도 |

ServiceInit에는 직접 등록 호출이 없었습니다. 별도 NMEA 검출 경로의 다른
등록도 있으며, 같은 ID라는 이유로 다른 callback을 위 handler로 취급하지
않습니다. 위 표는 물리 측정 출처나 등록 instance의 고유 ID를 뜻하지 않습니다.

수정 전 실제 ARM 설치기는 ID 0·2·4만 wrapper로 바꿨습니다. 나머지
**1·3·5·6·7·8·9의 일곱 회귀는 등록 성공 뒤 wrapper 누락으로 실패**했습니다.
수정 후 10개 모두 통과했습니다. 원본의 중복 등록 반환 104와 저장된
callback 불변, 먼저 저장한 wrapper가 계속 같은 원본에 전달하는 동작도
검사했습니다. 원본 enclosing startup 함수는 실행하지 않았습니다.

## 통과한 검사와 실패 주입

| ARM 검사 범위 | 사례 수 |
| --- | ---: |
| LOCAL/GLOBAL 설치·호출자 dlclose 이후 유지·반복 설치 거부 | 5 |
| 잘못된 옵션·각 파일 해시 거부·각 live GOT 연결 불일치 | 38 |
| cold lease 거부 | 1 |
| 원본 등록 ID별 wrapper와 immutable target | 10 |
| 원본 caller 참조만 닫았을 때의 unload 기준선 | 1 |
| descriptor 네 필드·cache literal 네 곳·entry/caller/register 명령어 훼손 | 11 |
| 데이터 페이지 세 곳의 mprotect 실패 | 3 |
| lease 진입 직후 GOT 변경·기존 immutable bus preparation 점유 | 2 |
| **합계** | **71** |

호스트에서는 `UNSUPPORTED_ARCH` 한 사례를 별도로 확인했습니다.
초기 TODO stub의 기본 45개 assertion 실패와 등록 alias의 추가 실패를
보존했습니다. 최초 GCC 4.9 fixture 문법 오류도 남겼으며, 이를 제품 동작의
실패 회귀로 세지 않습니다.

`mprotect` 실패는 명시적인 `--wrap=mprotect` 검사 주입입니다. 나머지 호출은
실제 mprotect를 사용합니다. 훼손한 원본은 해당 프로세스의 메모리 사본뿐이며
원본 파일은 수정하지 않았습니다. 거부 시 다른 슬롯과 원본 코드를 바꾸지
않고, lease를 얻었다면 `end(true)`로 종료함을 확인했습니다. 준비 전 실패는
불필요한 모듈 참조를 남기지 않습니다. 선행 bus preparation이 있는 상태에서
설치 preparation이 실패해도 원본 targets를 유지해 호출자의 dlclose 뒤에
전달합니다.

## 공개 runner 재현

[run_lds_install.sh](../tests/adapter/run_lds_install.sh)는
[lds_install_test.cpp](../tests/adapter/lds_install_test.cpp)를 별도로 빌드하고
71개 사례를 각각 새 QEMU 프로세스로 실행합니다.

- `CROSS_COMPILE`과 `QEMU_SYSROOT`는 고정 빌드 도구를 선택합니다.
- `MX5DR_LDS_STOCK`은 별도의 비공개 원본 runtime입니다. 없거나 필요한
  파일이 빠지면 명시적으로 종료 77을 반환하며 통과로 표시하지 않습니다.
- `QEMU_ARM`은 실행 파일 경로, `LDS_INSTALL_BUILD`는 기록의 상위 경로입니다.
  매 실행은 고유 하위 디렉터리를 만들어 이전 실패를 덮어쓰지 않습니다.
- 원본 여섯 파일과 소스의 실행 전후 SHA, NUL로 구분한 명령 인자,
  각 로그·종료값과 fixture 해시를 남깁니다.

공개 runner를 **section GC 없이 새로 빌드하여 ARM 71개를 다시 통과**했습니다.
소스·컴파일러·sysroot·원본 root·QEMU 실행 파일·출력 경로 모두에 공백을
넣어 대조했습니다. 원본 fixture 미제공 시 종료 77도 별도로 확인했습니다.
이는 앞선 소스 링크 실행 파일과 다른 바이너리이며, 실제 제품 DSO 실행으로
바꾸어 세지 않습니다. 각 실행의 원본 파일과 소스 입력은 실행 전후
동일했습니다.

| 고정 항목 | SHA-256 |
| --- | --- |
| installer 소스 | `f88f8de18bf93ffe0595afc501b66043a748e83a31f89fcf62d6c24d2f371a98` |
| C++ 검사 소스 | `a94cbf905709787e1733f39074a65e1a6a08374ac9333682fbc9cc6c521262b8` |
| 공개 runner | `b2355d3a17d9d06e4de5d77f484ae94ff5f189f206d84192fae36c7c0ac411da` |
| 공개 runner가 새로 만든 ARM fixture | `2e1c59d84049b8409577b0a1c5d86c2a75d7db4b22b705d78d0115939a83749e` |

비공개 근거는 `evidence/lds-runtime-20261002/private-analysis/`의
`registration-contract-r1.json`, `install-registration-red-r1`,
`install-all-r1`, `installer-test-final-summary.json`, `public-runner-r1`에
보존했습니다. OEM 바이너리·덤프·원시 차량 로그는 공개하지 않습니다.
이번 검사 중 도구를 추가 설치하지 않았으며, 기존 격리 환경은 후속 LDS
검증을 위해 유지 중입니다. 최종 도구 제거 완료를 아직 주장하지 않습니다.

## 별도 검증이 필요한 범위

실제 제품 DSO의 loader lease→cold installer 자동 연결과 다른 preload와의
공존, 정상 SM/ServiceInit·driver 수명, 등록된 실제 parser 입력부터
응답 sideband·AA 소비까지의 연결은 후속 검증 범위입니다. 이 문서의 직접
cache API 호출이나 null callback 전달을 해당 경로의 성공으로 대신하지
않습니다. 물리 측정 시각·센서 품질·복구·폰/앱 수용은 여전히 별개이며,
실제 관성항법을 적용하는 v1.0 완료 조건도 남아 있습니다.
