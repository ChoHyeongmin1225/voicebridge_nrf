/*
 * VoiceBridge - nRF52840 펌웨어
 * BLE NUS 수신 → WAV 버퍼링 → I2S MAX98357A 출력
 *
 * ┌─ 핀 배정 (MAX98357A) ──────────────────────────┐
 * │  BCLK  ← P1.13 (Arduino D11)                  │
 * │  LRCLK ← P1.14 (Arduino D12)                  │
 * │  DIN   ← P1.15 (Arduino D13)                  │
 * │  VIN   ← 3.3V  │  GND ← GND  │  SD ← 3.3V   │
 * └────────────────────────────────────────────────┘
 *
 * ┌─ BLE 프로토콜 (ble_sender.py와 동일) ──────────┐
 * │  START : [0xAA][0xBB][size 4B LE][...0]  20B  │
 * │  DATA  : [seq_lo][seq_hi][data 18B]       20B  │
 * │  END   : [0xFF][0xFE][...0]               20B  │
 * └────────────────────────────────────────────────┘
 *
 * ┌─ LED 동작 ─────────────────────────────────────┐
 * │  부팅    : LED 1→2→3→4 순차 점등 후 소등       │
 * │  BLE 연결: LED 1 ON                            │
 * │  수신 중 : LED 1→2→3→4 체이스 (5패킷마다)      │
 * │  수신 완료: 전체 ON → 재생 시작                 │
 * │  재생 완료: 전체 OFF → 대기 상태               │
 * └────────────────────────────────────────────────┘
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/services/nus.h>
#include <string.h>

/* ═══════════════════════════════════════════════════
 *  LED (nRF52840 DK: active low)
 * ═══════════════════════════════════════════════════ */
static const struct gpio_dt_spec leds[4] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios),
};

static inline void led_set(int idx, int val) { gpio_pin_set_dt(&leds[idx], val); }
static void leds_all(int val) { for (int i = 0; i < 4; i++) led_set(i, val); }

/* ═══════════════════════════════════════════════════
 *  BLE 프로토콜 상수
 * ═══════════════════════════════════════════════════ */
#define PKT_SIZE      20
#define START_M0      0xAA
#define START_M1      0xBB
#define END_M0        0xFF
#define END_M1        0xFE
#define DATA_HDR      2      /* [seq_lo][seq_hi] */
#define DATA_PAYLOAD  18     /* 패킷당 실제 오디오 데이터 */

/* ═══════════════════════════════════════════════════
 *  WAV 수신 버퍼 (최대 100 KB)
 *  nRF52840 RAM: 256 KB / BLE 스택: ~40 KB → 여유 있음
 * ═══════════════════════════════════════════════════ */
#define WAV_BUF_MAX  (100U * 1024U)
static uint8_t  g_wav[WAV_BUF_MAX];
static uint32_t g_wav_expected = 0;   /* START에서 수신한 총 파일 크기 */
static uint32_t g_wav_written  = 0;   /* 현재까지 누적된 바이트 */

/* ═══════════════════════════════════════════════════
 *  상태 머신
 * ═══════════════════════════════════════════════════ */
typedef enum { ST_IDLE, ST_RECV, ST_PLAY } state_t;
static volatile state_t g_state = ST_IDLE;

K_SEM_DEFINE(g_play_sem, 0, 1);   /* END 수신 시 give → playback_thread 깨움 */

/* ═══════════════════════════════════════════════════
 *  LED chase 효과 (수신 진행 표시)
 * ═══════════════════════════════════════════════════ */
static uint32_t g_rx_pkts   = 0;
static uint8_t  g_chase_pos = 0;
#define CHASE_PERIOD  5   /* N 패킷마다 LED 이동 (5 × 10ms = 50ms 간격) */

static void chase_step(void)
{
	g_rx_pkts++;
	if ((g_rx_pkts % CHASE_PERIOD) == 0) {
		leds_all(0);
		led_set(g_chase_pos & 3, 1);
		g_chase_pos++;
	}
}

/* ═══════════════════════════════════════════════════
 *  NUS 수신 콜백 (BT RX 스레드에서 호출됨)
 * ═══════════════════════════════════════════════════ */
static void nus_rx_cb(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
	if (len < 2) {
		return;
	}

	/* ── START 패킷 ─────────────────────────────── */
	if (data[0] == START_M0 && data[1] == START_M1) {
		if (len >= 6) {
			g_wav_expected = (uint32_t)data[2]
				| ((uint32_t)data[3] << 8)
				| ((uint32_t)data[4] << 16)
				| ((uint32_t)data[5] << 24);
		}
		g_wav_written = 0;
		g_rx_pkts     = 0;
		g_chase_pos   = 0;
		leds_all(0);
		g_state = ST_RECV;
		printk("[RX] START: 총 %u 바이트 예정\n", g_wav_expected);
		return;
	}

	/* ── END 패킷 ───────────────────────────────── */
	/* data[1]==END_M1(0xFE) 도 확인하되, 0x00 패딩도 허용 (송신측 호환성) */
	if (data[0] == END_M0 && (data[1] == END_M1 || data[1] == 0x00)) {
		g_state = ST_PLAY;
		leds_all(1);   /* 수신 완료: 전체 ON */
		printk("[RX] END: %u / %u 바이트 수신 완료 → 재생!\n",
		       g_wav_written, g_wav_expected);
		k_sem_give(&g_play_sem);
		return;
	}

	/* ── DATA 패킷 ──────────────────────────────── */
	if (g_state != ST_RECV || len <= DATA_HDR) {
		return;
	}

	const uint8_t *payload = data + DATA_HDR;
	uint16_t       pay_len = len  - DATA_HDR;

	/* 예상 크기 초과 방지 */
	uint32_t remain = g_wav_expected - g_wav_written;

	if (pay_len > remain) {
		pay_len = (uint16_t)remain;
	}

	if (g_wav_written + pay_len <= WAV_BUF_MAX) {
		memcpy(g_wav + g_wav_written, payload, pay_len);
		g_wav_written += pay_len;
	}

	chase_step();
}

/* ═══════════════════════════════════════════════════
 *  BLE 연결/해제 콜백
 * ═══════════════════════════════════════════════════ */
static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("[BLE] 연결 실패 err=%d\n", err);
		return;
	}
	printk("[BLE] 연결됨!\n");
	leds_all(0);
	led_set(0, 1);   /* LED1 ON: 연결 표시 */

	/* 짧은 connection interval 요청 → Android TX 큐 포화 방지
	 * interval 6~12 = 7.5~15ms / supervision timeout 4s */
	static const struct bt_le_conn_param fast = {
		.interval_min = 6,
		.interval_max = 12,
		.latency      = 0,
		.timeout      = 400,
	};
	bt_conn_le_param_update(conn, &fast);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("[BLE] 연결 해제 reason=%d\n", reason);
	leds_all(0);
	g_state = ST_IDLE;
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected    = on_connected,
	.disconnected = on_disconnected,
};

static struct bt_nus_cb nus_cb = { .received = nus_rx_cb };

/* ═══════════════════════════════════════════════════
 *  BLE 광고 데이터
 * ═══════════════════════════════════════════════════ */
static const struct bt_data adv[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE,
		CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* ═══════════════════════════════════════════════════
 *  I2S (MAX98357A)
 * ═══════════════════════════════════════════════════ */
#define I2S_BLK_BYTES   512    /* DMA 블록 크기 */
#define I2S_SLAB_BLOCKS 4      /* 더블 버퍼링 여유 */
#define AUDIO_GAIN      4      /* 8비트 오디오 소프트웨어 게인 (저진폭 TTS 보정) */
K_MEM_SLAB_DEFINE_STATIC(i2s_slab, I2S_BLK_BYTES, I2S_SLAB_BLOCKS, 4);
static const struct device *i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));

/* ═══════════════════════════════════════════════════
 *  WAV 청크 탐색 파서
 *  - "fmt " / "data" 청크를 직접 검색
 *  - 44바이트 고정 가정 없음 (fact 청크, EXTENSIBLE 형식 대응)
 * ═══════════════════════════════════════════════════ */
static bool wav_parse(const uint8_t *b, uint32_t len,
		      uint32_t *sr, uint16_t *ch, uint16_t *bps,
		      uint32_t *data_off, uint32_t *data_sz)
{
	if (len < 12) {
		return false;
	}
	if (b[0] != 'R' || b[1] != 'I' || b[2] != 'F' || b[3] != 'F') {
		return false;
	}
	if (b[8] != 'W' || b[9] != 'A' || b[10] != 'V' || b[11] != 'E') {
		return false;
	}

	bool got_fmt = false, got_data = false;
	uint32_t pos = 12;

	while (pos + 8 <= len) {
		uint32_t csz = (uint32_t)b[pos + 4]
			     | ((uint32_t)b[pos + 5] << 8)
			     | ((uint32_t)b[pos + 6] << 16)
			     | ((uint32_t)b[pos + 7] << 24);

		if (b[pos]=='f' && b[pos+1]=='m' && b[pos+2]=='t' && b[pos+3]==' ') {
			/* fmt 청크: 최소 16바이트 필요 */
			if (pos + 8 + 16 > len) {
				return false;
			}
			/* 오프셋은 청크 데이터 시작(pos+8) 기준
			 * +2  NumChannels
			 * +4  SampleRate
			 * +14 BitsPerSample */
			*ch  = (uint16_t)(b[pos+10] | (b[pos+11] << 8));
			*sr  = (uint32_t)(b[pos+12] | (b[pos+13] << 8)
					| (b[pos+14] << 16) | (b[pos+15] << 24));
			*bps = (uint16_t)(b[pos+22] | (b[pos+23] << 8));
			got_fmt = true;

		} else if (b[pos]=='d' && b[pos+1]=='a' && b[pos+2]=='t' && b[pos+3]=='a') {
			*data_off = pos + 8;
			*data_sz  = csz;
			got_data  = true;
		}

		if (got_fmt && got_data) {
			break;
		}
		/* 청크 크기가 홀수면 1바이트 패딩 */
		pos += 8 + csz + (csz & 1U);
	}

	return got_fmt && got_data;
}

/* ═══════════════════════════════════════════════════
 *  I2S 재생 스레드
 *  - WAV 헤더 파싱 후 PCM 데이터를 stereo 블록으로 변환해 전송
 *  - VoiceBridge 출력(mono 8kHz 16bit)과 stereo WAV 모두 지원
 * ═══════════════════════════════════════════════════ */
static void playback_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	while (1) {
		k_sem_take(&g_play_sem, K_FOREVER);

		/* WAV 청크 탐색 파싱 */
		uint32_t sr, data_off, data_sz;
		uint16_t ch, bps;

		if (!wav_parse(g_wav, g_wav_written, &sr, &ch, &bps,
			       &data_off, &data_sz)) {
			printk("[PLAY] WAV 헤더 오류!\n");
			goto play_done;
		}

		/* data 청크가 수신 버퍼 범위를 벗어나지 않도록 클램프 */
		if (data_off >= g_wav_written) {
			printk("[PLAY] data 청크 오프셋 오류 (%u >= %u)\n",
			       data_off, g_wav_written);
			goto play_done;
		}
		uint32_t pcm_bytes = g_wav_written - data_off;

		if (data_sz < pcm_bytes) {
			pcm_bytes = data_sz;
		}
		printk("[PLAY] %u Hz | %u ch | %u bit | PCM@+%u %u bytes\n",
		       sr, ch, bps, data_off, pcm_bytes);

		/* I2S 설정: 항상 stereo 16비트 출력 (8비트 입력도 int16으로 변환) */
		struct i2s_config cfg = {
			.word_size      = 16,
			.channels       = 2,
			.format         = I2S_FMT_DATA_FORMAT_I2S,
			.options        = I2S_OPT_BIT_CLK_MASTER
					| I2S_OPT_FRAME_CLK_MASTER,
			.frame_clk_freq = sr,
			.mem_slab       = &i2s_slab,
			.block_size     = I2S_BLK_BYTES,
			.timeout        = 2000,
		};

		if (i2s_configure(i2s_dev, I2S_DIR_TX, &cfg) < 0) {
			printk("[PLAY] I2S 설정 실패!\n");
			goto play_done;
		}

		/* PCM 데이터 포인터 및 프레임 수 계산 */
		const uint8_t *pcm_raw       = g_wav + data_off;
		uint32_t       bytes_per_frm = (uint32_t)ch * (bps / 8);
		uint32_t       n_frames      = pcm_bytes / bytes_per_frm;

		/* stereo 출력 블록당 L/R pair 수 */
		const uint32_t pairs_per_blk = I2S_BLK_BYTES
					       / (2U * sizeof(int16_t));

		uint32_t frm_off = 0;
		bool     started = false;
		int      queued  = 0;

		while (frm_off < n_frames) {
			void *blk;

			/* slab 할당 대기 (driver가 DMA 완료 후 반환) */
			if (k_mem_slab_alloc(&i2s_slab, &blk, K_MSEC(500)) < 0) {
				printk("[PLAY] slab 할당 실패\n");
				break;
			}

			int16_t *out = (int16_t *)blk;
			uint32_t p;

			/* 프레임 → stereo int16 변환 */
			for (p = 0;
			     p < pairs_per_blk && frm_off < n_frames;
			     p++, frm_off++) {
				int16_t s;

				if (bps == 8) {
					/* WAV 8비트: unsigned uint8 (0~255, 무음=128)
					 * → signed int16 변환 후 AUDIO_GAIN 배 증폭
					 * 8비트 TTS는 저진폭으로 인코딩되는 경우가 많아
					 * (val-128)*256 만으로는 "하세요" 등 약음절이
					 * 가청 임계값에 못 미침 → GAIN으로 보정 */
					const uint8_t *b =
						pcm_raw + frm_off * bytes_per_frm;
					int32_t l32 = (int32_t)b[0] - 128;
					int32_t r32 = (ch > 1)
						? ((int32_t)b[1] - 128)
						: l32;
					int32_t avg = (ch > 1)
						? ((l32 + r32) >> 1)
						: l32;
					int32_t amp = avg * 256 * AUDIO_GAIN;
					s = (int16_t)(amp >  32767 ?  32767 :
						      amp < -32768 ? -32768 : amp);
				} else {
					/* 16비트 signed PCM */
					const int16_t *frm = (const int16_t *)
						(pcm_raw + frm_off * bytes_per_frm);
					s = (ch == 1)
						? frm[0]
						: (int16_t)(((int32_t)frm[0]
							     + frm[1]) >> 1);
				}

				/* 완전 묵음 시 MAX98357A 자동 슬립 방지
				 * 0x0000이 1초 이상 지속되면 앰프가 슬립 진입 →
				 * 다음 소리 시작 시 짧은 무음 발생
				 * ±1 dither는 -90dBFS로 청각적으로 무해 */
				if (s == 0) {
					s = (int16_t)((p & 1U) ? 1 : -1);
				}

				out[p * 2]     = s;   /* Left  */
				out[p * 2 + 1] = s;   /* Right */
			}

			/* 마지막 블록 zero-padding */
			for (; p < pairs_per_blk; p++) {
				out[p * 2] = out[p * 2 + 1] = 0;
			}

			i2s_write(i2s_dev, blk, I2S_BLK_BYTES);
			queued++;

			/* 2블록 선채우기 후 DMA 시작 (언더런 방지) */
			if (!started && queued >= 2) {
				i2s_trigger(i2s_dev, I2S_DIR_TX,
					    I2S_TRIGGER_START);
				started = true;
			}
		}

		/* 블록이 1개뿐일 때도 시작 */
		if (!started && queued > 0) {
			i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		}
		/* 큐 소진 후 정지 */
		if (queued > 0) {
			i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
		}

		printk("[PLAY] 재생 완료!\n");

play_done:
		leds_all(0);
		g_state = ST_IDLE;
	}
}

K_THREAD_DEFINE(playback_tid, 4096,
		playback_thread, NULL, NULL, NULL, 5, 0, 0);

/* ═══════════════════════════════════════════════════
 *  main
 * ═══════════════════════════════════════════════════ */
int main(void)
{
	/* LED 초기화 */
	for (int i = 0; i < 4; i++) {
		if (!gpio_is_ready_dt(&leds[i])) {
			printk("LED %d 준비 안됨\n", i);
			return -1;
		}
		gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
	}

	/* I2S 디바이스 확인 */
	if (!device_is_ready(i2s_dev)) {
		printk("I2S 디바이스 준비 안됨\n");
		return -1;
	}

	/* Bluetooth 초기화 */
	if (bt_enable(NULL) < 0) {
		printk("BT 초기화 실패\n");
		return -1;
	}
	if (bt_nus_init(&nus_cb) < 0) {
		printk("NUS 초기화 실패\n");
		return -1;
	}
	if (bt_le_adv_start(BT_LE_ADV_CONN, adv, ARRAY_SIZE(adv),
			    NULL, 0) < 0) {
		printk("BLE 광고 시작 실패\n");
		return -1;
	}

	printk("VoiceBridge Ready! 광고 중: \"%s\"\n", CONFIG_BT_DEVICE_NAME);

	/* 부팅 완료 표시: LED 순차 점등 후 소등 */
	for (int i = 0; i < 4; i++) {
		led_set(i, 1);
		k_sleep(K_MSEC(100));
	}
	k_sleep(K_MSEC(300));
	leds_all(0);

	return 0;
}
