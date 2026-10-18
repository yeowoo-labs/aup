# aup

aup는 [Pure](/srv/git/pure.git) 런타임을 위한 **언어와 툴체인**이다. Java와 javac가 JVM에
대해 하는 역할과 같다. aup 소스를 검사해 Pure가 검증한 뒤 실행하는 모듈(`.pmod`)로 만든다.

| 층 | 역할 |
|---|---|
| Pure | 런타임. 모듈 형식과 검증기·실행기, 실행·메모리·영속 상태·통신 |
| **aup** | 언어와 툴체인. 소스 → 진단 → Pure 모듈 |
| [Vega](/srv/git/vega.git) | aup와 Pure 위의 인터페이스 구현자(TUI·CLI·GUI·NUI) |

상태: v0a(scalar 상태·키 입력·원자적 이벤트·보기), v0b(바이트·영속 문서·메시지·응답·text·커서),
v0c(레코드·리스트·`index of`·조건식·표 보기·`fn`·`use` 라이브러리·집계·키 있는 리스트·text 비교·`bool`·`i64`) 동작. 그 위의 첫 인터페이스는 [Vega](/srv/git/vega.git)(`use vega`, 목록·표·톤을 TUI·CLI·문장으로).
[언어 v0](docs/design/001-language-v0.md).

```sh
make                                   # build/aup
build/aup build examples/counter.aup -o counter.pmod
build/aup watch examples/counter.aup -o counter.pmod   # 저장할 때마다 다시 빌드(vega ... --watch가 받아 바꾼다)
printf 'down tap\nup tap\nshow\n' | ../pure/build/pure-run counter.pmod
build/aup build examples/notes.aup -o notes.pmod
printf 'type typed %s\ndown left\nshow\n' "$(printf '안녕' | xxd -p)" | ../pure/build/pure-run notes.pmod
build/aup build examples/tasks.aup -o tasks.pmod
printf 'type typed %s\ndown again\ndown toggle\nshow\n' "$(printf '우유' | xxd -p)" | ../pure/build/pure-run tasks.pmod
../pure/build/pure-run tasks.pmod --store /tmp/tasks < /dev/null   # 상태는 저장소에 남는다
build/aup build examples/documents.aup -o documents.pmod  # 키 있는 리스트의 문서, 리비전으로 확인하는 편집
make check                             # Pure 검증, 결정성, 진단, 워크벤치·text·리스트·문서 참조 모델과의 대조, 재시작을 끼운 영속 대조
```

`make check`는 옆의 Pure 체크아웃(`PURE=../pure`)의 검증기와 시험 호스트를 쓰고,
워크벤치 대조에는 Node가 필요하다.

## 원칙

- **앱은 상태·입력·반응·보기를 선언하고, 저장·트랜잭션·화면 모델은 런타임이 소유한다.**
- 컴파일 결과는 Pure가 로드 전에 검증할 수 있는 형식이어야 한다. 소스나 AST를 런타임에 두지 않는다.
- 툴체인은 C17로 작성하고 외부 의존성을 두지 않는다. 검증 기기는 로컬 Raspberry Pi 5다.
- 기존 Pure `.pure` 두 방언(워크벤치 scalar, 문서 연결)을 aup 하나로 대체했다. 대체의 근거는 기존 실행기와의 비트 단위
  일치였다. 문서 연결의 키별 문서는 Pure 027의 4b에서 키 있는 리스트로 옮겨 갔다(`examples/documents.aup`).
