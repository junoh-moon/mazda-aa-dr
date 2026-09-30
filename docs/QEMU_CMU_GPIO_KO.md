# 원본 LDS 조사를 위한 실험용 GPIO 모델

개발 PC의 격리 QEMU에서 i.MX6Q GPIO 두 핀의 출력 되읽기를 조사하는 도구입니다.
제품 라이브러리와 설치 ZIP에는 포함하지 않습니다. 완전한 CMU 보드 모델, 전기적
정상 상태, 유효 GPS, 관성항법 정확도나 폰/앱 수용을 증명하지 않습니다.

QEMU 7.2.22는 GPIO PSR을 `psr & ~gdir`로 읽어 출력 핀의 값을 0으로 가립니다.
해당 SoC의 IOMUXC 핀 설정도 구현되어 있지 않습니다. 원본 NA 74.00.324A 커널은
GPIO 값을 PSR에서 읽으며, 아래 두 핀에 실제 `ALT5 + SION` 설정을 씁니다.
출력 레지스터의 1 쓰기 이후에도 sysfs 읽기는 0이어서 원본 LDS의 수신기 대기가
계속됐습니다. 상세 실행과 한계는 [검증 기록](../validation/LDS_GPIO_2026-09-30.md)을 따릅니다.

| 핀 | GPIO | mux 레지스터 |
| --- | --- | --- |
| EIM_D31 | GPIO3_IO31 | `0x020e00d0` |
| KEY_ROW4 | GPIO4_IO15 | `0x020e021c` |

패치는 이 두 mux 레지스터만 추가합니다. `ALT5 + SION`이고 GDIR이 출력일 때
PSR의 해당 비트를 DR에서 읽습니다. 입력 방향에서는 기존 외부 입력을 읽으며,
다른 핀은 기존 동작을 유지합니다. 값은 고정하지 않고 원본 펌웨어의 설정과
출력을 따릅니다. reset은 SION을 해제하며 migration은 차단합니다.

**미구현 범위:** 다른 IOMUXC 레지스터, pad 전압·외부 부하·단락·open-drain,
입출력 전파 지연과 이 되읽기에 따른 interrupt는 모델링하지 않았습니다.
두 핀을 외부 부하가 없는 디지털 출력으로 가정한 실험용 모델입니다. 원본의
GPS/USB 장치가 준비됐다는 응답이나 위치를 대신 만들지 않습니다.

근거는 [QEMU GPIO 소스](https://gitlab.com/qemu-project/qemu/-/blob/v7.2.22/hw/gpio/imx_gpio.c),
[NXP의 SION 설명](https://community.nxp.com/t5/i-MX-Processors/How-to-use-the-SION-bit/m-p/185519),
[NXP i.MX6Q mux 정의](https://github.com/nxp-imx/linux-imx/blob/imx_3.0.35_4.1.0_caf/arch/arm/plat-mxc/include/mach/iomux-mx6q.h)와
원본 커널의 실제 설정 관측입니다. 공개 하드웨어 주소를 사용하며 OEM 바이너리나
덤프는 이 도구에 포함하지 않습니다.

## 빌드와 검사

호스트 설치 목록을 먼저 보존하고 폐기 가능한 전용 컨테이너에서 빌드하십시오.
필요한 빌드 도구는 C/C++ 컴파일러, Python 3, Meson, Ninja, pkg-config,
glib2·pixman·libfdt·zlib 개발 패키지와 xz입니다. 작업 후 컨테이너·소스·빌드 도구와
임시 폴더를 제거하고 호스트 목록을 대조하십시오.

[QEMU 7.2.22 원본 archive](https://download.qemu.org/qemu-7.2.22.tar.xz)의 SHA-256은
`c74b398c1950526686cb7784e23bd8945ef138b0e6541ba0495015aad3ac9532`입니다.
압축을 푼 QEMU 소스 디렉터리에서 저장소의
[패치](../tests/packaging/qemu-7.2.22-cmu-gpio-readback.patch)를 적용하십시오.
패치 대상 코드는 QEMU의 기존 GPL 조건을 따릅니다.

```sh
patch -p1 < /path/to/mazda-aa-dr/tests/packaging/qemu-7.2.22-cmu-gpio-readback.patch
printf '%s\n' CONFIG_SABRELITE=y CONFIG_ARM_V7M=y > configs/devices/arm-softmmu/cmu.mak
./configure --target-list=arm-softmmu --without-default-features \
  --enable-tcg --enable-fdt=system --without-default-devices \
  --with-devices-arm=cmu --enable-pie
ninja -C build -j4 qemu-system-arm
python3 /path/to/mazda-aa-dr/tests/packaging/qemu_cmu_gpio_check.py \
  --qemu "$PWD/build/qemu-system-arm" --enable-model
```

`CONFIG_ARM_V7M`은 이 QEMU 버전의 공통 ARM TCG 링크에 필요합니다. CMU의 CPU를
Cortex-M으로 바꾸는 설정은 아닙니다. 검사는 CPU를 정지한 상태에서 실행하며
펌웨어·물리 장치·외부 네트워크를 사용하지 않습니다. 출력의 양쪽 값, 방향,
SION·mux 조건, 기존 입력, 다른 핀, reset과 기본 비활성 동작을 검사합니다.

모델을 활성화하려면 QEMU 실행에 아래 옵션을 명시하십시오. 기본값은 비활성입니다.
기존 `oem_system_emulation.py`는 이 옵션을 자동으로 넣지 않습니다. 원본 VM의
비교 실험에서는 실행 명령·QEMU 바이너리 해시·모델 활성 여부를 별도로 기록합니다.

```text
-global fsl-imx6.experimental-cmu-gpio-readback=on
```
