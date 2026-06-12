"""
VoiceBridge - BLE 테스트 송신기 (독립 실행)
==============================================
용도 : PC 블루투스로 nRF52840 보드에 WAV 파일을 전송하여
       LED 동작 및 I2S 출력을 확인합니다.

설치 :
    pip install bleak tqdm

실행 :
    python test_ble_sender.py <wav파일>
    python test_ble_sender.py output_korean_8k.wav

전송 프로토콜 (nRF 펌웨어와 약속된 규격):
    START  [0xAA][0xBB][size 4B LE][...0]  20바이트
    DATA   [seq_lo][seq_hi][data 18B]       20바이트 × N
    END    [0xFF][0xFE][...0]               20바이트

예상 LED 동작:
    전송 시작 → LED 1→2→3→4 체이스 깜빡임
    전송 완료 → LED 전체 ON → 재생 후 전체 OFF
"""

import asyncio
import struct
import sys
from pathlib import Path

try:
    from bleak import BleakClient, BleakScanner
    from tqdm import tqdm
except ImportError:
    print("필요 라이브러리가 없습니다. 아래 명령으로 설치하세요:")
    print("  pip install bleak tqdm")
    sys.exit(1)

# ── BLE 설정 ──────────────────────────────────────────────────────────
DEVICE_NAME  = "VoiceBridge"                       # 보드 광고 이름
NUS_RX_UUID  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # NUS RX (보드 수신)

TARGET_MTU   = 247   # nRF52840 지원 최대 MTU (prj.conf 설정과 일치)
# Write With Response를 사용하므로 별도 지연 불필요 (ACK가 flow control 역할)


# ── 패킷 생성 ─────────────────────────────────────────────────────────

def make_packets(wav_bytes: bytes, mtu: int) -> list[bytes]:
    """WAV 바이트를 START / DATA / END 패킷 리스트로 변환.
    MTU에 따라 DATA 페이로드 크기를 자동 조정 (최대 MTU-5)."""
    pkt_size    = mtu - 3          # ATT Write Without Response 오버헤드 3B
    data_per_pkt = pkt_size - 2    # 2B 시퀀스 헤더 제외
    total = len(wav_bytes)
    packets = []

    # START 패킷: [0xAA][0xBB][총크기 4B LE] + 제로패딩
    start = struct.pack('<BBI', 0xAA, 0xBB, total)
    packets.append(start.ljust(pkt_size, b'\x00'))

    # DATA 패킷: [seq_lo][seq_hi][data N바이트]
    seq = 0
    for off in range(0, total, data_per_pkt):
        chunk = wav_bytes[off:off + data_per_pkt]
        pkt = struct.pack('<H', seq % 65536) + chunk
        packets.append(pkt.ljust(pkt_size, b'\x00'))
        seq += 1

    # END 패킷: [0xFF][0xFE] + 제로패딩
    end = struct.pack('<BB', 0xFF, 0xFE)
    packets.append(end.ljust(pkt_size, b'\x00'))

    return packets, seq, data_per_pkt


# ── BLE 스캔 및 전송 ──────────────────────────────────────────────────

async def scan_and_send(wav_path: str):
    print(f"[스캔] '{DEVICE_NAME}' 장치를 10초간 탐색합니다...")
    devices = await BleakScanner.discover(timeout=10.0)

    addr = None
    print("[스캔] 발견된 BLE 장치:")
    for d in devices:
        name = d.name or "(이름없음)"
        print(f"  - {name:30s} {d.address}")
        if DEVICE_NAME.lower() in name.lower():
            addr = d.address
            print(f"       ★ 타겟 발견!")

    if not addr:
        print(f"\n❌ '{DEVICE_NAME}' 장치를 찾지 못했습니다.")
        print("   체크리스트:")
        print("   1. nRF52840 보드 전원 ON 확인")
        print("   2. 펌웨어 플래시 및 정상 부팅 확인 (RTT 로그)")
        print("   3. 광고 이름이 'VoiceBridge'인지 확인")
        return

    raw = Path(wav_path).read_bytes()
    total = len(raw)

    print(f"[연결] {addr} 에 연결 중...")
    async with BleakClient(addr) as client:
        if not client.is_connected:
            print("❌ 연결 실패!")
            return

        print("✓ 연결 성공!")

        # MTU 협상: ATT 페이로드를 최대 244B까지 늘림
        try:
            negotiated = await client.exchange_mtu(TARGET_MTU)
            print(f"[MTU] 협상 완료: {negotiated} bytes")
        except Exception:
            negotiated = 23   # 협상 실패 시 기본값
            print(f"[MTU] 협상 실패, 기본값 {negotiated}B 사용")

        packets, n_data, dpkt = make_packets(raw, negotiated)
        duration_s = total / (8000 * 2)

        print(f"\n파일     : {Path(wav_path).name}")
        print(f"크기     : {total:,} bytes ({total / 1024:.1f} KB)")
        print(f"추정 재생 : ~{duration_s:.1f}초 (8kHz mono 기준)")
        print(f"패킷 크기 : {negotiated-3}B / 패킷 (데이터 {dpkt}B)")
        print(f"패킷 수   : START 1 + DATA {n_data} + END 1 = 총 {len(packets)}개")
        print(f"전송 방식 : GATT Write Request (ACK, 패킷 유실 없음)\n")
        print(f"[전송] {len(packets)}개 패킷 전송 시작...")

        with tqdm(total=len(packets), unit="pkt", ncols=60,
                  bar_format="{l_bar}{bar}| {n_fmt}/{total_fmt} [{elapsed}]") as bar:
            for pkt in packets:
                # response=True → GATT Write Request: 서버 ACK 후 다음 전송
                # Write Without Response는 패킷 유실 시 재생 속도가 빨라지는 원인
                await client.write_gatt_char(NUS_RX_UUID, pkt, response=True)
                bar.update(1)

    print("\n✅ 전송 완료!")
    print("   → 보드 LED가 전체 ON 되면 수신 성공")
    print("   → 스피커 연결 시 한국어 음성이 재생됩니다")


# ── 오프라인 패킷 구조 검증 (BLE 없이 실행 가능) ─────────────────────

def verify_packets(wav_path: str):
    """BLE 장치 없이 패킷 구조만 확인하는 디버깅 함수"""
    raw = Path(wav_path).read_bytes()
    packets, n_data, dpkt = make_packets(raw, TARGET_MTU)

    print("─── 패킷 구조 검증 ───────────────────────")
    for i, pkt in enumerate([packets[0], packets[1], packets[-1]]):
        label = "START" if i == 0 else ("DATA#1" if i == 1 else "END")
        hex_s = " ".join(f"{b:02X}" for b in pkt[:8])
        print(f"  {label:7s}: {hex_s} ...")

    print(f"\n✅ 총 {len(packets)}개 패킷 정상 생성 (데이터 {dpkt}B/패킷)")


# ── 진입점 ────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("=" * 50)
        print(" VoiceBridge BLE 테스트 송신기")
        print("=" * 50)
        print("\n사용법:")
        print("  python test_ble_sender.py <wav파일>       BLE 전송")
        print("  python test_ble_sender.py <wav파일> --dry 패킷 구조만 확인\n")
        print("예시:")
        print("  python test_ble_sender.py output_korean_8k.wav")
        print("  python test_ble_sender.py C:\\VoiceBridge\\output\\output_korean_8k.wav")
        sys.exit(0)

    wav = sys.argv[1]
    dry = "--dry" in sys.argv

    if not Path(wav).exists():
        print(f"❌ 파일을 찾을 수 없습니다: {wav}")
        sys.exit(1)

    if dry:
        verify_packets(wav)
    else:
        asyncio.run(scan_and_send(wav))
