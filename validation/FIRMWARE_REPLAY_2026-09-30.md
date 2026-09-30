# 제공 펌웨어의 로컬 재실행과 임시 도구 정리 — 2026-09-30

사용자가 제공한 NA 74.00.324A 업데이트에서 순정 커널과 rootfs를 직접 추출하고,
현재 제품을 새로 빌드하여 실행했습니다. 호스트·ARM 합성 검사는 통과했습니다.
원본 커널 VM에서는 순정 위치 API 응답, 제품 후크 설치와 SHADOW 초기화를
확인했습니다. 물리 센서와 AA LOCATION 표본이 없어 로그 분석은 **inconclusive**입니다.
정상 전체 차량 기동, DR 정확도, 폰 수용 또는 v1.0 완료로 세지 않습니다.

제품 소스와 공개 ZIP은 변경하지 않았습니다. 기준 커밋은
`2b959ff713a259bebe36ebe7f7dfd8da9ebed449`입니다. 이번 기록은 과거 시험의
집계나 실행 범위를 소급 변경하지 않습니다.

## 고정 입력과 새 빌드

| 입력·산출물 | SHA-256 |
| --- | --- |
| 사용자가 제공한 외부 ZIP | `20b7089f37652e095225486dea696d5f7264f57d47fa6667ad94191f27bbe84e` |
| 내부 `cmu150_NA_74.00.324A_update.up` | `ffd04e2c8cfaf77388aacde0f9c1cddc17cb6b7f02d7caa2fe6ad39c0f40e787` |
| 추출한 순정 zImage | `57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240` |
| 추출한 rootfs tar.gz | `61bbcea608cc915f45a1775d4f49fb1603e4d92a0163311ced15220d03cf9d44` |
| 새 제품 `libmx5dr.so` | `42568553215bea8ef9998add0ba6e0f208c438455cfb576cfae4e808d883331b` |
| 이번 진단 initramfs | `229a97ebd613cc71702359cdb22e65096a2b9caf9ae778724fa6b4c415758202` |

업데이트의 선택된 ZIP 항목과 gzip 스트림을 검사했습니다. rootfs payload의
이중 gzip을 해제했고, 커널은 payload의 512바이트 뒤에서 zImage 헤더 길이
2,404,776바이트를 추출했습니다. 추출 결과는 기존 원본 실행 기록의 커널·rootfs
해시와 일치합니다. 업데이트 스크립트를 호스트에서 실행하지 않았습니다.

`packaging/firmware.sha256`의 네 파일을 모두 대조했습니다. VIM·IPC·VBS와
JCIDBUS·LDS·common-util 및 순정 libc/loader/C++ runtime의 해시도 별도로
보존했습니다. 실제 rootfs에는 glibc 2.11.1이 있습니다.

고정 m3-toolchain 커밋 `61ec0343de84f6fc7c46840056df1d600d44be8a`의
2,124개 blob을 검증하고 GCC 4.9.1로 새 빌드 디렉터리에 빌드했습니다.
다섯 ARM 산출물은 [요청 관측 제품 연결 기록](REQUEST_PRODUCT_2026-09-30.md)의
최종 빌드와 바이트 단위로 같습니다. 이전 빌드 파일을 복사하여 검사하지 않았습니다.

## 호스트·ARM 검사

- 전체 `make test`: C/C++ 실행과 Python 270개 통과, 생략 0입니다.
  Python 집계는 build 41, journal 53, recovery 28, loader 1, collector 10,
  packaging 107, analyzer 30입니다. 추출한 stock rootfs와 새 SHADOW bundle을
  명시했습니다. 기존 문서의 다른 집계를 이번 결과로 바꾸지 않습니다.
- 전체 `tests/run_arm_all.sh`: 통과했습니다. 시작·종료의 입력 검사에서
  `release_verified=true`, 도구체인·sysroot·다섯 산출물 해시를 확인했습니다.
  실제 제품 DSO를 통과하는 작성 position 예외·취소 8개와 request 13개도
  포함합니다. 이 ARM 합성 시험은 순정 서비스 실행과 별개입니다.
- 첫 호스트 실행은 컨테이너 UID와 복제본 소유자가 달라 Git clone 검사 8개가
  중단됐습니다. 컨테이너 안에서 해당 복제본과 `.git` 경로만 `safe.directory`로
  허용한 뒤 전체 검사를 다시 실행했습니다. 제품 코드 수정은 없으며 실패 로그도
  보존했습니다. fixture 디렉터리는 `umask 022`로 생성했습니다.

## 원본 커널 VM 비교

QEMU 7.2.22/Sabrelite, 순정 Linux 3.0.35와 같은 진단 initramfs를 사용했습니다.
저장소의 `oem_system_emulation.py`, `oem_guest_init.sh`를 수정 없이 실행했습니다.
기존 hardware breakpoint 방식으로 커널 진입의 r1만 machine ID 3837로 바꾸고
전체 register packet을 대조한 뒤 debugger를 분리했습니다. 커널 코드와 OEM
함수·반환값은 변경하지 않았습니다. guest NIC·호스트 장치·공유 디렉터리는 없습니다.

진단 PID1의 `--phase location`은 원본 launcher로 서비스를 개별 시작합니다.
순정 SM의 전체 의존성 그래프나 정상 OEM PID1 기동을 실행한 시험이 아닙니다.

| 관측 | 순정 baseline | 현재 SHADOW |
| --- | --- | --- |
| 원본 위치 서비스 시작 전 | owner 없음, 위치·제어 요청은 ServiceUnknown | 같은 상태 |
| 제공자 시작 뒤와 후반 위치 조회 | 각각 실제 응답, 아홉 수치 필드 모두 0 | 같은 상태 |
| read status | 두 조회 모두 5/READ_NOT_READY | 같은 상태 |
| 제품 실행 매핑 | 없음 | AA DSO와 VBS tap DSO 매핑 |
| 설치·일회 가드 | 제품 설치 안 함 | 설치 완료, guard select=0 |
| 제품 초기화 | 해당 없음 | `install=ok`, SHADOW capture/calculation active |
| collector | 해당 없음 | UID/GID 1001, 위치 poll 18건, 초기 오류 5건, stop marker로 종료 |
| 콘솔 전체 출력 종료 표식 | 140초 제한 전에 확인 못함 | 200초 제한 전에 inspection shell 진입 확인 |

각 실행에서 위치 서비스 진단 조회 12개의 시작·종료와 반환을 대조했습니다.
위치 응답은 실제 `dbus-send`와 collector의 요청 결과입니다. BLM manager가
만든 AA 위치 요청이나 물리 GPS 측정값으로 바꾸어 해석하지 않습니다.

SHADOW journal에는 boot 1, shadow_boot 1, health 78, shadow 587,
shadow_calibration 78개가 있습니다. 기록된 health의 drop·audit fault는 모두
0입니다. 요청 Observer는 준비됐고 ABI fault/loss는 없지만 **요청·worker 관측은
0개**입니다. 초기화만으로 실제 요청 수명 검증을 완료했다고 하지 않습니다.

센서 motion과 AA position/send 표본은 없습니다. SHADOW 587개는 모두
`model_valid=false`, `E_NO_SEED`이며 ASSIST는 비활성입니다. 실제 분석기는
`no_location_samples`로 종료 2/inconclusive를 반환했습니다. collector의 성공한
poll도 producer 측정 시각이나 fresh/VALID 센서를 만들지 않습니다.

두 runner는 각각 140.219초·200.477초 제한으로 종료 124, QEMU는 종료 0입니다.
이를 통과 근거로 사용하지 않습니다. baseline의 뒤쪽 진단 출력은 완결되지 않았고,
SHADOW의 journal 블록과 inspection shell 표식은 확인했습니다. 수집 완료
`capture_stopped` 기록은 없습니다. 종료 뒤 console로 freeze를 요청하려던
시도는 이미 QEMU가 없어 전달되지 않았으며, 실제 실행으로 세지 않습니다.

| 콘솔 | SHA-256 |
| --- | --- |
| baseline | `1698b234f4737b49ff4a09c95fa28c9e9fd2338729de6a53b4e5f10d37b24838` |
| SHADOW | `f9456cf41bf13e66296632d1fed3a229eb53d89b6a8b3048be4e57968a15cae0` |

## 실행하지 못한 범위

실제 폰 연결, 유효 GPS·휠·yaw·후진, DR 오차, 정상 전체 SM 기동, 전원 차단과
다음 부팅 복귀는 이번 시험에 없습니다. 기존 사용자 touch DSO도 제공되지 않아
공존 검사를 재실행하지 않았습니다.

**TODO:** 기존 [전체 manager 취소 비교](MANAGER_CANCELLATION_2026-09-30.md)의
전용 외부 caller·실행 재료는 이 checkout에 없습니다. 이번은 저장소에 있는
`location` 실행기의 재현이며, 그 전용 caller를 새로 구현하거나 복원하지
않았습니다. 따라서 manager의 자동 요청→worker→send, 반복 취소 timeout,
같은 이름의 재연결과 원본 세션 정리 오류는 이번에 재현·해결한 항목이 아닙니다.
이전 실행 근거와 이번 재현 범위를 구분합니다. 독립 에이전트·Claude 검토도
이번 작업에서 실행하지 않았습니다.

## 추가 도구와 정리

기존에 캐시되어 있던 Docker 이미지를 사용했습니다. 새 이미지를 받거나
호스트 패키지를 설치하지 않았습니다. 전용 컨테이너에만 QEMU system/user,
네이티브 빌드 도구, D-Bus 개발 파일, archive 도구 등을 설치했습니다.
설치 전후 dpkg 목록과 추가 129개·변경 6개 패키지의 이름·버전을 보존했습니다.
고정 ARM 도구체인과 qemu-arm 링크도 그 컨테이너 안에만 만들었습니다.
도구 준비 뒤 컨테이너 네트워크를 제거하고 OEM 실행을 진행했습니다.

검증 자료의 복사와 이미지 해시 대조 뒤 전용 컨테이너, 도구체인, 임시 복제본·
추출본·빌드·USB 묶음을 모두 삭제했습니다. 처음 정리에서 남은 USB 디렉터리도
별도 `--rm --network none` 컨테이너로 제거했습니다. 작업 컨테이너·임시 작업
폴더의 부재, Docker 이미지·컨테이너 목록의 원복과 호스트 PATH에 QEMU가
추가되지 않았음을 확인했습니다. 기존 기반 이미지는 유지했습니다.

비공개 증거는 `evidence/firmware-replay-20260930-fq9n2k/`에 보존합니다.
`results/tool-lifecycle.json`, `packages-added.tsv`, `packages-updated.tsv`,
실행·실패 로그, 분석기 결과, 빌드 manifest, 재현용 이미지와 추출·검사 스크립트를
포함합니다. 전체 디렉터리는 Git에서 제외되며 접근 권한은 0700입니다.
원본 ZIP은 그대로 유지했습니다. OEM 파일·전체 콘솔·이미지·추출 암호는
공개 변경에 포함하지 않습니다.
