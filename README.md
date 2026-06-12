# VoiceBridge — nRF52840 수신 보드 펌웨어

영어 음성을 실시간으로 한국어로 번역·합성하여 BLE로 전달받고, I2S DAC을 통해 스피커로 출력하는 nRF52840 DK 펌웨어입니다.

## 데모 영상

[![VoiceBridge Demo](https://img.youtube.com/vi/d-RMXx4DXOw/0.jpg)](https://youtu.be/d-RMXx4DXOw)

---

## 시스템 아키텍처

```
┌─────────────────────────────────────────┐        ┌──────────────────────────┐
│         Python AI Server (Laptop)       │        │  nRF52840 Embedded Board │
│                                         │        │                          │
│  ① Audio Recording                      │        │  BLE Receive             │
│     audio_recorder.py · PyAudio         │        │  Nordic UART Service     │
│     16kHz 16-bit Mono WAV               │        │  (NUS) · GATT Write      │
│              ↓ input_recording.wav      │        │          ↓               │
│  ② Speech-to-Text (STT)                 │        │  I2S DAC Output          │
│     ai_pipeline.py · faster-whisper     │        │  8kHz PCM → Analog       │
│     English speech → English text       │        │          ↓               │
│              ↓ English text             │        │  Speaker Output          │
│  ③ Translation                          │  BLE   │  Korean audio playback   │
│     ai_pipeline.py · argostranslate     │ ─────► │                          │
│     English → Korean (offline)          │        └──────────────────────────┘
│              ↓ Korean text              │
│  ④ Text-to-Speech (TTS)                 │   [main_gui.py · tkinter Control Panel]
│     ai_pipeline.py · gTTS               │
│     Korean text → MP3 → WAV            │
│              ↓ output_korean_8k.wav     │
│  ⑤ BLE Transmit                         │
│     ble_sender.py · bleak               │
│     20-byte packet slicing              │
└─────────────────────────────────────────┘
```

이 레포지토리는 **nRF52840 수신 보드 펌웨어**와 **PC/폰 테스트 송신 스크립트**를 포함합니다.  
Python AI Server(`VoiceBridge`) 코드는 별도 레포에서 관리됩니다.

---

## 하드웨어 연결

### MAX98357A ↔ nRF52840 DK

| MAX98357A 핀 | nRF52840 DK 핀 | 설명 |
|:-----------:|:--------------:|:-----|
| BCLK        | P1.13 (D11)    | I2S Bit Clock |
| LRC         | P1.14 (D12)    | I2S Word Select (LRCLK) |
| DIN         | P1.15 (D13)    | I2S Data In |
| VIN         | VDD (3.3V)     | 전원 |
| SD          | VDD (3.3V)     | 항상 활성 (VIN과 분기) |
| GND         | GND            | 공통 접지 |

---

## BLE 통신 프로토콜

- **BLE 서비스**: Nordic UART Service (NUS)
- **광고 이름**: `VoiceBridge`
- **NUS RX UUID**: `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- **패킷 크기**: MTU 협상 후 자동 결정 (최대 244B, 기본 20B)

| 패킷 종류 | 구조 | 설명 |
|:--------:|:-----|:-----|
| START    | `[0xAA][0xBB][총크기 4B LE][패딩]` | 전송 시작, 파일 크기 전달 |
| DATA     | `[seq_lo][seq_hi][오디오 데이터]`   | 순차 WAV 데이터 |
| END      | `[0xFF][0xFE][패딩]`               | 전송 완료, 재생 트리거 |

### WAV 포맷 (VoiceBridge 출력 규격)

- **Sample Rate**: 8kHz
- **Bit Depth**: 16-bit signed PCM
- **Channels**: Mono (펌웨어 내부에서 Stereo로 복제 후 I2S 출력)
- **최대 파일 크기**: 100KB

---

## LED 동작

| 상태 | LED 패턴 |
|:-----|:--------|
| 부팅 완료 | LED 1→2→3→4 순차 점등 후 전체 소등 |
| BLE 연결됨 | LED 1만 ON |
| 수신 중 | LED 1→2→3→4 체이스 애니메이션 (5패킷마다 이동) |
| 수신 완료 | LED 전체 ON |
| 재생 완료 | LED 전체 OFF → 대기 상태 복귀 |

---

## 파일 구조

```
voicebridge_nrf/
├── src/
│   └── main.c                      # BLE NUS 수신 + I2S 재생 + LED 제어
├── CMakeLists.txt                  # Zephyr 빌드 설정
├── prj.conf                        # Kconfig (BT_NUS, I2S, MTU 등)
├── nrf52840dk_nrf52840.overlay     # I2S 핀 배정 (P1.13/14/15)
├── test_ble_sender.py              # PC BLE 테스트 송신 스크립트 (bleak)
├── phone_sender.html               # 폰 Chrome Web Bluetooth 송신 앱
├── start_phone_server.py           # HTTPS 서버 (phone_sender.html 서빙)
└── .gitignore
```

---

## 빌드 및 플래시

### 요구사항

- [nRF Connect SDK](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/getting_started.html) (v2.x 이상)
- VS Code + nRF Connect for VS Code 확장

### 빌드

1. VS Code nRF Connect 패널 → **Applications** → 프로젝트 추가
2. Board: `nrf52840dk_nrf52840` 선택
3. **Build** 클릭

### 플래시

1. nRF52840 DK를 USB로 연결
2. nRF Connect 패널에서 **Flash** 클릭
3. LED 1→2→3→4 순차 점등 후 소등 = 정상 부팅 확인

---

## 테스트 방법

### PC BLE 테스트

```bash
conda activate voicebridge
pip install bleak tqdm

python test_ble_sender.py <wav파일>
# 예: python test_ble_sender.py C:\VoiceBridge\output\output_korean_8k.wav
```

패킷 구조만 확인 (BLE 연결 없이):
```bash
python test_ble_sender.py <wav파일> --dry
```

### 폰(Android Chrome) BLE 테스트

```bash
conda activate voicebridge
pip install cryptography
python start_phone_server.py
# 출력된 https://[내IP]:8443/phone_sender.html 을 폰 Chrome에서 접속
```

> Web Bluetooth API는 Android Chrome에서만 지원됩니다. iOS Safari는 미지원.

---

## 브랜치

| 브랜치 | 설명 |
|:------|:-----|
| `main` | GATT Write With Response (ACK 방식, 안정적) |
| `feature/write-without-response` | GATT Write Without Response (2ms 딜레이, 전송 속도 개선 테스트) |
