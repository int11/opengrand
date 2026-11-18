# result/ — 개발자용 구성 안내

설치 · 빌드 · 기여 방법은 저장소 최상위 문서가 단일 출처다: [README.md](../README.md), [BUILDING.md](../docs/BUILDING.md),
[FLASHING.md](../docs/FLASHING.md), [SAFETY.md](../docs/SAFETY.md), [CONTRIBUTING.md](../docs/CONTRIBUTING.md). 이 파일은 `result/` 아래
코드가 어디에 있는지만 적는다.

| 문서 | 내용 |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | 아키텍처 · 설계 결정 · 원본 대비 변경점 |
| [protocol/REMOTE_PROTOCOL.md](protocol/REMOTE_PROTOCOL.md) | 원격 프로토콜 명세 (원본과 바이트 호환 + OpenGrand 확장) |
| [protocol/PARAMS.md](protocol/PARAMS.md) · [protocol/NGVER.md](protocol/NGVER.md) | 파라미터 표 · 확장 버전 표 (생성물) |
| [TRACEABILITY.md](TRACEABILITY.md) | 기능 → 근거 → 구현 → 테스트 추적표 |
| [stockfw/ARTIFACTS.md](stockfw/ARTIFACTS.md) | `make image`가 만드는 이미지와 그 해시 |

## 구성

```
result/
├── protocol/   단일 진실 원천 spec/ca79.json (손으로 고침) → tools/gen.py → 펌웨어 C 표 + 앱 TS 표 + PARAMS.md · NGVER.md
├── firmware/   C11 펌웨어 코어 + Linux 시뮬레이터 + 호스트 테스트
│   ├── src/        midi kproto keyboard sound rhythm recorder system bt params update util
│   ├── hal/        하드웨어 인터페이스
│   ├── ports/sim/  참조 신스 + stdin/stdout 시뮬레이터
│   ├── ports/cave/ 스톡 펌웨어 안에서 도는 포트 (stockfw/cave가 함께 링크)
│   ├── tests/      호스트 테스트 (make -C firmware check)
│   └── tools/      mkupd.py · updcheck(업데이트 컨테이너), 라운드 트립용 호스트 라이브러리
├── stockfw/    스톡 이미지 확장: cave/(ARM 소스 · 링커 스크립트 · 고정 blob) · tools/(이미지 빌드 · 패처 · OTA 패커 · 에뮬레이터 라운드 트립)
├── app/        TypeScript 원격 앱 (폰 · 브라우저 · 내장 패널)
│   ├── src/protocol  SysEx · 세션 · 코덱 · BLE-MIDI · OpenGrand 확장
│   ├── src/transport WebMIDI · Web Bluetooth · Android 브리지 · 데스크톱 · WebSocket(시뮬레이터)
│   ├── src/ui        화면
│   ├── panelui/      내장 패널용 UI 번들 로더
│   └── android/      Kotlin 셸 (panel / phone 플레이버)
└── desktop/    Windows 데스크톱 셸 (Electron — app/dist를 서버 없이 띄움)
```

## 자주 쓰는 명령 (`result/` 기준)

```bash
make -C firmware check                         # 펌웨어 호스트 테스트 + 업데이트 컨테이너 교차검증 + 시뮬레이터 스모크

# 시뮬레이터에 직접 명령 (MIDI 송신은 "tx <port> <hex>"로 출력)
printf 'midi panel F0 40 7F 7E 08 02 01 01 F7\nkey 60 90 500\nwait 800\nstatus\nquit\n' \
  | firmware/build/ca79ng_sim --wav /tmp/out.wav

cd app && npm ci && npm test && npm run dev    # http://localhost:8079/?sim = 브라우저 UI ↔ 실시간 펌웨어 시뮬레이터
cd desktop && npm ci && npm run desktop        # Electron 개발 실행; npm run dist:win = 휴대용 exe
```

프로토콜 표를 바꾸려면 `protocol/spec/ca79.json`을 고치고 저장소 최상위에서 `make gen` (생성물은 손으로 고치지 않는다; `make gen-check`).
