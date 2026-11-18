# 추적표 — 기능 → 근거 → 구현 → 검증

> 아래 표는 *구현된* 기능의 근거·구현·테스트 연결만 보여 준다. 이것만 보고 원본의 모든 기능이 구현됐다고 읽으면 안 된다.

근거 표기: **A** = 원본 앱의 인터페이스 분석(REA), **F** = 스톡 펌웨어의 인터페이스 분석(REA), **M** = Kawai 사용 설명서. 분석 노트 자체는 이 저장소에 없다.
경로는 `result/` 기준. 테스트: `firmware/tests/*`(C), `app/tests/*`(TS).

| 기능 | 근거 | 구현 | 검증 |
|---|---|---|---|
| SysEx 프레임·모델 ID (08 02 / 04 1E) | A §3, F 0x4014a2c8 | `firmware/src/kproto/kproto.c`, `app/src/protocol/sysex.ts` | test_connect_identity_version, blemidi.test "Kawai frame", e2e |
| 접속 시퀀스·모델/버전 질의 | A §3, F sysex_commands | kproto `handle_system`, app `session.ts connect()` | session.test, **e2e.sim** |
| 파라미터 209종 (디스크립터·범위) | F descriptors(216), A protocol_spec | `protocol/spec/ca79.json` → 생성 `params_gen.*`, `params.gen.ts` | test_param_out_of_range_nack, codecs.test |
| ACK/NACK·재전송(16×1 s)·0x31/0x32 규칙 | A handshake | kproto `handle_param`, session `enqueue/transmit` | test_param_set_ack…, session.test(재전송/포기/비대기) |
| 전체 덤프·EOD | A all_parameter, F fn02 | `kp_dump_all` | test_dump_all, e2e(150+ 값) |
| 다중 컨트롤러 동기화 | (설계 확장) | `kp_notify` | test_param_set_ack_and_notify…, e2e "two controllers" |
| 0x24 파일 전송 (8→7, XOR) | A file_dump, F 0x4014f96c | kproto `handle_file`, `kp_pack87` | test_pack87_roundtrip_and_file_transfer |
| range 코덱 JS 동등성 | A protocol_spec | `param_range_encode/decode`, `codecs.ts` | test_range_codec_matches_js, codecs.test |
| 기본값(SK-EX Rendering 등) | A initial_data, default_model | 생성 `PARAM_DEFAULTS`, `param_store_defaults` | test_store_defaults_from_initial_data |
| 3센서 속도·반복 | M GFIII | `keyboard/keyboard.c` | test_scanner_velocity_and_repetition |
| 터치커브 10+User, Minimum Touch, Key Volume | M VT(1,16,20), A TouchCurve enum | `touch_apply` | test_touch_curves |
| 페달·하프페달·소프트 | M VT(21,22), Pedals | `pedal_value`, `piano_pedal` | test_pedals, test_voice_damper_sostenuto |
| 4Hands 페달 분리·Split Lower Pedal | M 4Hands | `piano_pedal` | test_dual_split_fourhands |
| 튜닝 427–453 Hz·음률 7종·스트레치 | M VT(17–19), A Tuning codec | `sound/tuning.c` | test_tuning, e2e(442 Hz) |
| 256 폴리·스틸링·댐퍼/소스테누토 | M 사양 | `sound/voices.c` | test_voice_stealing, test_voice_damper_sostenuto |
| Single/Dual/Split/4Hands | M, A KeyboardMode | `kb_layout_route` | test_layout_modes, test_dual_split_fourhands |
| MIDI 채널·Omni·Local·멀티팀버럴·PC 송신 | M MIDI 매뉴얼 | `midi/router.c` | test_remote_changes_sound_and_local_off, test_midi_in_plays_and_multitimbral |
| MIDI 파서 (running status·realtime·SysEx 512) | F 0x40148b24 | `midi/stream.c` | test_midi_* 3종 |
| 메트로놈 (10–400 BPM, 박자 10종, 리듬 100) | M, A BPM/Beat, F 0x2A0000 | `rhythm/metronome.c`, 리듬 표 생성 | test_metronome, test_metronome_beat_counter_to_remote, e2e |
| 레코더 10곡×2파트·오버더브·A-B | M Recorder, A Music | `recorder/recorder.c` | test_recorder_record_play_and_smf, test_overdub_second_part |
| SMF 저장/읽기 | M USB Recorder, A recordExport | `smf_write/smf_read` | test_recorder_record_play_and_smf |
| 시작 설정 저장·공장 초기화 | A StartUpSetting*, M User Data | `system/system.c`, `piano_factory_reset` | test_startup_settings_and_factory_reset |
| Auto Power Off | M System, A AutoPowerOff | `power_timer_*` | test_auto_power_off |
| 설정 저장소 전원차단 내성 | F UserMemory 파티션 | `params/kvstore.c` | test_kv_power_loss(40 절단점), test_kv_compaction |
| BT 오디오 중 안 꺼짐 · 원격 끄기(기능 1 · 2) | F, 실기 | `firmware/src/system/power.c` · `powerctl.c` · `ngext.c` · `ports/cave/power_cave.c`, app `ui/power.ts` | test_ng_remote_off_op, pwc_remote_off_*, pwc_bt_hold_*, `roundtrip_power.py ota_main` 13, power.sim.test |
| 업데이트 컨테이너·A/B 롤백 | F raw .SYS(무결성 없음) → 설계 변경 | `update/update.c`, `tools/mkupd.py` | test_update_container, test_boot_ab, `make check`(Python↔C 교차) |
| BLE 모듈 SysEx·DFU 진입 | A bluetoothmidi.js, DFU_execute | `bt/blemidi.c`, app `BleModule` | test_blemidi_module_commands, blemidi.test |
| BLE-MIDI 패킷 (MTU·SysEx 연속·running status) | A BleMidi/BleScanner, BLE-MIDI 규격 | `app/src/protocol/blemidi.ts` | blemidi.test 4종 |
| 전송 계층 (WebMIDI/BLE/Android/WS) | A KWMMidiManager | `app/src/transport/*`, Android `MidiBridge.kt` | e2e(WS 경로는 수동 확인), APK 빌드 |
| 패널 모드 (HOME 런처·백라이트·USB 경로) | A manifest, libMRJNI | Android `MainActivity.kt`, panel 플레이버 | REA로 APK 매니페스트 검증 |
| UI 전 화면·컨트롤 바인딩 | A UI 기능 목록, M 메뉴 구조 | `app/src/ui/*` | ui.dom.test 4종 |

## 재현하지 않은 것 (의도적·불가) — 원인별 요약

아래는 *막힌 원인*의 설명이다.
| 항목 | 이유 |
|---|---|
| SK-EX/HI-XL 실제 음원(샘플·공명 모델) | 원본 웨이브 ROM·DSP 이미지는 저작물 → 미사용. `sound_engine_t` 인터페이스와 파라미터 라우팅만 제공, 시뮬레이터는 참조 신스 |
| C674x DSP 프로그램 | REA/Ghidra가 C6000 미지원(펌웨어 분석 미확인 항목) |
| BootProgram (.SYS 기록 측) | 업데이트 파일에 포함되지 않음 → 새 A/B 부트 선택 로직으로 대체 |
| 레슨/피아노 뮤직 곡 데이터(1330곡) | 저작물. 재생 경로(SMF 플레이어/레코더)만 제공 |
| BT 오디오(WT32i iWRAP) 명령 드라이버, USB 호스트(메모리) 스택 | 하드웨어 의존 → 타깃 포트 TODO |
| 원본 UI 번들 형식 | 쓰지 않음 → 평문 번들 + 업데이트 컨테이너 무결성으로 대체 |
