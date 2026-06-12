"""
VoiceBridge - 폰 테스트용 HTTPS 서버
=====================================
phone_sender.html을 HTTPS로 서빙합니다.
Web Bluetooth API는 HTTPS(보안 컨텍스트)에서만 동작합니다.

사전 준비:
    pip install cryptography

실행:
    python start_phone_server.py

접속:
    출력된 주소를 폰 Chrome 주소창에 입력
    (PC와 폰이 같은 WiFi에 연결되어 있어야 합니다)
"""

import http.server
import ssl
import socket
import os
from pathlib import Path

PORT      = 8443
BASE_DIR  = Path(__file__).parent
CERT_FILE = BASE_DIR / "cert.pem"
KEY_FILE  = BASE_DIR / "key.pem"


def get_local_ip():
    """PC의 로컬 IP 주소를 반환"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def generate_cert(local_ip: str) -> bool:
    """자체 서명 인증서 생성 (cryptography 패키지 사용)"""
    try:
        from cryptography import x509
        from cryptography.x509.oid import NameOID
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        import datetime
        import ipaddress

        print("인증서 생성 중...")

        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)

        name = x509.Name([
            x509.NameAttribute(NameOID.COMMON_NAME, "VoiceBridge"),
        ])
        cert = (
            x509.CertificateBuilder()
            .subject_name(name)
            .issuer_name(name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(datetime.datetime.utcnow())
            .not_valid_after(
                datetime.datetime.utcnow() + datetime.timedelta(days=365)
            )
            .add_extension(
                x509.SubjectAlternativeName([
                    x509.IPAddress(ipaddress.IPv4Address(local_ip)),
                    x509.DNSName("localhost"),
                ]),
                critical=False,
            )
            .sign(key, hashes.SHA256())
        )

        CERT_FILE.write_bytes(
            cert.public_bytes(serialization.Encoding.PEM)
        )
        KEY_FILE.write_bytes(
            key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.TraditionalOpenSSL,
                serialization.NoEncryption(),
            )
        )
        print(f"✓ 인증서 생성 완료")
        return True

    except ImportError:
        print()
        print("❌ cryptography 패키지가 없습니다.")
        print("   아래 명령으로 설치 후 다시 실행하세요:")
        print()
        print("   pip install cryptography")
        print()
        return False

    except Exception as e:
        print(f"❌ 인증서 생성 실패: {e}")
        return False


if __name__ == "__main__":
    local_ip = get_local_ip()

    # 인증서 생성 (최초 1회)
    if not CERT_FILE.exists() or not KEY_FILE.exists():
        if not generate_cert(local_ip):
            exit(1)

    # 서버 디렉토리 = 이 파일이 있는 폴더
    os.chdir(BASE_DIR)

    # HTTPS 서버 시작
    handler = http.server.SimpleHTTPRequestHandler
    handler.log_message = lambda *a: None  # 접속 로그 출력 끄기

    httpd = http.server.HTTPServer(("0.0.0.0", PORT), handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=str(CERT_FILE), keyfile=str(KEY_FILE))
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    url = f"https://{local_ip}:{PORT}/phone_sender.html"

    print()
    print("=" * 54)
    print("  VoiceBridge 폰 테스트 서버 실행 중")
    print("=" * 54)
    print()
    print("  [1] PC와 폰을 같은 WiFi에 연결하세요")
    print()
    print("  [2] 폰 Chrome 주소창에 아래 주소 입력:")
    print()
    print(f"      {url}")
    print()
    print("  [3] '연결이 안전하지 않습니다' 경고 뜨면:")
    print("      '고급' 버튼 → '안전하지 않음으로 이동' 클릭")
    print()
    print("  [4] 화면에서 WAV 파일 선택 후 전송 버튼 탭")
    print()
    print("  ⚠️  안드로이드 Chrome 전용 (iOS 미지원)")
    print()
    print("  종료: Ctrl+C")
    print("=" * 54)
    print()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n서버 종료")
