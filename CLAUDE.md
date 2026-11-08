# libfprint-CS9711 작업 지침

`CLAUDE.md`가 공통 지침의 원본이다. 루트 `AGENTS.md`는 `CLAUDE.md`를
가리키는 상대 심볼릭 링크로 유지한다.

## 목적과 범위

- LANStar LS-WINFG / USB FingPintAcl16 (`2541:2549`)을 libfprint와
  fprintd로 사용하기 위한 포크다. 기반 버전은 libfprint 1.94.10이다.
- 기본 브랜치는 `main`이다. 기존 CS9711 장치 `2541:0236`, `2541:9711`의
  동작도 보존한다. 이 두 장치는 현재 실물 검증 없이 단위 테스트만 있다.
- 가벼운 드라이버와 진단 도구, 제거 가능한 RPM이 범위다. 별도 인증
  프레임워크나 GUI는 요청 없이 추가하지 않는다.
- 한국어로 간결하게 소통하며 확인한 사실과 추정을 구분한다.
- 셸 명령은 `rtk`로 실행한다. 원본 출력이 필요하면 `rtk proxy`를 쓴다.

## 작업 분담과 환경

- Codex는 요구사항 조율, 환경·설정, 코드 리뷰, 실제 장치 QA와 설치·제거를
  맡는다. 제품 코드 설계·구현과 관련 테스트 변경은 Claude Code가 맡는다.
  Claude Code가 구현을 위임받은 경우 그 범위는 직접 수행한다.
- 실제 USB나 시스템 서비스를 다루기 전 `~/.codex/ENVIRONMENT.md`가 있으면
  읽고, 현재 호스트·OS·USB ID를 확인한다. 이전 USB 버스 번호나 ACL을 재사용하지 않는다.
- 같은 저장소에서 다른 작업자가 만든 변경을 되돌리지 않는다. 커밋과 푸시는
  각각 사용자가 요청한 범위에서만 수행한다. `gh` 사용에는 별도 명시적 허가가 필요하다.

## 주요 파일

| 경로 | 역할 |
| --- | --- |
| `libfprint/drivers/cs9711/cs9711.{c,h}` | USB 프로토콜, Acl16/기존 장치 분기, 상태 머신 |
| `libfprint/fpi-image-device.c`, `fpi-print.c`, `fp-image.c` | 이미지 처리·매칭·객체 수명 |
| `libfprint/sigfm/` | 호스트에서 실행되는 SIGFM/OpenCV 매처 |
| `examples/cs9711-diag.c` | 공개 API를 사용하는 JSON Lines 진단 CLI |
| `tests/test-cs9711-driver.c` | 합성 이미지와 가짜 USB 전송을 이용한 회귀 테스트 |
| `tests/test-cs9711-variants.c` | 장치 ID별 설정 검증 |
| `packaging/rpm/` | RPM spec, 빌드 스크립트, fprintd drop-in, 설치·QA 문서 |

`HACKING.md`의 API·코딩 지침을 따른다. 기존 C/GLib 스타일과
Meson 테스트 구조를 유지하고, 공용 API와 내부 `fpi_` API를 구분한다.

## 드라이버 변경 시 지킬 조건

- Acl16의 지문 인터페이스는 1, 기존 CS9711 장치는 0이다.
  Acl16 프레임은 8192바이트이며 이미지 영역은 112×73이다.
- Acl16은 스캔 후 RESET부터 다음 명령까지 최소 1.5초의 대기를 유지한다.
  fprintd의 중복 확인 스캔 → 등록처럼 연속되는 작업도 포함한다.
  타이머가 일찍 깨어날 수 있으므로 단조 시계로 종료 시점을 재확인한다.
- 실제 SCAN 명령을 보낸 뒤에 입력 대기를 알린다. 실제 프레임 수신 후에만
  손가락 감지를 보고한다. 시간 초과를 불일치 결과로 취급하지 않는다.
- 취소·오류 시 진행 중인 USB 읽기를 회수한 뒤 상태 머신과 장치를 정리한다.
  오류를 버리거나 완료 콜백 없이 작업을 종료하지 않는다.
- SIGFM 점수 기준을 낮춰 성공을 만들지 않는다. 잘못된 입력이나 지원하지
  않는 알고리즘은 인증 실패·오류로 처리한다. 이미지와 템플릿 소유권을 명확히 한다.

## 빌드와 검증

필요한 Fedora 빌드 의존성은 `packaging/rpm/libfprint-cs9711.spec`의
`BuildRequires`를 기준으로 한다. 설치된 도구부터 확인하고 시스템 패키지를
임의로 교체하지 않는다. 로컬 드라이버 빌드 예:

```sh
rtk proxy meson setup _build-cs9711 -Ddrivers=cs9711 -Dintrospection=false -Ddoc=false -Dgtk-examples=false -Dudev_rules=disabled -Dudev_hwdb=disabled -Dinstalled-tests=false
rtk proxy meson compile -C _build-cs9711
rtk proxy meson test -C _build-cs9711 --suite unit-tests --print-errorlogs
rtk proxy sh packaging/rpm/make-srpm.sh --binary
```

- 기존 빌드 디렉터리가 있으면 설정부터 확인한다. 빌드 결과물은 `_build*`,
  `build*`, `packaging/rpm/out/` 아래에 둔다.
- USB 수명·타이밍 변경에는 취소, 시간 초과, 연속 작업, 재사용 경로를
  검증하는 회귀 테스트를 추가한다. 관련 변경은 ASAN/UBSAN도 확인한다.
- 단위 테스트, 가상 장치, 실물 장치 결과를 구분한다. 실물 변경 검증에는
  등록, 같은 손가락 일치, 다른 손가락 불일치, 재시도와 취소를 포함한다.
- 손가락 입력은 실제 `waiting-for-contact`/`finger-needed` 확인 후 요청한다.
  진단 CLI와 fprintd가 동시에 장치를 점유하지 않게 한다.
- 날짜·패키지 해시·환경이 포함된 검증 기록은 `packaging/rpm/QA.json`을
  참고한다. 이전 결과를 새 빌드의 검증으로 주장하지 않는다.

## 설치와 데이터 보호

- 기본 RPM은 `/usr/lib64/libfprint-cs9711` 아래에 설치한다. 배포판
  libfprint/fprintd 파일, 전역 링커 설정, PAM/authselect, udev 규칙,
  SELinux 정책을 덮어쓰거나 완화하지 않는다.
- fprintd 연결은 선택적 `libfprint-cs9711-fprintd` 패키지의 서비스 전용
  drop-in으로만 한다. 기본 패키지를 전역 libfprint SONAME 제공자로 등록하지 않는다.
- 설치·제거는 패키지 관리자를 통해 수행한다. 파일 소유권, 의존성,
  기존 파일 해시, 서비스가 실제 로드한 라이브러리와 원상복구를 확인한다.
  기존 오프라인 업데이트가 있으면 해당 상태도 보존한다.
- 지문 원본·템플릿·USB 프레임 덤프를 Git, 로그, 외부 모델에 전달하지 않는다.
  테스트는 합성 이미지를 쓴다. 실물 테스트 임시 데이터는 비공개 임시 경로에
  두고 삭제한다. 진단 CLI의 템플릿 저장은 명시적으로 요청한 경우에만 사용한다.
- `/var/lib/fprint`의 등록 데이터는 사용자의 인증 데이터다. 패키지 제거 시
  삭제하지 않는다. 기존 `enroll`/`verify` 예제는 이미지와
  `test-storage.variant`를 저장할 수 있으므로 저장소 루트에서 실물 실행하지 않는다.
- 커밋에는 소스·테스트·설정·문서만 포함한다. 빌드 디렉터리, RPM, 소스 압축파일,
  임시 로그, 지문 데이터는 제외한다. 설치된 RPM과 과거 QA 해시는 소스 커밋 후에도
  과거 검증 대상을 가리키며, 새 빌드 결과로 바꾸어 기록하지 않는다.
