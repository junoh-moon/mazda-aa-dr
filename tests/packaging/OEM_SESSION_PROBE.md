# 원본 AA 세션 API 진단

`oem_session_probe.cpp`는 NA 74.00.324A 원본 `RaceAap::Init/UnInit`과
AA interface API를 격리 ARM VM에서 호출합니다. 원본이 만든 콜백 테이블의
상태 콜백만 관측 wrapper로 감싸며, 원래 userdata와 전체 입력 포인터를
원본 콜백에 한 번 전달합니다. 두 생성마다 다른 wrapper를 사용합니다.
복사한 전체 콜백 테이블도 생성별 문맥에 보관하여 진단 종료까지 유지합니다.
제품이나 USB 설치 파일에는 포함되지 않습니다.

생성·송신·시작의 반환 0은 폰 수용을 의미하지 않습니다. 시작 입력은
**304바이트의 합성 0값**이며 실제 기기 연결 정보가 없습니다. 원본 서비스의
실패 상태·비동기 콜백·종료를 관찰하는 진단입니다. OEM 작업 큐, 정상 AA
연결 사건, LDS 요청, 제품 후크와 센서·ASSIST 경로는 실행하지 않습니다.

## 빌드와 실행

Linux 전용 컨테이너에서 [고정 도구체인](../../docs/toolchain.md)을 사용하십시오.
원본 rootfs tar·커널과 해시가 맞는 USB 묶음은 비공개 입력입니다. 추가 도구는
컨테이너 안에 설치하고 검증 자료를 보존한 뒤 컨테이너를 제거하십시오.

```sh
MX5_TOOLCHAIN=/opt/m3-toolchain
"$MX5_TOOLCHAIN/bin/arm-cortexa9_neon-linux-gnueabi-g++" \
  -std=c++11 -O2 -Wall -Wextra -Werror \
  -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm \
  -Isrc tests/packaging/oem_session_probe.cpp src/runtime/sha256.cpp \
  -ldl -pthread -lrt -o /work/session-probe

python3 tests/packaging/oem_system_emulation.py build \
  --rootfs-tar /work/rootfs.tar.gz --bundle /work/usb \
  --session-probe /work/session-probe --output /work/session.cpio.gz

python3 tests/packaging/oem_system_emulation.py run \
  --board cmu --mode baseline --phase session --seconds 115 \
  --kernel-arg nohlt --kernel-arg enable_wait_mode=off \
  --kernel /work/stock-zImage --initrd /work/session.cpio.gz \
  --output /work/session-run

python3 tests/packaging/oem_system_emulation.py check-session \
  --console /work/session-run.log
```

빌더는 UID 0인 격리 컨테이너에서 실행해야 합니다. 공통 이미지 빌더가 USB
묶음을 요구하지만 이 전용 init은 설치기나 제품 preload를 실행하지 않습니다.
원본 D-Bus·일부 커널 모듈·aap_service와 진단 프로그램을 실행합니다.
baseline/session 인자가 다르면 진단을 거부합니다.

`run`의 시간 제한 종료 124와 QEMU 종료 0은 검사 통과 기준이 아닙니다.
VM 종료 후 `check-session`으로 두 생성·identity·송신·시작·정지·파괴 주기,
콜백 대응과 개수, 명시적인
프로그램 종료 0을 확인하십시오. `complete=true`는 이 로컬 API 검사 범위가
끝났다는 뜻입니다. `states`와 원본 서비스 오류도 함께 읽어야 하며
`phone_acceptance_verified`와 `vehicle_validation`은 계속 false입니다.
정지 API 실패나 원본 정리 오류가 없는 정상 AA 세션이라는 판정은 하지 않습니다.

원본 SDK가 줄바꿈 없이 출력한 접두어 뒤의 완전한 JSON은 읽습니다. JSON
본문이 섞이거나 잘린 경우, 기록된 개수와 맞지 않는 콜백 누락·순서 모순,
종료 표식 누락은 거부합니다. 실제 콜백 0건은 상태 미확인으로 보고합니다.
NULL 데이터 콜백의 state/detail은 null이며 상태를 관측한 것으로 세지 않습니다.
이전 로그의 `data_nonnull=false`와 숫자 0 조합도 같은 미확인으로 해석합니다.
start/stop의 원본 반환은 별도로 표시합니다. 이 API들을 시도하고 기록했다는
뜻이며, 반환 0이나 로컬 검사 완료가 폰 연결 성공을 뜻하지 않습니다.
빌드 sidecar·실행 metadata·console·프로그램과 작성 소스의 해시를 보존하십시오.
OEM 이미지와 전체 로그는 공개하지 마십시오.

`tests/run_arm_all.sh`의 작성 callback 회귀는 위 실제 probe 소스를 사용하되
OEM main을 실행하지 않습니다. 테이블을 보관하는 작성 API와 두 세대·늦은
콜백·NULL 입력, 전체 payload와 errno·반환값을 검사합니다. 원본 VM 실행과
구분하십시오. 원형은 독립 브랜치 `76ea17a`의 작성 진단이며 후속 판정기 수정과
검사 결과는 새 validation 기록에 별도로 남깁니다.
