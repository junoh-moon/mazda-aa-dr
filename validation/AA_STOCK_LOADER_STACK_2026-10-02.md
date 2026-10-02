# 원본 로더의 AA 정적 TLS·작은 스레드 경계 — 2026-10-02

## 결론과 범위

NA 74.00.324A 원본 userspace의 `ld`·`libc`·`libpthread` 2.11.1을
QEMU-user에서 사용해 작성한 작은 스레드를 실행했습니다. 16 KiB 스택에서
이전 공개 AA DSO를 preload하면 첫 `thread_enter` 표식 전에 보호 페이지에
SIGSEGV가 났고, 새 AA DSO와 preload 없음은 통과했습니다. 제품 코드를
넣지 않은 **7,284바이트 정적 TLS 전용 DSO도 보호 페이지 SIGSEGV**를
재현했습니다. 따라서 이 작성한 16 KiB 경계에서는 큰 정적 TLS만으로
실패를 재현할 수 있습니다. 원본 AAPA의 실제 스레드 크기·여유나 차량
장애 원인을 확정하는 결과는 아닙니다.

이 기록은 [AA 문맥 풀 변경](AA_CONTEXT_POOL_2026-10-02.md)의 후속
오프라인 검증입니다. 제품 코드를 더 수정하거나 새 ZIP을 발행하지
않았습니다.

## 고정 입력과 실행

| 입력 | SHA-256 | `PT_TLS` |
| --- | --- | ---: |
| 사용 중단한 공개 `.2` ZIP의 AA DSO | `23d8265a300aff67a0123d39905f56e61fabf1a85d2c277f533d3c163382b8cf` | 7,284 B, 정렬 8 |
| 새 AA DSO | `c9ec09d9574740a25bad8a00206d8b7591049e93ad26a740ce460c9cea236497` | 196 B, 정렬 4 |

옛 파일은 공개 ZIP 내부의 `libmx5dr.so`와 해시가 같음을 대조했습니다.
작성한 [ARM probe](../tests/adapter/stock_tls_probe.cpp)는 고정
GCC 4.9.1로 빌드했으며 `pthread_attr_setstacksize`를 16/24 KiB로
바꾸는 환경 변수만 달리했습니다. QEMU-arm 5.2.0의 `-L`은 원본 rootfs를
가리키고, guest 로더 진단에서 `/lib/libc.so.6`·`/lib/libpthread.so.0`
및 `/usr/lib/libstdc++.so.6` 해결을 확인했습니다. 원본 `libc`의
실행 버전은 2.11.1이며 `ld`·`libpthread`의 원본 symlink 대상도
2.11.1입니다. 실행 Linux의 페이지 크기는 4,096바이트입니다.
preload는 guest의 `LD_PRELOAD`로만 설정했습니다. probe에는 별도
`dlopen` 코드 경로가 있지만 **이 TLS 대조에서는 해당 환경 변수를
설정하지 않았고**, 제품 hook도 직접 호출하지 않았습니다.

비공개 rootfs와 제품 경로를 제외한 작성 시험의 핵심 빌드·실행 형태는
다음과 같습니다. `CROSS`는 [고정 툴체인](../docs/toolchain.md)의
`arm-cortexa9_neon-linux-gnueabi-` 접두사이고, `OEM_ROOT`는 로컬
원본 rootfs, `PRELOAD_DSO`는 대조할 파일입니다. `LD_PRELOAD`를 빼면
baseline입니다.

```sh
"${CROSS}g++" -std=c++11 -O2 -pthread tests/adapter/stock_tls_probe.cpp \
  -ldl -o probe
"${CROSS}gcc" -shared -fPIC -O2 -DTLS_BYTES=7284 \
  tests/adapter/stock_tls_only.c -o tls-7284.so
qemu-arm -L "$OEM_ROOT" -E LD_PRELOAD="$PRELOAD_DSO" ./probe
```

스레드 시작 표식은 버퍼링 없는 `write`를 사용합니다. 옛 AA DSO의
16 KiB 실행은 `main_ready`·`before_pthread_create`·
`after_pthread_create`를 출력했지만 첫 `thread_enter` 전에 끝났습니다.
다음 표는 같은 작성 프로그램의 종료 코드를 구분합니다.

| preload | 16 KiB 스레드 | 24 KiB 스레드 |
| --- | --- | --- |
| 없음 | 종료 0, 3회 반복 | 종료 0, 1회 |
| 공개 `.2` AA DSO | SIGSEGV/139, 3회 반복 | 종료 0, 1회 |
| 새 AA DSO | 종료 0, 3회 반복 | 종료 0, 1회 |

반복 횟수는 동일 환경에서 결과의 재현을 확인한 것이며 독립적인 차량
관측 개수나 통계적 확률이 아닙니다. 두 제품은 TLS 외에도 코드·BSS·
relocation이 다르므로 이 제품끼리의 대조만으로 단일 원인을 단정하지
않습니다.

이를 분리하기 위해 생성자·AA 코드를 두지 않고 `__thread` 배열만 가진
합성 DSO를 [공개된 작성 소스](../tests/adapter/stock_tls_only.c)와 같은
컴파일러로 빌드했습니다. `readelf`에서 요구한 크기의
`PT_TLS`를 확인했습니다. 작성한 16 KiB 스레드에서는 196, 1,024,
2,048, 4,096, 6,144바이트가 종료 0이고 **7,284바이트가 SIGSEGV/139**
였습니다. 이 여섯 크기는 각 스택 크기에서 1회씩 실행했고,
24 KiB에서는 모두 종료 0이었습니다.
합성 DSO의 TLS 정렬은 4, 옛 AA DSO는 8로 동일한 ELF가 아닙니다.
정렬 8을 지정한 추가 대조에서는 컴파일러의 `.tbss` 반올림으로
`TLS_BYTES=7284`의 실제 `PT_TLS`가 **7,288바이트·정렬 8**이었습니다.
이 DSO의 16 KiB 실행은 3회 모두 139, 24 KiB 실행은 1회 종료 0이고,
6,144바이트·정렬 8은 16 KiB에서 종료 0입니다. 정렬을 맞춰도 같은
크기 경계에서 실패하지만 옛 제품의 7,284바이트 ELF와 완전히 같은
레이아웃이라는 뜻은 아닙니다.
7,284바이트의 합성 DSO와 이전 AA DSO의 guest syscall trace에는
16 KiB 스택의 주소 하단 4 KiB가 `PROT_NONE`으로 설정되고, SIGSEGV 주소가
두 실행 모두 그 보호 페이지의 위쪽 끝에서 12바이트 아래인 것이 확인됩니다.
`clone`의 `child_stack`은 두 조건에서 보호 페이지 위쪽 끝보다 808바이트
높았고, 새 AA DSO 조건에서는 7,896바이트 높았습니다. 따라서 이 작성한
프로세스에서는 큰 정적 TLS가 스레드의 초기 가용 스택을 7,088바이트
줄였습니다. 프로세스마다 함께 로드된 라이브러리의 TLS가 달라 이
16 KiB 실패 경계를 실제 AAPA에 옮길 수는 없습니다. 이는 작성한 경계에서
정적 TLS 크기가 실패를 일으키기에 충분하다는 직접 대조이며,
두 syscall trace만으로 fault 명령 위치를 알 수는 없습니다.
별도 QEMU GDB 원격 접속에서 첫 진입점부터 각각 한 번씩 계속 실행하자
옛 제품과 7,284바이트 합성 DSO 모두 **두 번째 guest 스레드**의
원본 로더 프레임에서 SIGSEGV로 정지했습니다. 두 실행의 guest PC는
같았고 상위 backtrace는 원본 로더·`libpthread`였습니다. 이 결과는
작성한 시험의 로더 스레드 시작 실패를 더 좁혀 주지만, debugger의
타이밍 영향과 실제 AAPA의 스택 조건까지 배제하지는 않습니다.
8,192바이트의 추가 16 KiB 시도는 신호 뒤 QEMU 종료가 지연돼 소유
프로세스를 정리했으며 통과·실패 표에 포함하지 않았습니다.

별도로 SHA-256 `c9ec09d9574740a25bad8a00206d8b7591049e93ad26a740ce460c9cea236497`의
**실제 AA 제품 DSO**를 preload하고 원본 로더·libc에서
[작성한 POSITION 호출기](../tests/adapter/veneer_unwind_test.cpp)를
[원본 로더 실행기](../tests/adapter/run_stock_unwind.sh)로 실행했습니다.
이 실행기는 [DSO 실행기](../tests/adapter/run_unwind_dso.py)의
`--sysroot`를 원본 rootfs로 지정하고 원본 `ld`·`libc`·`libpthread`·
`libgcc_s`·`libstdc++`·`libdl`·`libm`·`librt`, QEMU·컴파일러
드라이버·제품·실행기 파일을 전후
해시 대조합니다. 모든 사례는 종료 0과
`PASS ARM unwind` 표식을 확인했습니다. 기존 14개 중 예외·취소
사례는 unwind 뒤 문맥 없음과 fault를 검사했지만 **풀 슬롯 반환은
검사하지 못했습니다**. 이어 예외·취소를 각 65회 반복해 64개 슬롯이 누적
고갈되지 않는지 검사하고, WorkerContext 크기의 작성 스택 사용과
16 KiB 스택에서 예외·취소를 각 1회 더 실행했습니다. 최종 **18개
사례 전부 통과**했습니다. 새 반복 사례의 `CONTEXT_UNAVAILABLE`는
0건입니다. 비공개 `unwind-dso.json`에는 제품·실행 파일·76개 소스
해시와 18개 사례·종료 코드가 고정돼 있으며 SHA-256은
`244e477b02ff5f7bfd18c823ee5e2f59da49a22553634d041058b193912e7e63`입니다.
원본 라이브러리·도구 identity 목록은 실행 전후 바이트가 같았고,
그 목록의 SHA-256은
`bb81407315d998b3b0e85d70a1ced01ee6d3e36438b743bfea5b9d9b0aa12fbd`입니다.
같은 실행 파일의 `LD_DEBUG=libs` 작은 스택 취소 재실행에서 원본 rootfs의
`libpthread`·`libc`·`libgcc_s`·`libstdc++`·`libdl`·`libm`·`librt`를
해결함을 확인했습니다.
호출기 컴파일은 고정 툴체인의 sysroot를, guest 실행은 원본 rootfs를
썼습니다. 원본 AAPA 프레임의 unwind나 실제 요청·송신은 실행하지
않았습니다.

## 풀리지 않은 원본 기동

작성한 16 KiB 스레드에서 원본 `blmjciaapa.so`를 곧바로 `dlopen`하면
preload 없음과 새 AA DSO preload 양쪽에서 SIGSEGV가 납니다. 24 KiB로
바꾸면 양쪽 모두 원본 전역 의존성이 빠졌다는 로더 오류까지 진행합니다.
해당 원본 공통 유틸 DSO를 `LD_PRELOAD`로 전역 scope에 먼저 로드하고,
원본 AA 경로를 `MX5DR_OEM_AA_PATH`로 지정한 뒤 `dlclose`를 생략한
작성 대조에서는 24 KiB의 AA 제품 preload 없음·새 AA 제품 preload 조건 모두
`after_dlopen` 표식과 프로세스 종료 0을 각각 1회 확인했습니다.
새 제품은 `dlopen` interposer를 내보내므로 이 대조에서 그 경로를
거칠 수 있지만, 설치 결과·hook 상태는 검사하지 않았습니다.
`dlclose`를 생략하지 않은 bare 대조에서는 `after_dlopen` 뒤
SIGSEGV가 났으므로 모듈 수명 전체의 성공으로 세지 않습니다.
이 실행은 원본 서비스의 초기화·IPC·작업 큐나 제품 cold hook 성공을
확인하지 않습니다. 16 KiB의 bare `dlopen`
SIGSEGV는 원본 모듈의 스택 사용과 환경 부재가 섞였으므로 새 제품의
결함이나 성공으로 분류하지 않습니다. 변경 전
제품의 [원본 계산·송신 실행](AA_PRODUCT_ASSIST_2026-10-02.md)과
[철회·재개 실행](AA_ASSIST_RECOVERY_2026-10-02.md)에 사용한 비공개
호출기는 현재 작업공간에서 찾지 못했습니다. 새 AA 제품으로 해당
전체 경로를 재실행하지 못했고, 정상 OEM 기동·차량·폰 수용은 남습니다.

Claude의 읽기 전용 적대적 검토는 제품 차이만으로 TLS 원인을 단정할
수 없고, `dlopen`의 작은 스택 혼선, 14개 unwind 사례의 슬롯 반환
검사 누락, 원본 로더 identity 기록 누락을 지적했습니다. 이에 TLS 전용
대조·버퍼링 없는 표식·syscall 보호 페이지 주소·별도 GDB의 같은 guest
PC, 65회 재획득·16 KiB unwind와 원본 라이브러리 전후 해시를
보강했습니다. Claude는 소스와 비공개 원문을 읽었지만 파일 수정은
하지 않았습니다. **작성한 실험의 인과 추론만** 보강된 것이며,
같은 fault PC의 내부 함수 의미와 실제 AAPA 스택 크기·고수위 사용량·
전체 기동은 여전히 미확인입니다.
원본 바이너리·메모리 맵·syscall 원문은 비공개 보관하고 이 문서에
포함하지 않았습니다.

## 변경 뒤 회귀 검사

같은 AA 제품 해시로 `make test`를 다시 실행했습니다. host Python
590개와 C/C++ 검사는 통과했습니다. 이 일반 실행의 패키징 293개에는
원본 rootfs·여섯 산출물 입력이 없어서 40개가 생략됐습니다. 이어 원본
rootfs fixture와 [로컬 ZIP](AA_CONTEXT_POOL_2026-10-02.md)의 실제
여섯 산출물을 지정한 `make test-packaging`에서는 패키징 293개가
**생략 없이 통과**했습니다. 별도 고정 ARM 전체 실행의 Python 118개와
실제 AA 제품 DSO 열 묶음 163개도 통과했고 실행 시작·종료에 기록한
여섯 산출물 해시가 같았습니다. 이 회귀는 작성한 사례이며 원본 AAPA의
전체 서비스 시작이나 실차의 합격 판정은 아닙니다.
