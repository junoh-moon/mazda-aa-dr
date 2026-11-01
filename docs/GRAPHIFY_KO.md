# 프로젝트 전용 Graphify

Codex CLI, Claude Code, OpenCode에서 이 저장소의 코드 관계를 조회하기 위한
개발 도구입니다. 차량에 설치하는 제품이나 릴리즈 묶음과는 무관합니다.

## 설치 범위

- CLI: `./tools/graphify`가 `uv tool run --isolated --from graphifyy`로 실행합니다.
  Python 패키지와 의존성은 uv 캐시의 격리 환경에 있으며 시스템 Python,
  프로젝트 가상환경, `~/.local/bin/graphify`에는 설치하지 않습니다.
- Codex CLI: `.agents/skills/graphify/SKILL.md`에서 공통 절차를 읽습니다.
- Claude Code: `.claude/skills/graphify/SKILL.md`와 루트 `CLAUDE.md`를 읽습니다.
- OpenCode: `.opencode/skills/graphify/SKILL.md`와 루트 `AGENTS.md`를 읽습니다.
- 사용자 전역 스킬·설정, Git 훅, MCP 서버, 상주 감시기는 추가하지 않습니다.
  위 스킬은 공식 CLI를 사용하는 프로젝트 전용 절차이며, 전역 설치를
  자동 수행하는 upstream 스킬을 그대로 복사한 것은 아닙니다.

저장소 루트에서 실행하십시오. 실행 스크립트 자체는 다른 작업 디렉터리에서
절대 경로로 호출해도 이 저장소를 작업 디렉터리로 선택합니다. 에이전트가
이미 실행 중이라면 세션을 다시 열어 새 프로젝트 스킬을 발견하게 하십시오.

## 생성과 조회

```sh
./tools/graphify extract . --code-only --max-workers 2
./tools/graphify cluster-only . --no-label
./tools/graphify query "G3 heading carry and NO_FIX bearing" --budget 1500
./tools/graphify explain "G3Motion"
```

최초 실행에는 패키지를 내려받기 위한 네트워크가 필요합니다. 코드 분석은
로컬 tree-sitter AST로 수행합니다. `GRAPHIFY_BACKEND=none`으로 외부 LLM의
자동 선택을 끄며 사용자 전역 질의 로그도 끕니다. 문서·이미지·PDF 의미
분석은 이 구성에 포함하지 않습니다. 현재 상태·제약은 `AGENTS.md`가 지정한
원문 문서를 직접 읽으십시오.

`.graphifyignore`는 소스·시험·설치 스크립트·도구·폰 코드·문서·검증 기록
디렉터리만 탐색하도록 제한하고 `.gitignore`도 적용합니다. 비공개 자료,
다른 worktree, OEM/APK 분석물과 생성물은 제외합니다. 루트 기준 경로 구성
요소가 9개인 파일·디렉터리를 제외하여 재귀 탐색의 최대 깊이를 8로
제한합니다. 새 최상위 소스 디렉터리나 더 깊은 파일은 자동 포함되지 않습니다.

결과는 Git에서 제외한 `graphify-out/` 안에 저장합니다.

- `graph.json`: 질의에 사용하는 그래프입니다.
- `GRAPH_REPORT.md`: 전체 구조를 살펴보는 보고서입니다.
- `graph.html`: 브라우저에서 여는 시각화입니다.
- manifest/cache 등: 다음 실행의 증분 갱신에 사용하는 로컬 생성물입니다.

코드를 수정한 뒤 같은 `extract` 명령을 실행하면 증분 갱신합니다.
`cluster-only . --no-label`은 외부 모델의 이름 붙이기 없이 보고서와 HTML을
생성합니다. 노드가 5,000개를 넘으면 HTML은 기본적으로 커뮤니티를 묶어
표시하며, CLI 질의는 전체 그래프를 사용합니다. 전체 재분석이 필요하면
`extract`에 `--force`를 추가하십시오. 검색 결과가 없거나
그래프가 오래되었다면 범위를 제한한 원문 검색으로 확인하십시오.
추론된 연결과 호출 관계는 실행·실차 검증을 대신하지 않습니다.

## 버전 업그레이드

버전을 고정하지 않습니다. 다음 명령으로 uv의 실행 환경을 갱신하십시오.

```sh
uv tool run --isolated --upgrade --from graphifyy graphify --help
```

업그레이드 뒤에는 위 생성·조회 명령으로 호환성을 확인하십시오. 이 명령은
스킬이나 프로젝트 지침을 재설치하지 않으므로 세 도구의 적용 범위를
사용자 전역으로 넓히지 않습니다. 공식 패키지 이름은 `graphifyy`입니다.

## 제거

이 변경에서 추가한 실행 스크립트·세 스킬 디렉터리·루트 `CLAUDE.md`·이
문서를 제거하고, `AGENTS.md`의 Graphify 절 및 `.gitignore`와
`.graphifyignore`의 관련 설정을 되돌리십시오. 이후 다른 내용을 추가한
파일은 Graphify 부분만 제거하십시오. 로컬 결과 `graphify-out/`도 삭제할 수
있습니다. uv 캐시는 다른 도구와 공유하므로 전체를 지울 필요는 없습니다.

공식 자료: [Graphify 저장소](https://github.com/Graphify-Labs/graphify),
[설치 안내](https://github.com/Graphify-Labs/graphify#install).
