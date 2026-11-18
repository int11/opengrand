# 재설계 아키텍처 — "OpenGrand"

`result/` 에 새로 만드는 펌웨어·앱의 설계 문서. 근거는 스톡 펌웨어 · 원본 앱의 인터페이스 분석(REA)과 Kawai 사용 설명서다.
원본 코드/데이터(샘플, 바이너리, UI 번들)는 **복사하지 않는다** — 관찰한 *기능과 인터페이스* 만 재구현한다.

## 0. 설계 원칙

1. **와이어 호환(wire-compatible)**: 피아노↔앱 사이 Kawai SysEx 프로토콜(`F0 40 …`)을 그대로 구현한다.
   → 새 펌웨어는 원래 PianoRemote 앱과, 새 앱은 실제 CA79와도 동작할 수 있다.
2. **단일 진실 원천(SSOT)**: 파라미터 표·음색 표·리듬 표·시스템 제어 표를 `result/protocol/spec/*.json` 한 곳에 두고,
   코드 생성기(`result/protocol/tools/gen.py`)가 펌웨어용 C 표와 앱용 TypeScript 모듈을 만든다. 원본은 앱 JS·펌웨어 디스크립터 표·매뉴얼 3곳에 같은 정보가 흩어져 있었다.
3. **하드웨어 분리 + 호스트 시뮬레이터**: 펌웨어 로직은 순수 C11, 하드웨어는 `hal/*.h` 인터페이스 뒤로. `ports/sim`(Linux)에서 전체 펌웨어가 돌아가고 단위·통합 테스트가 가능하다.
4. **결정론적 코어**: 원본은 NORTi(μITRON) RTOS 위 다중 태스크였다. 새 설계는 "ISR → 락프리 링버퍼 → 단일 이벤트 루프" + 별도 오디오 콜백으로 단순화해 테스트 재현성을 확보한다(RTOS 포트는 이벤트 루프를 한 태스크로 감싸면 됨).
5. **안전한 업데이트**: 원본 `.SYS` 는 헤더·서명 없는 파티션 raw 이미지였다(REA 관찰). 새 설계는 헤더+섹션+SHA-256+서명 슬롯을 가진 컨테이너와 A/B 슬롯 부트로더.

## 1. 원본 시스템 (관찰 요약)

```
           ┌──────────── CA79 본체 ─────────────────────────────────────────────┐
 건반(88×3센서)─┐                                                                  │
 페달 ADC ──────┼─> 메인 SoC (TI OMAP-L13x급: ARM926 + C674x DSP, NORTi RTOS)        │
               │   · MIDI 파서 4포트(UART) · Kawai SysEx 디스패처(216 파라미터)       │
               │   · 음원 DB 0x1A0000 · 드럼 리듬 0x2A0000 · DSP 이미지 ~0x360000   │
               │   UART1 ── BT Audio 모듈 (Bluegiga WT32i, iWRAP)                   │
               │   UARTx ── BLE MIDI 모듈 (MBH7BLZ07, Nordic, 모듈 SysEx F0 00 02 08)│
               │   UARTx ── MIDI IN/OUT, USB to Host(USB-MIDI), 패널 링크              │
               └─ 5" LCD 패널 = Android 보드 (PianoRemote 앱이 홈 런처, priv-app)      │
                    · android.media.midi 로 메인보드와 USB-MIDI(usb2) [추론]          │
                    · WebView(Vue UI) ⇄ kwmcore 브리지(122 명령)                     │
           └──────────────────────────────────────────────────────────────────────┘
 스마트폰 PianoRemote = 같은 APK, AppMode(BLE-MIDI/USB-MIDI)
```

## 2. 새 시스템 구성

```
result/
├── protocol/            SSOT 명세 + 코드 생성기 (+ 생성물은 각 프로젝트에 커밋)
├── firmware/            OpenGrand 메인 펌웨어 (C11)
│   ├── include/ src/    코어 모듈
│   ├── hal/             하드웨어 인터페이스 헤더
│   ├── ports/sim/       Linux 시뮬레이터 (stdin/stdout·TCP MIDI, WAV 출력)
│   ├── ports/target/    실기 포트 골격 (메모리맵·드라이버 TODO 명시)
│   ├── tools/           업데이트 컨테이너 패커/검증기
│   └── tests/           단위·통합 테스트
└── app/                 OpenGrand 컨트롤러 앱 (TypeScript)
    ├── src/protocol/    SysEx 코덱·세션·BLE-MIDI 패킷 코덱 (순수 TS, Node에서 테스트)
    ├── src/transport/   WebMIDI · WebBluetooth(BLE-MIDI) · AndroidBridge · WebSocket(sim)
    ├── src/ui/          화면 (Piano / Sound / Music / Metronome / VT / Settings / Connect)
    ├── android/         얇은 Kotlin 셸 (WebView + android.media.midi 브리지, 패널 모드)
    └── tests/
```

### 2.1 펌웨어 모듈

| 모듈 | 책임 | 원본 대응 (관찰) |
|---|---|---|
| `core/evloop` | ISR 링버퍼 → 이벤트 디스패치, 1 ms 틱, 타이머 | NORTi 태스크들 |
| `midi/stream` | 바이트 스트림 파서(running status, realtime 끼어들기, SysEx 누적) | 0x40148b24 파서 |
| `midi/ports` | 포트 추상화: DIN, USB-Device(Host PC), PANEL, BLE, BT(미사용) | 4 UART 포트 + 2 KB 링 |
| `midi/router` | Local, 채널/Omni, Multi-timbral Off/On1/On2, Dual/Split 채널+1 송신, PC 송신 | 매뉴얼 MIDI 사양 |
| `kproto/*` | Kawai SysEx: 프레임 코덱, 포트별 세션(Sync/Mode/DeviceID), Set/Request/ACK/NACK, AllParameter 덤프, 시스템 제어(Model/Dest/Lang/Version), 원격 명령, 0x24 파일 전송 | 0x4014a2c8 디스패처, 0x400faa2c 디스크립터 표 |
| `params/store` | 파트별 파라미터 저장소(Main1–4, Layer, Lower, 4H-R/L, Global, Recorder, MIDI ch1–16), 변경 구독 | 디스크립터 표 |
| `params/persist` | 시작 설정·사용자 메모리 저장 (저널링 KV, CRC32, 전원차단 안전) | UserMemory 파티션 |
| `keyboard/scan` | 3센서 타이밍 → 속도, Let-off, 반복 감지 | GFIII Triple Sensor |
| `keyboard/touch` | 터치커브 10종 + 사용자 커브(조작점), Minimum Touch, Key Volume, Dynamics | TouchCurve* 파라미터 |
| `keyboard/pedal` | 3페달 ADC 보정, Half-Pedal Adjust, Soft Depth, 4Hands 페달 분리 | PedalRawADValue 등 |
| `sound/voices` | 256 보이스 할당·스틸링·댐퍼/소스테누토 상태 | 폴리 256 |
| `sound/parts` | Single/Dual/Split/4Hands 레이어링·밸런스·옥타브·스플릿 | KeyboardMode |
| `sound/tuning` | 마스터 튜닝(427–453 Hz), 7 음률 × 키, 스트레치, 노트별 튜닝 | VT |
| `sound/vt` | Virtual Technician 23 파라미터 → 엔진 제어값 매핑, 프리셋 | VT |
| `sound/engine` | 음원 백엔드 인터페이스 (실기: DSP / sim: 참조 신스) | C674x DSP |
| `fx/` | Reverb 6, Ambience 10, Effect 24 파라미터 → 백엔드 | 매뉴얼 |
| `rhythm/metronome` | 템포 10–400(8분 계열 20–800), 박자 10종, 리듬 100, 비트카운터 송신 | 0x2A0000 리듬 |
| `recorder/` | 내부 10곡×2파트 이벤트 저장(≈90k 노트), 오버더브, 재생, A-B, SMF 입출력 | RecorderData 파티션 |
| `recorder/recview` | 스톡 레코더 · 곡 재생 상태와 곡 표(RecorderData 거울)의 읽기 전용 보기 — 3-6 R0, 스톡에 쓰지 않음 | `0x4191EF79` 등 · `0x40500000` |
| `system/` | 전원(Auto Power Off), Lock, Factory Reset, 버전/모델 정체성, 업데이트 진입 키콤보 | 매뉴얼 |
| `bt/blemidi` | BLE MIDI 모듈 드라이버(이름 설정·타임스탬프·DFU 진입 SysEx `F0 00 02 08 10 55`) | 앱 분석 §4 |
| `bt/audio` | BT Audio 모듈 제어(On/Off, 볼륨, 페어링 상태) — 명령셋은 포트에서 구현 | UART1 iWRAP |
| `update/` | 컨테이너 파싱·검증·A/B 슬롯 기록, 부트 선택 | Boot/Main 파티션 |

### 2.2 데이터 흐름

```
 key ISR ─┐                  ┌─> engine.note_on/off ─> audio callback
 pedal ADC┼─> evloop ─> keyboard ─> parts ─> voices ─┤
 MIDI ISR ┘     │               ▲                     └─> router.tx (DIN/USB/BLE/PANEL)
                ├─> midi/stream ─> router ─┬─> parts (채널 메시지)
                │                         └─> kproto ─> params/store ─(구독)─> keyboard/sound/fx/rhythm/system
                │                                           └─> 변경 알림 SysEx (다른 포트의 세션에 브로드캐스트)
                └─> rhythm/recorder 틱
```

- **변경 전파 규칙**: 어떤 포트에서 온 Set 이든 저장소에 반영 후 ① 요청 포트에 ACK, ② *다른* 연결 세션(패널·폰)에 같은 값을 0x10 으로 통지 → 패널과 폰 UI가 항상 동기화. (원본: 패널 EmbeddedMode는 전체 동기화를 생략하고 피아노가 상태를 push 하는 구조 — 앱 분석 §1)

### 2.3 앱 구조

- **protocol 계층** (순수 TS, DOM 없음): `sysex.ts`(프레임), `params.gen.ts`(생성), `codecs.ts`(BPM 14bit, Tuning, Beat, Progress, EQ…), `session.ts`(접속 시퀀스, ACK 큐: 원본과 동일하게 16회×1 s 재시도), `blemidi.ts`(BLE-MIDI 패킷 인코드/디코드·MTU 분할), `models.ts`.
- **transport 계층**: 공통 인터페이스 `MidiLink { send(bytes); onMessage; close }`
  - `WebMidiLink` (데스크톱/안드로이드 크롬, USB·OS 페어링 BLE)
  - `WebBluetoothLink` (BLE-MIDI GATT `03B80E5A-EDE8-4B33-A751-6CE34EC4C700` / `7772E5DB-3868-4112-A1A9-F2669D106BF3` 직접 — 자체 패킷 코덱)
  - `AndroidBridgeLink` (`window.ca79ng` — Kotlin 셸이 android.media.midi 로 USB/BLE 연결)
  - `WebSocketLink` (펌웨어 시뮬레이터 개발용)
- **상태 계층**: 파라미터 저장소 미러(피아노가 진실 원천), 낙관적 업데이트 + ACK 실패 시 롤백.
- **UI**: 프레임워크 없는 경량 컴포넌트(의존성 최소화, 패널의 구형 WebView에서도 동작). 480×800 패널 레이아웃과 폰 레이아웃을 같은 코드로.
- **Android 셸**: 원본은 122개 명령 브리지 + 앱 내부 SMF 플레이어/녹음기였다. 새 설계는 브리지를 **MIDI 전송 + 패널 기기 기능(백라이트·USB 경로·APK 업데이트) 10여 개**로 줄이고, 연주 기능은 펌웨어로 일원화한다.

## 3. 원본 대비 의도적 변경 (설계 결정)

| 항목 | 원본 (관찰) | 새 설계 | 이유 |
|---|---|---|---|
| 업데이트 이미지 | 헤더 없는 raw 파티션 이미지 | `CA79NG.UPD` 컨테이너 (매직, 버전, 섹션 표, SHA-256, 서명 슬롯) + A/B 슬롯 | 손상·오기록 방지, 롤백 |
| UI 번들 | 원본 앱 고유 형식 | 평문 번들 + 무결성 해시(서명 매니페스트) | 숨기기 대신 무결성 보장 |
| SMF 재생·녹음 위치 | 폰 앱(Java 10 ms 스레드)·피아노 양쪽 | 펌웨어 하나로 일원화, 앱은 원격 제어 | 타이밍 정확도·중복 제거 |
| 앱 브리지 | 122 명령, `setTimeStampLatency` 등 no-op 존재 | 최소 브리지 + 표준 Web MIDI 형태 | 유지보수성 |
| 패널 링크 | USB-MIDI(usb2 bind/unbind) [추론] | 동일하게 USB-MIDI class-compliant (패널을 일반 컨트롤러로 취급) | 호환 + 단순 |
| 파라미터 정의 | 앱 JSON / 펌웨어 C 표 / 매뉴얼 중복 | SSOT JSON → 코드 생성 | 불일치 제거 |

## 4. 검증 전략

- 펌웨어: 모듈 단위 테스트(파서, 라우터, kproto, 저장소, 터치커브, 튜닝, 메트로놈, 레코더, 업데이트 컨테이너, KV 저장소 전원차단 시뮬레이션).
- 통합: 앱의 `session.ts` 를 Node에서 돌려 시뮬레이터 프로세스와 실제 SysEx 로 접속 → 전체 파라미터 동기화·Set/ACK·변경 통지 검증.
- 앱: 코덱 왕복 테스트, BLE-MIDI 패킷 테스트(타임스탬프 랩어라운드, SysEx 분할), 세션 재시도 테스트(가짜 링크).
