# LDS 응답의 필드 할당 출처 진단 — 2026-10-02

원본 `GetPosition`의 아홉 필드는 캐시 한 번의 읽기 사본이지만, 원본 RMC·
GGA·GSA callback의 여러 쓰기에서 남은 값일 수 있습니다. [원본 필드 실행](LDS_FIELD_LINEAGE_2026-10-01.md)은
B GGA 좌표와 A RMC UTC가 같은 응답에 들어오는 경우를 보였고,
[VM 재생](LDS_WFI_REPLAY_2026-10-01.md)에서도 경계의 UTC·좌표 혼합을
분리했습니다. 이 결과는 물리 수신기의 정확도나 실제 차량 측정이 아닙니다.

기존 분석기는 원본 AA POSITION과 LDS sideband를 완전한 여섯 wire 식별자와
본문으로 사후 연결하지만, 연결된 응답의 필드 할당 출처 다양성은 요약하지
않았습니다. 이제 `lds_sideband.assignment_patterns`가 **연결된** 응답에 한해
`field_write_sequences`의 비영(非零) 출처 번호를 분류합니다. 각 첨부에는
필드 출처가 모두/일부/전혀 알려졌는지와 남아 있는 서로 다른 할당 출처
번호가 몇 종류인지 표시합니다. 요약은 두 축을 결합한 범주라 256건의 첨부
표시 제한을 넘어도 출처 패턴의 대응이 유지됩니다. 관측 시각이 같아도
서로 다른 쓰기 번호를 합치지 않습니다.

모든 필드 출처가 0인 경우도 세 가지로 구분합니다. 관측 Ledger 수명 자체가
없으면 `observation_lifetime_unavailable`, 수명 안의 쓰기 번호가 0이면
`no_tracked_cache_write_in_lifetime`, 쓰기 번호가 양수인데 필드 출처가
지워졌으면 `no_known_field_assignment`입니다. 마지막 경우는 실제 쓰기가
없었다는 뜻이 아닙니다. 부분적으로 알려진 필드의 출처 한 종류도 전체
캐시의 쓰기 횟수로 표현하지 않습니다. 수명 0인데 양수 쓰기 번호를 가진
불가능한 레코드는 malformed로 분리하고 원본 POSITION 분석은 보존합니다.

이 추가 분석은 제공된 파일 범위에 한정한 **사후 진단**입니다. 파일 끝이
실제 세션 종료나 누락 없는 관측의 증명은 아니며, 뒤늦은 행을 추가하면
앞서 표시한 match도 충돌로 바뀔 수 있습니다. 같은 worker의 live
POSITION/LDS 공급부, 생산자의 물리 측정 시각·품질, 실제 제공자·세션·수신기
자격을 구현하지 않았습니다. 즉시 내보내는 live join은 뒤늦은 중복·재시도·
유실·세션 경계를 볼 수 없어 판정을 철회해야 할 수 있습니다. 원본 callback이
실행된 뒤 도착한 sideband로 그 callback의 출처를 소급 자격화하지 않습니다.
`matched`나 할당 출처 한 종류를 신선한 GPS fix 또는 ASSIST 준비로 읽지
않으며 제품의 live ASSIST는 계속 비활성입니다.

Claude CLI의 읽기 전용 적대적 설계·코드 리뷰와 Codex 읽기 전용 리뷰 세
갈래가 초기 라벨 `no_cache_write`/`one_cache_write`의 과대 해석을
지적했습니다. 실제 캐시 쓰기 번호가 양수여도 필드 출처가 0일 수 있어
해당 라벨을 없앴고, 관측 수명·연결 분모를 명시했습니다. 리뷰어는 파일을
수정하지 않았습니다. 이 범위에는 비공개 자료를 외부에 전달할 필요가
없어 공개 소스와 사실 요약만 Claude에 제공했습니다.

Linux 검증 컨테이너에서 제품 LDS hook·formatter fixture를 새로 빌드해
관련 Python 회귀 28건을 통과했습니다. 최종 소스 상태의 전체 `make test`는
종료 코드 0으로 끝났고 Python 580건(6개 그룹)과 C/C++·통합 검사가 모두
통과했습니다. 실패·오류·건너뛴 검사 표시는 없었습니다. 검사는 기존 stock
rootfs와 `v0.3.10-shadow.2` 배포 묶음을 fixture로 사용했습니다. 이 변경은
PC 분석기와 테스트·문서뿐이며 새 ARM/QEMU 검사, 새 설치 ZIP 제작 또는
원본 OEM·차량 실행은 하지 않았습니다.
기존 실차 archive에는 trace·collector JSONL이 없어서 새 분류를 적용할
실차 응답도 없습니다. [v1.0 조건](../docs/V1_READINESS_KO.md)의 요청별
provider/receiver/session 연결과 센서 시간·품질, 실제 보정 위치 송신·폰
수용은 그대로 미완료입니다.
