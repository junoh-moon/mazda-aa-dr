# ARM 후크의 예외·스레드 취소 정리 수정 — 2026-09-30

기존 위치 assembly 후크를 통과하는 합성 C++ 예외가 호출자의 catch에
도달하지 않고 abort(종료 134)하는 결함을 재현했습니다. 이 수정은 후크의
정상 호출 계약을 보존하면서 예외와 deferred pthread cancellation이
호출자까지 전달되고 관측 상태도 정리되도록 합니다. 예외를 성공 반환으로
바꾸거나 OEM 작업을 재시도하지 않습니다.

## 변경

- 위치 veneer와 원본 호출용 invoker에 실제 스택과 일치하는 ARM EHABI
  정보를 추가했습니다. r0–r3를 저장하여 인자와 불투명한 결과 비트를
  전달하고, C++ scope가 정상 반환·unwind 모두에서 위치 깊이를 복원합니다.
- send 깊이도 C++ scope로 정리합니다. 원본 send나 callback의 unwind가
  다음 호출을 잘못된 중첩 호출로 만들지 않습니다.
- 해당 adapter 번역 단위에만 예외 정리 코드를 활성화했습니다. 첫 제품
  빌드에서 static libstdc++의 `__cxa_*` 등이 동적 심볼로 노출되는 것도
  발견했습니다. `--exclude-libs,ALL`로 숨겨 preload가 OEM의 C++ 런타임
  심볼을 가로채지 않도록 수정했습니다.
- 생산 라이브러리 파일 자체를 startup preload한 뒤 숨겨진 작성 코드의
  ELF 심볼 위치로 호출하는 별도 시험을 추가했습니다. 실제 C send 진입점과
  설치기의 공개 prologue 계약을 따르는 mmap trampoline을 거칩니다.
  OEM 함수나 OEM 파일을 합성 대역으로 바꿔 실행한 시험은 아닙니다.

EHABI directive 의미는 [GNU assembler 공식 문서](https://sourceware.org/binutils/docs/as/ARM-Directives.html)를
참고했으며, 선언만으로 완료 처리하지 않고 생성된 ELF와 실제 unwind를 검사했습니다.

## 검증

| 항목 | 결과와 범위 |
| --- | --- |
| 수정 전 재현 | 외부 catch가 있는 위치 예외에도 abort 134. 이전 source·ELF·실패 로그 보존 |
| 기존 ARM veneer | 중첩 위치, 인자, r0–r3 반환 비트, errno 검사 통과 |
| 작성 ARM 예외/취소 8개 | 위치/송신 예외, 중첩 위치 예외, 위치/송신 취소, 위치 진입 sink의 예외/취소, 중첩 send 예외 모두 통과 |
| 실제 제품 DSO | 별도 동적 C++ 런타임의 caller→정적 런타임을 포함한 배포 DSO→외부 catch/cleanup 경계에서 같은 8개 통과 |
| 순정 runtime VM | 동일한 제품 DSO와 작성 target를 원본 커널/rootfs의 libc·C++ 공유 런타임에서 실행하여 8개 모두 정상 종료 0 |
| 전체 회귀 | 새 다섯 ARM 산출물로 `tests/run_arm_all.sh` 통과. `make test`의 C/C++ 검사와 Python 267개 실행. 처음 원본 fixture 경로 때문에 생략된 설치기 20개는 경로를 바로잡아 별도 재실행, 모두 통과 |
| 원본 정상 호출 경로 | 별도 [요청 종료 시험](REQUEST_LIFECYCLE_2026-09-30.md)의 원본 AA/LDS/worker/send fixture에도 새 adapter를 링크하여 정상 반환 검사 통과. 이 fixture는 제품 DSO 설치 시험과 별개 |

새 DSO 시험은 제품 파일을 변경하지 않고 해시 불변을 확인합니다. caller는
동적 libstdc++/libgcc를 사용하고 제품은 숨긴 정적 런타임을 포함합니다.
위치/send scope 바깥에서 다시 송신하여 `NO_CONTEXT` 및 단일 관측을 검사하고,
중첩 예외를 내부에서 catch한 경우 바깥 위치의 SCRUB 동작을 확인했습니다.
취소는 무시하거나 일반 성공으로 돌려주지 않고 `PTHREAD_CANCELED`로 끝납니다.

순정 runtime VM도 예외를 던지는 target는 **작성한 코드**입니다. 원본 OEM
함수마다 예외를 주입하거나 OEM 전체 호출 스택의 unwind를 검증한 것이
아닙니다. 비공개 요청 후크의 notify/doWork 전체에는 별도 정리가 남아
있습니다. asynchronous cancellation, signal 탈출, live C++ scope를 가로지르는
longjmp까지 지원한다는 주장은 하지 않습니다. 차량·폰 검증은 수행하지 않았습니다.

## 독립 리뷰와 검증기 수정

네 Codex 리뷰어가 코드·installer 계약·빌드·시험을 독립 검토했습니다.
원본 runtime 조사는 작성자가 직접 수행했습니다.

- 리뷰어 한 명이 별도 작성 DSO의 다섯 예외/취소 사례와 100회의 r0–r3
  인자/결과, r4–r11, 정확한 SP 복원·정렬, errno 검사를 실행해 통과했습니다.
- 검증기의 두 결함을 지적받아 수정했습니다. Python `-O`에서 사라지는
  assert 판정은 명시적 실패로 바꿨고, 기록 해시는 실제 컴파일 snapshot에서
  산출합니다. 원본 입력의 전후 일치도 확인합니다. 리뷰어는 수정 전의 잘못된
  PASS와 수정 후의 거부를 실패 주입으로 대조했습니다.
- 처음 빠졌던 실제 제품 DSO, C send 진입점, relocated trampoline과
  진입 중·중첩 send unwind 시험을 추가했습니다. 최종 리뷰에서 추가로
  확정한 P1/P2는 없었습니다. 독립 리뷰가 OEM 전체 실행을 대신하지는 않습니다.

Claude의 새 실행·검토를 주장하지 않습니다. 기존 Claude 조사 커밋과 별개인
이번 코드·시험·리뷰 결과이며 새 검증 기록으로 남깁니다.

## 고정된 입력과 산출물

| 항목 | SHA-256 |
| --- | --- |
| 최종 `libmx5dr.so` | `b6cb9ce2d0ba861c077a219459665132272eacc5bed86b7286f1611b9a5e8a56` |
| 순정 runtime에서 실행한 작성 ELF | `41d7320539ccd19bc41ca60acf80127a9d00be9c9740b79a8728e662f4e22644` |
| 순정 runtime 진단 initramfs | `13831a0d9798d6cd3e89ac882dda58b5ee9fe9877c16e57af78d1fc23d222402` |
| 순정 runtime 콘솔 | `b924a08eea9ea91bbd1935e45f8349624e53cead2746a2827ebd09256d54f9cc` |

고정 toolchain은 GCC 4.9.1, commit
`61ec0343de84f6fc7c46840056df1d600d44be8a`입니다. private 증거는
`evidence/adapter-unwind-20260930/`, `evidence/request-lifecycle-20260930/`에
보존했습니다. 원본 이미지·주소 포함 로그는 게시하지 않습니다. VM은 guest
halt 뒤 외부 120초 제한으로 종료됐으며 runner 124/QEMU 0은 PASS 근거가
아닙니다. 각 사례의 실제 종료와 독립 verifier를 기준으로 삼았습니다.

이 변경은 소스 수정입니다. 새 설치 ZIP이나 v1.0 릴리즈를 게시한 기록이
아니며 live ASSIST를 활성화하지 않습니다.
