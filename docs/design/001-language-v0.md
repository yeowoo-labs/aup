# aup v0 — 한 언어, 한 모듈 형식

2026-09-25. 상태: v0a(scalar), v0b(바이트·영속 문서·text·커서), v0c(레코드·리스트·인덱스·조건식·표 보기·fn·use) 구현.
text와 리스트 문법은 [002](002-dsl-direction.md).

착수 기준:

- 바꿀 결정: Pure 앱을 두 `.pure` 방언과 두 실행기(워크벤치 Zig scalar VM, 문서 `PureProgram` 해석기)로
  계속 만들지, aup 한 언어 → Pure 모듈 한 형식 → Pure 실행기 하나로 합칠지.
- 남은 질문: 작은 언어 하나와 형식 하나로 두 앱의 관찰 가능한 동작을 특수 규칙 없이 표현할 수 있는가?
  런타임은 소스를 몰라도 로드 전에 모듈을 검증할 수 있는가?
- 최소 확인: v0a는 워크벤치 예제를 aup로 옮겨, 같은 이벤트열에서 Pure 실행기의 상태가 워크벤치
  Zig 엔진과 비트 단위로 같아야 한다. v0b는 문서 편집 앱을 옮겨 기존 CLI와 저장 결과가 같아야 한다.
- 중단 기준: 한쪽 앱에 그 앱만을 위한 규칙이 필요하거나, Pure 실행기가 기존 두 실행기의 합보다
  커지면 설계를 다시 본다.

## 의심한 관례와 aup의 선택

| 관례 | aup |
|---|---|
| 앱이 저장·불러오기·직렬화 코드를 쓴다 | 상태를 선언하면 런타임이 소유한다(영속화는 v0b) |
| 트랜잭션과 롤백을 앱이 관리한다 | 모든 이벤트가 트랜잭션이다. 성공하면 반영하고, 실패하면 아무것도 바뀌지 않는다 |
| 요청/응답 핸들러와 라우터 | 입력은 이름 있는 이벤트다. 키·CLI·HTTP는 호스트 어댑터가 입력으로 바꾼다 |
| 화면용 모델을 따로 만든다 | `view`는 상태의 순수 함수다. 호스트와 나중의 Vega가 읽어 표시한다 |
| 런타임이 소스를 해석한다 | 툴체인이 모듈로 컴파일하고, 런타임은 검증한 뒤 실행한다(JVM의 class 파일처럼) |
| 이벤트 처리 시간에 상한이 없다 | 반복이 없고 분기는 앞으로만 한다. 이벤트당 실행량의 상한이 모듈에서 정해진다 |

## v0a 문법

```text
# 주석은 줄 끝까지
state count = 0                  # f64 상태. 초기값은 부호 있는 숫자 상수
input tap = key Space            # 키 입력. tap.down, tap.up 이벤트
view label = count               # 상태의 순수 함수
view width = 40 + min(count, 20) * 8

on tap.down {
  count = count + 1
}

on frame {                       # 1/60초 진행. 핸들러 안에서 dt를 쓸 수 있다
  if count > 20 {
    count = 0
  } else {
    count = count + dt
  }
}
```

- 선언과 문장은 줄바꿈이나 `;`로 끝난다. 이름은 ASCII `[A-Za-z_][A-Za-z_0-9]*`, 31바이트 이하다.
- 예약어: `state input view on if else key frame dt min max clamp message by bytes u32 reply require record each fn use`.
- 키: `Space Enter ArrowLeft ArrowRight ArrowUp ArrowDown KeyA..KeyZ Digit0..Digit9`. 입력 이름과
  키는 각각 한 번만 쓴다. 상태·입력·보기의 이름공간은 서로 겹칠 수 없다.
- 식: 숫자, 상태 이름, `dt`(핸들러 안에서만), 괄호, 단항 `- !`, 이항 `* /`, `+ -`,
  `< <= > >=`, `== !=`, `&&`, `||`(뒤로 갈수록 우선순위가 낮다), `min(a,b) max(a,b) clamp(x,lo,hi)`.
- 문장: `이름 = 식`, `if 식 { … }`, `if 식 { … } else { … }`.
- 같은 이벤트의 핸들러가 여럿이면 소스 순서로 하나의 이벤트 안에서 실행한다.

## 의미 (워크벤치 엔진과 같음)

- 값은 IEEE binary64다. 연산 결과나 읽은 값이 유한하지 않으면, 0으로 나누면, `clamp`의 `lo > hi`이면
  이벤트 전체가 실패하고 상태는 이벤트 전과 같다.
- 비교와 논리 연산은 0 또는 1을 낸다. `&& ||`는 양쪽을 모두 계산한다. 0이 아니면 참이다.
- `if`의 조건은 한 번 계산한다. 문장은 순서대로 실행한다.
- `frame`의 `dt`는 1/60이고 키 이벤트의 `dt`는 0이다.
- 한도: 상태 32, 입력 32, 보기 64, 핸들러 64, 명령 512, 스택 64, 소스 32 KiB.
- 모듈을 교체하면 같은 이름의 상태 값은 유지하고, 새 상태는 초기값을 쓰고, 없어진 상태는 버린다.
  교체에 실패하면 이전 모듈과 상태가 그대로다.
- **선언한 상태는 영속 상태다.** 호스트가 저장소를 주면(`pure-run 앱.pmod --store 경로`) 모든 상태(수·text·커서·
  리스트·인덱스)가 저장소에 살고, 앱은 다음 실행에서 그대로 이어진다. 저장·불러오기·직렬화 코드가 없다.
  키·text·메시지 이벤트는 바꾼 상태를 한 커밋으로 기록하고(메시지의 문서와 함께), frame의 변경은 다음 이벤트와
  함께 기록된다. 레코드가 바뀐 리스트는 필드 이름으로 옮겨진다([Pure 008](/srv/git/pure.git) 상태 영속).

워크벤치와 다른 점은 셋이다. 입력 종류를 `key`로 적고, 도형 대신 `view`로 값을 내보내고, `else`가 있다.
도형과 인터페이스 표현은 Vega가 맡는다. 포인터 입력, 값 입력, 레코드·규칙은 v0a에 없다.

## 모듈

모듈 형식은 Pure가 소유한다([Pure 설계 008](/srv/git/pure.git): `docs/design/008-module-format.md`).
aup는 그 형식의 생산자일 뿐이다. 모듈에는 이름표·초기값·입력 키·보기·핸들러·명령이 들어가고,
Pure는 로드할 때 범위·이름·분기·스택 높이를 모두 정적으로 검증한다. 검증을 통과한 모듈은 실행 중에
스택 넘침이나 잘못된 참조를 만들 수 없다.

## 툴체인

- `aup check FILE`: 진단만 한다. 오류는 `파일:줄:열: 메시지` 한 줄이다(라이브러리 안이면 그 파일).
- `use 이름`은 `이름.aup`을 FILE 옆, 그다음 `AUP_PATH`의 디렉터리 순서로 찾는다.
- `aup build FILE -o OUT.pmod`: 모듈을 만든다. 같은 소스는 바이트 단위로 같은 모듈을 만든다.
- C17, 외부 의존성 없음. 컴파일러는 Pure를 링크하지 않는다. 시험에서는 Pure의 검증기로 결과를 확인한다.

## v0b: 바이트와 영속 문서 (구현)

```text
state documents: bytes by key          # 키마다 바이트 값 하나. 런타임이 버전을 붙여 영속화한다

input show = message by key            # 키(와 선택적 기준 버전)를 가진 메시지
input edit = message by key {
  start: u32                           # 필드는 u32와 마지막 bytes 하나
  remove: u32
  insert: bytes
}

on show {
  reply documents[key]
}

on edit {
  documents[key].replace(edit.start .. edit.start + edit.remove, edit.insert)
  require documents[key].bytes <= 8    # 선택: 크기 정책은 require 한 줄
  reply edit.insert
}
```

- `on 메시지 { }`는 메시지가 도착할 때 한 번 실행된다. 필드는 `메시지.필드`, 키는 `key`로만 쓴다.
- `이름[key]`는 그 메시지 키의 문서다. 읽으면 bytes, `.bytes`는 크기(없으면 0)다. 없는 문서를 읽으면 `not_found`.
- `replace(a .. b, 값)`는 문서의 `[a, b)`를 바꾼다. 정수가 아니거나 범위 밖이면 이벤트가 `limit`으로 실패한다.
- `require 조건`이 거짓이면 이벤트가 `limit`으로 실패한다. 크기 정책처럼 저장소의 특수 인자였던 것이 문장 하나가 된다.
- 이벤트가 곧 Pure draft 트랜잭션이다. 기준 버전이 현재와 다르면 바로 `conflict`, 핸들러가 성공하면 커밋, 실패하면 아무것도 바뀌지 않는다.
- `get/put`, `store.read/splice`, `request.revision`은 언어에서 사라졌다. CLI·HTTP 호스트가 입력과 응답으로 바꾼다.

검증: Pure의 `pure-edit-module`로 실행한 `examples/editor.aup`·`bounded-editor.aup`가, 기존 `pure-edit`로 실행한
`editor.pure`·`bounded-editor.pure`와 무작위 show·edit·apply 1,280회(성공·충돌·범위·없음·예산·잘못된 키 포함)에서
stdout·stderr·종료 코드가 바이트 단위로 같고, 서로의 저장소를 같게 읽는다(`make check`).
