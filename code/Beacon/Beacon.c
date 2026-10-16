#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/status_led.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "version.h"

// ============================================================
//  Hardware configuration (edit in code)
// ============================================================
#define TONE_PIN          15   // Telegraph tone output (to transmitter input)
#define PTT_PIN           14   // PTT output (to transmitter)
#define KEY_PIN           7    // CW key output (to transmitter key input via transistor)

// PTT and CW key outputs are active-high: 1 while transmitting / keyed
#define PTT_LEAD_MS       1000  // PTT lead-in delay before transmission, ms
#define PTT_TAIL_MS       100  // PTT tail delay after transmission, ms
#define TX_MAX_MS         120000 // Time-out timer: PTT is forcibly released after this, ms
#define WATCHDOG_MS       3000 // Watchdog timeout: reboot if the main loop hangs, ms

// Sine tone synthesis: PWM carrier = sample rate (ultrasonic, filtered out by
// the transmitter audio input or an RC low-pass), DDS sine with a
// raised-cosine envelope to avoid key clicks
#define SYNTH_RATE_HZ     62500 // PWM carrier / sample rate, Hz
#define SINE_BITS         10    // Sine table size: 2^SINE_BITS entries
#define RAMP_MS           5     // Tone rise / fall time, ms
#define RAMP_N            (SYNTH_RATE_HZ * RAMP_MS / 1000)

// ============================================================
//  Beacon configuration (stored in flash, editable via USB console)
// ============================================================
#define MSG_MAX_LEN     96
#define DEFAULT_TONE_HZ 800    // Tone frequency, Hz (600 / 800 or other)
#define DEFAULT_PAUSE_S 10     // Pause between message repeats, s
#define DEFAULT_WPM     20     // Keying speed, words per minute
#define DEFAULT_LED_PCT 20     // Status LED brightness, %
#define DEFAULT_MSG     "CQ CQ CQ DE RP2040"

// Tone sequence limits
#define TONES_MAX_LEN   96     // Max tone sequence string length
#define TONES_MAX_COUNT 24     // Max number of tone entries

// Transmission mode
enum { MODE_CW = 0, MODE_TONES = 1, MODE_BOTH = 2 };

#define CONFIG_MAGIC_V1 0xBEAC0AC4u  // legacy layout (no version / CRC)
#define CONFIG_MAGIC    0xBEAC0C0Fu
#define CONFIG_VERSION  2

typedef struct {
    uint32_t magic;
    uint32_t version;          // CONFIG_VERSION
    uint32_t tone_hz;          // CW tone frequency, Hz
    uint32_t pause_s;          // Pause between messages, s
    uint32_t wpm;              // Keying speed, words per minute
    uint32_t tx_mode;          // Transmission mode: MODE_CW / MODE_TONES / MODE_BOTH
    uint32_t tx_enabled;       // Transmission enabled: 1 = transmit, 0 = paused
    uint32_t led_pct;          // Status LED brightness, 0..100 %
    char     msg[MSG_MAX_LEN];     // Message text (Latin letters/digits)
    char     tones[TONES_MAX_LEN]; // Tone sequence "freq:ms ..." (freq 0 = silence)
    uint32_t crc;              // CRC-32 of all preceding bytes
} beacon_config_t;

// Legacy (v1) layout, migrated on load
typedef struct {
    uint32_t magic, tone_hz, pause_s, wpm, tx_mode, tx_enabled;
    char     msg[MSG_MAX_LEN];
    char     tones[TONES_MAX_LEN];
} beacon_config_v1_t;

static_assert(sizeof(beacon_config_t) <= FLASH_PAGE_SIZE, "config must fit in one flash page");

static const beacon_config_t cfg_default = {
    .magic           = CONFIG_MAGIC,
    .version         = CONFIG_VERSION,
    .tone_hz         = DEFAULT_TONE_HZ,
    .pause_s         = DEFAULT_PAUSE_S,
    .wpm             = DEFAULT_WPM,
    .tx_mode         = MODE_CW,
    .tx_enabled      = 1,
    .led_pct         = DEFAULT_LED_PCT,
    .msg             = DEFAULT_MSG,
    .tones           = "",
};

static beacon_config_t cfg;

// Dot duration in us (standard PARIS timing: 1.2 s / WPM)
static inline uint32_t dot_us(uint32_t wpm) { return 1200000u / wpm; }

static bool msg_valid(const char *s, const char **err);
static bool tones_validate(const char *s);

// Last flash sector - non-volatile configuration storage
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

// Range and syntax check of every field (also used for flash contents)
static bool config_valid(const beacon_config_t *c)
{
    return c->tone_hz >= 100 && c->tone_hz <= 5000 &&
           c->pause_s <= 3600 &&
           c->wpm >= 5 && c->wpm <= 60 &&
           c->tx_mode <= MODE_BOTH &&
           c->tx_enabled <= 1 &&
           c->led_pct <= 100 &&
           memchr(c->msg, '\0', MSG_MAX_LEN) && c->msg[0] &&
           msg_valid(c->msg, NULL) &&
           memchr(c->tones, '\0', TONES_MAX_LEN) &&
           (c->tones[0] == '\0' || tones_validate(c->tones));
}

static void config_load(void)
{
    const void *flash = (const void *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    const beacon_config_t *p = flash;
    const beacon_config_v1_t *v1 = flash;
    beacon_config_t c = cfg_default;

    if (p->magic == CONFIG_MAGIC && p->version == CONFIG_VERSION &&
        p->crc == crc32(p, offsetof(beacon_config_t, crc))) {
        c = *p;
    } else if (v1->magic == CONFIG_MAGIC_V1) {
        // Migrate the legacy layout (LED brightness gets the default)
        c.tone_hz    = v1->tone_hz;
        c.pause_s    = v1->pause_s;
        c.wpm        = v1->wpm;
        c.tx_mode    = v1->tx_mode;
        c.tx_enabled = v1->tx_enabled;
        memcpy(c.msg, v1->msg, MSG_MAX_LEN);
        memcpy(c.tones, v1->tones, TONES_MAX_LEN);
        c.msg[MSG_MAX_LEN - 1] = '\0';
        c.tones[TONES_MAX_LEN - 1] = '\0';
        printf("Legacy config found, migrating (use 'save' to store in the new format)\n");
    } else {
        printf("No valid config in flash, using defaults\n");
        return;
    }
    if (!config_valid(&c)) {
        printf("Config in flash is invalid, using defaults\n");
        return;
    }
    cfg = c;
    printf("Config loaded from flash: tone=%u Hz, pause=%u s, wpm=%u, mode=%u, txen=%u\n",
           cfg.tone_hz, cfg.pause_s, cfg.wpm, cfg.tx_mode, cfg.tx_enabled);
}

static void config_save(void)
{
    static uint8_t page_buf[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    cfg.magic   = CONFIG_MAGIC;
    cfg.version = CONFIG_VERSION;
    cfg.crc     = crc32(&cfg, offsetof(beacon_config_t, crc));
    memcpy(page_buf, &cfg, sizeof(cfg));
    memset(page_buf + sizeof(cfg), 0xFF, sizeof(page_buf) - sizeof(cfg));

    watchdog_update(); // sector erase can take up to ~400 ms
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    printf("Config saved to flash\n");
}

// ============================================================
//  Sine tone synthesis (PWM + DDS in the PWM wrap interrupt)
//  The tone output idles at mid level (DC); a tone fades in and out
//  with a raised-cosine envelope over RAMP_MS.
// ============================================================
static uint tone_slice, tone_chan;
static uint32_t synth_rate;            // actual sample rate, Hz
static uint16_t synth_mid;             // PWM level of the zero line
static int16_t  sine_tab[1u << SINE_BITS];
static uint16_t ramp_tab[RAMP_N + 1];  // envelope 0..32767
static volatile uint32_t phase_inc;
static volatile bool synth_on;         // envelope target: true = fade in
static uint32_t phase, env_idx;

static void __isr __not_in_flash_func(synth_isr)(void)
{
    pwm_clear_irq(tone_slice);
    if (synth_on) {
        if (env_idx < RAMP_N) env_idx++;
    } else if (env_idx) {
        env_idx--;
    } else {
        // Faded out: park at the zero line and stop interrupting
        pwm_set_chan_level(tone_slice, tone_chan, synth_mid);
        pwm_set_irq_enabled(tone_slice, false);
        return;
    }
    phase += phase_inc;
    int32_t s = (sine_tab[phase >> (32 - SINE_BITS)] * (int32_t)ramp_tab[env_idx]) >> 15;
    pwm_set_chan_level(tone_slice, tone_chan, (uint16_t)(synth_mid + ((s * synth_mid) >> 15)));
}

static void synth_init(void)
{
    for (uint32_t i = 0; i < (1u << SINE_BITS); i++)
        sine_tab[i] = (int16_t)lroundf(31129.0f * sinf(2.0f * (float)M_PI * i / (1u << SINE_BITS))); // 95% of full scale
    for (uint32_t i = 0; i <= RAMP_N; i++)
        ramp_tab[i] = (uint16_t)lroundf(32767.0f * 0.5f * (1.0f - cosf((float)M_PI * i / RAMP_N)));

    tone_slice = pwm_gpio_to_slice_num(TONE_PIN);
    tone_chan  = pwm_gpio_to_channel(TONE_PIN);
    uint32_t wrap = clock_get_hz(clk_sys) / SYNTH_RATE_HZ - 1;
    synth_rate = clock_get_hz(clk_sys) / (wrap + 1);
    synth_mid  = (uint16_t)((wrap + 1) / 2);

    pwm_set_clkdiv(tone_slice, 1.0f);
    pwm_set_wrap(tone_slice, (uint16_t)wrap);
    pwm_set_chan_level(tone_slice, tone_chan, synth_mid);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, synth_isr);
    irq_set_priority(PWM_IRQ_WRAP, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_set_enabled(tone_slice, true);
    gpio_set_function(TONE_PIN, GPIO_FUNC_PWM);
}

static void synth_start(uint32_t freq_hz)
{
    phase_inc = (uint32_t)(((uint64_t)freq_hz << 32) / synth_rate);
    synth_on = true;
    pwm_set_irq_enabled(tone_slice, true);
}

static void synth_stop(void)
{
    synth_on = false; // the interrupt fades out and then disables itself
}

// ============================================================
//  Keying outputs
// ============================================================
static volatile bool key_state;

// CW key output (drives the transmitter key input via a transistor);
// active while the tone sounds
static void key_set(bool keyed)
{
    key_state = keyed;
    gpio_put(KEY_PIN, keyed); // active high
}

static void tone_on(uint32_t freq_hz)
{
    synth_start(freq_hz);
    key_set(true);
}

static void tone_off(void)
{
    key_set(false);
    synth_stop();
}

static void ptt_set(bool active)
{
    gpio_put(PTT_PIN, active); // active high
}

// ============================================================
//  Status LED (WS2812)
//  - during transmission: red, blinking in sync with the keying
//    (red while the tone/key sounds, off in the gaps)
//  - during the pause: steady green
//  Updated from the main loop (the keying itself runs from a timer interrupt).
// ============================================================
static volatile bool tx_active = false;

enum { LED_UNSET, LED_OFF, LED_RED, LED_GREEN };

static void led_update(void)
{
    static int shown = LED_UNSET;
    static uint32_t shown_pct;
    int want = tx_active ? (key_state ? LED_RED : LED_OFF) : LED_GREEN;
    if (want == shown && cfg.led_pct == shown_pct) return;
    shown = want;
    shown_pct = cfg.led_pct;
    uint32_t v = 255u * cfg.led_pct / 100u;
    if (want == LED_OFF || v == 0)
        colored_status_led_set_state(false);
    else if (want == LED_RED)
        colored_status_led_set_on_with_color(PICO_COLORED_STATUS_LED_COLOR_FROM_RGB(v, 0, 0));
    else
        colored_status_led_set_on_with_color(PICO_COLORED_STATUS_LED_COLOR_FROM_RGB(0, v, 0));
}

// ============================================================
//  Morse code
// ============================================================
static const char *morse_char(char c)
{
    switch (toupper((unsigned char)c)) {
    case 'A': return ".-";     case 'B': return "-...";
    case 'C': return "-.-.";   case 'D': return "-..";
    case 'E': return ".";      case 'F': return "..-.";
    case 'G': return "--.";    case 'H': return "....";
    case 'I': return "..";     case 'J': return ".---";
    case 'K': return "-.-";    case 'L': return ".-..";
    case 'M': return "--";     case 'N': return "-.";
    case 'O': return "---";    case 'P': return ".--.";
    case 'Q': return "--.-";   case 'R': return ".-.";
    case 'S': return "...";    case 'T': return "-";
    case 'U': return "..-";    case 'V': return "...-";
    case 'W': return ".--";    case 'X': return "-..-";
    case 'Y': return "-.--";   case 'Z': return "--..";
    case '0': return "-----";  case '1': return ".----";
    case '2': return "..---";  case '3': return "...--";
    case '4': return "....-";  case '5': return ".....";
    case '6': return "-....";  case '7': return "--...";
    case '8': return "---..";  case '9': return "----.";
    case '.': return ".-.-.-"; case ',': return "--..--";
    case '?': return "..--.."; case '/': return "-..-.";
    case '-': return "-....-"; case '=': return "-...-";
    case '@': return ".--.-."; case ':': return "---...";
    case '+': return ".-.-.";  case '(': return "-.--.";
    case ')': return "-.--.-"; case '"': return ".-..-.";
    case '\'': return ".----."; case '!': return "-.-.--";
    default:  return NULL;
    }
}

// Check a message: Morse characters, spaces and prosigns written as
// letters in angle brackets (e.g. <SK>, <BT>: sent without letter gaps).
// On error *err (if not NULL) points to a description.
static bool msg_valid(const char *s, const char **err)
{
    static char buf[48];
    const char *e = NULL;
    bool in = false;
    int plen = 0;
    for (; *s && !e; s++) {
        if (*s == '<') {
            if (in) e = "nested '<'";
            in = true;
            plen = 0;
        } else if (*s == '>') {
            if (!in || plen < 2) e = "bad prosign, use e.g. <SK>";
            in = false;
        } else if (*s == ' ') {
            if (in) e = "space inside a prosign";
        } else if (!morse_char(*s)) {
            snprintf(buf, sizeof(buf), "unsupported character '%c'", *s);
            e = buf;
        } else if (in) {
            plen++;
        }
    }
    if (!e && in) e = "unclosed '<'";
    if (err) *err = e;
    return !e;
}

// ============================================================
//  Tone sequence "freq:ms freq:ms ..." (freq 0 = silence)
// ============================================================

// Copy the next space-separated token of *s into tok[TOK_LEN].
// Returns 1 for a token, 0 at the end of the string, -1 if it is too long.
#define TOK_LEN 16
static int next_token(const char **s, char *tok)
{
    while (**s == ' ') (*s)++;
    if (!**s) return 0;
    int i = 0;
    while (**s && **s != ' ') {
        if (i >= TOK_LEN - 1) return -1;
        tok[i++] = *(*s)++;
    }
    tok[i] = '\0';
    return 1;
}

// Parse one "freq:ms" entry strictly (no trailing garbage, values in range)
static bool tone_entry_parse(const char *tok, long *f, long *d)
{
    char *end;
    *f = strtol(tok, &end, 10);
    if (end == tok || *end != ':') return false;
    const char *p = end + 1;
    *d = strtol(p, &end, 10);
    if (end == p || *end) return false;
    return *f >= 0 && *f <= 5000 && *d >= 10 && *d <= 10000;
}

// Validate a tone sequence: up to TONES_MAX_COUNT valid entries, at least one
static bool tones_validate(const char *s)
{
    uint32_t count = 0;
    char tok[TOK_LEN];
    int r;
    long f, d;
    while ((r = next_token(&s, tok)) > 0) {
        if (!tone_entry_parse(tok, &f, &d)) return false;
        if (++count > TONES_MAX_COUNT) return false;
    }
    return r == 0 && count > 0;
}

// ============================================================
//  Transmission schedule
//  The whole transmission is laid out in advance as a list of
//  (frequency, duration) events and played by a hardware timer alarm,
//  so keying timing does not depend on the console or USB output.
// ============================================================
typedef struct {
    uint16_t freq;     // tone frequency, Hz; 0 = silence (key up)
    uint32_t dur_us;
} tx_event_t;

// Worst case: 95 chars x 6 elements x (tone + gap) + tones + separator
#define TX_EVENTS_MAX 1200

typedef struct {
    tx_event_t *ev;    // NULL: only count the duration
    uint32_t n;
    uint64_t total_us;
    bool     overflow;
} sched_t;

static void sched_add(sched_t *s, uint32_t freq, uint32_t dur_us)
{
    s->total_us += dur_us;
    if (!s->ev) return;
    if (!freq && s->n && !s->ev[s->n - 1].freq) { // merge adjacent silences
        s->ev[s->n - 1].dur_us += dur_us;
        return;
    }
    if (s->n >= TX_EVENTS_MAX) {
        s->overflow = true;
        return;
    }
    s->ev[s->n++] = (tx_event_t){ (uint16_t)freq, dur_us };
}

static void sched_build(sched_t *s, const beacon_config_t *c)
{
    uint32_t dot = dot_us(c->wpm);
    if (c->tx_mode == MODE_CW || c->tx_mode == MODE_BOTH) {
        bool prosign = false;
        for (const char *p = c->msg; *p; p++) {
            if (*p == ' ') {
                sched_add(s, 0, dot * 4); // word gap (4 + 3 from previous char = 7)
                continue;
            }
            if (*p == '<') { prosign = true; continue; }
            if (*p == '>') {
                prosign = false;
                sched_add(s, 0, dot * 2); // character gap after the prosign
                continue;
            }
            const char *m = morse_char(*p);
            if (!m) continue;
            for (; *m; m++) {
                sched_add(s, c->tone_hz, *m == '.' ? dot : dot * 3); // dot / dash
                sched_add(s, 0, dot); // gap between elements within a character
            }
            if (!prosign)
                sched_add(s, 0, dot * 2); // character gap (2 + 1 = 3)
        }
        if (c->tx_mode == MODE_BOTH)
            sched_add(s, 0, dot * 7); // gap between message and tones
    }
    if (c->tx_mode == MODE_TONES || c->tx_mode == MODE_BOTH) {
        const char *t = c->tones;
        char tok[TOK_LEN];
        long f, d;
        while (next_token(&t, tok) > 0)
            if (tone_entry_parse(tok, &f, &d))
                sched_add(s, (uint32_t)f, (uint32_t)d * 1000u);
    }
}

// Transmission length (keying only, without PTT lead/tail), ms
static uint32_t tx_length_ms(const beacon_config_t *c)
{
    sched_t s = { 0 };
    sched_build(&s, c);
    return (uint32_t)(s.total_us / 1000u);
}

static void tx_length_check(void)
{
    uint32_t ms = tx_length_ms(&cfg) + PTT_LEAD_MS + PTT_TAIL_MS;
    if (ms > TX_MAX_MS)
        printf("warning: transmission takes %u.%u s, longer than the %u s time-out - "
               "it will be cut off\n", ms / 1000, ms % 1000 / 100, TX_MAX_MS / 1000);
}

// --- player (runs from the timer interrupt) ---
static tx_event_t tx_events[TX_EVENTS_MAX];
static uint32_t tx_event_count;
static volatile uint32_t tx_event_idx;
static volatile bool tx_playing;
static alarm_id_t tx_alarm;

static int64_t tx_alarm_cb(alarm_id_t id, void *user)
{
    (void)id; (void)user;
    if (tx_event_idx >= tx_event_count) {
        tone_off();
        tx_playing = false;
        return 0;
    }
    const tx_event_t *e = &tx_events[tx_event_idx++];
    if (e->freq) tone_on(e->freq);
    else         tone_off();
    // Negative: next alarm relative to this one's scheduled time - no drift
    return -(int64_t)e->dur_us;
}

static bool player_start(void)
{
    tx_event_idx = 0;
    tx_playing = true;
    tx_alarm = add_alarm_in_us(100, tx_alarm_cb, NULL, true);
    if (tx_alarm <= 0) {
        tx_playing = false;
        return false;
    }
    return true;
}

static void player_stop(void)
{
    if (tx_playing) {
        cancel_alarm(tx_alarm);
        tx_playing = false;
    }
    tone_off();
}

// ============================================================
//  USB console (configuration)
// ============================================================
#define CONSOLE_IDLE_FLUSH_MS 5000 // execute an unfinished line (no Enter sent) after this idle time
#define LINE_MAX_LEN          128

static volatile bool tx_running;   // PTT keyed, transmission in progress
static bool save_pending;          // 'save' requested during transmission

// Parse a decimal integer that must fill the whole string (surrounding
// spaces allowed) and lie in [min, max]. Rejects empty input and garbage
// such as "abc" or "10x", unlike atoi() which silently returns 0.
static bool parse_int(const char *s, long min, long max, long *out)
{
    char *end;
    while (*s == ' ') s++;
    if (!*s) return false;
    long v = strtol(s, &end, 10);
    while (*end == ' ') end++;
    if (*end || v < min || v > max) return false;
    *out = v;
    return true;
}

static void print_version(void)
{
    printf("Beacon %s (%s)\n", BEACON_VERSION, BEACON_GIT_VERSION);
}

// Parse and execute a command line
static void handle_command(char *line)
{
    // Trim surrounding spaces
    while (*line == ' ') line++;
    for (size_t n = strlen(line); n && line[n - 1] == ' '; n--) line[n - 1] = '\0';

    // Split the line: first word is the command, the rest is the argument
    char *arg = strchr(line, ' ');
    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ') arg++;
    } else {
        arg = line + strlen(line);
    }
    // Command is case-insensitive, but the argument case is preserved
    for (char *c = line; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *cmd = line;

    if (!strcmp(cmd, "msg")) {
        const char *err;
        if (!arg[0]) {
            printf("usage: msg <text>\n");
        } else if (strlen(arg) > MSG_MAX_LEN - 1) {
            printf("error: message too long (%u chars, max %u)\n",
                   (unsigned)strlen(arg), MSG_MAX_LEN - 1);
        } else if (!msg_valid(arg, &err)) {
            printf("error: %s\n", err);
        } else {
            strcpy(cfg.msg, arg);
            printf("msg set: %s (use 'save' to store)\n", cfg.msg);
            tx_length_check();
        }
    } else if (!strcmp(cmd, "tone")) {
        long hz;
        if (parse_int(arg, 100, 5000, &hz)) {
            cfg.tone_hz = (uint32_t)hz;
            printf("tone set: %u Hz (use 'save' to store)\n", cfg.tone_hz);
        } else printf("usage: tone <100..5000>\n");
    } else if (!strcmp(cmd, "pause")) {
        long s;
        if (parse_int(arg, 0, 3600, &s)) {
            cfg.pause_s = (uint32_t)s;
            printf("pause set: %u s (use 'save' to store)\n", cfg.pause_s);
        } else printf("usage: pause <0..3600>\n");
    } else if (!strcmp(cmd, "wpm")) {
        long w;
        if (parse_int(arg, 5, 60, &w)) {
            cfg.wpm = (uint32_t)w;
            printf("wpm set: %u (dot %u ms) (use 'save' to store)\n",
                   cfg.wpm, dot_us(cfg.wpm) / 1000);
            tx_length_check();
        } else printf("usage: wpm <5..60>\n");
    } else if (!strcmp(cmd, "tones")) {
        if (!arg[0]) {
            printf("tones: %s\n", cfg.tones[0] ? cfg.tones : "(empty)");
        } else if (strlen(arg) > TONES_MAX_LEN - 1) {
            printf("error: tone sequence too long (%u chars, max %u)\n",
                   (unsigned)strlen(arg), TONES_MAX_LEN - 1);
        } else if (tones_validate(arg)) {
            strcpy(cfg.tones, arg);
            printf("tones set: %s (use 'save' to store)\n", cfg.tones);
            tx_length_check();
        } else {
            printf("invalid sequence, usage: tones <freq:ms> ...\n"
                   "  e.g. tones 800:100 0:100 800:100 0:500\n"
                   "  freq 0..5000 Hz (0 = silence), ms 10..10000, max %d entries\n",
                   TONES_MAX_COUNT);
        }
    } else if (!strcmp(cmd, "txmode")) {
        for (char *c = arg; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strcmp(arg, "cw"))         cfg.tx_mode = MODE_CW;
        else if (!strcmp(arg, "tones")) cfg.tx_mode = MODE_TONES;
        else if (!strcmp(arg, "both"))  cfg.tx_mode = MODE_BOTH;
        else {
            printf("usage: txmode cw|tones|both\n"
                   "  cw    - morse message only\n"
                   "  tones - tone sequence only\n"
                   "  both  - morse message, then tone sequence\n");
            return;
        }
        printf("txmode set: %s (use 'save' to store)\n", arg);
        tx_length_check();
    } else if (!strcmp(cmd, "txen")) {
        long en;
        if (parse_int(arg, 0, 1, &en)) {
            cfg.tx_enabled = (uint32_t)en;
            printf("transmission %s (use 'save' to store)\n",
                   cfg.tx_enabled ? "ENABLED" : "DISABLED");
        } else printf("usage: txen <0|1>\n");
    } else if (!strcmp(cmd, "led")) {
        long pct;
        if (parse_int(arg, 0, 100, &pct)) {
            cfg.led_pct = (uint32_t)pct;
            printf("led set: %u %% (use 'save' to store)\n", cfg.led_pct);
        } else printf("usage: led <0..100>\n");
    } else if (!strcmp(cmd, "save")) {
        if (tx_running) {
            // Flash erase stops all interrupts (and the keying) - wait for the pause
            save_pending = true;
            printf("save deferred until the end of the transmission\n");
        } else {
            config_save();
        }
    } else if (!strcmp(cmd, "show")) {
        uint32_t len = tx_length_ms(&cfg);
        printf("--- Beacon config ---\n");
        printf("ver  : %s (%s)\n", BEACON_VERSION, BEACON_GIT_VERSION);
        printf("txen : %u\n", cfg.tx_enabled);
        printf("mode : %s\n", cfg.tx_mode == MODE_CW ? "cw" :
                              cfg.tx_mode == MODE_TONES ? "tones" : "both");
        printf("tone : %u Hz\n", cfg.tone_hz);
        printf("msg  : %s\n", cfg.msg);
        printf("tones: %s\n", cfg.tones[0] ? cfg.tones : "(empty)");
        printf("pause: %u s\n", cfg.pause_s);
        printf("wpm  : %u (dot %u ms)\n", cfg.wpm, dot_us(cfg.wpm) / 1000);
        printf("led  : %u %%\n", cfg.led_pct);
        printf("txlen: %u.%u s\n", len / 1000, len % 1000 / 100);
        printf("tot  : %u s\n", TX_MAX_MS / 1000);
        printf("ptt  : GP%u (active high)\n", PTT_PIN);
        printf("key  : GP%u (active high)\n", KEY_PIN);
        printf("tone pin: GP%u (sine, PWM %u kHz)\n", TONE_PIN, SYNTH_RATE_HZ / 1000);
        printf("---------------------\n");
    } else if (!strcmp(cmd, "default")) {
        cfg = cfg_default;
        printf("Defaults restored (use 'save' to store)\n");
    } else if (!strcmp(cmd, "ver")) {
        print_version();
    } else if (!strcmp(cmd, "help")) {
        printf("Commands:\n"
               "  msg <text>      - set message (A-Z 0-9 . , ? / - = @ : + ( ) \" ' !)\n"
               "                    prosigns in angle brackets, e.g. <SK> <BT> <AR>\n"
               "  tone <hz>       - CW tone 100..5000 (600/800 for audio)\n"
               "  tones <f:d> ... - tone sequence: freq:ms pairs, freq 0 = silence\n"
               "                    e.g. tones 800:100 0:100 800:100 0:500\n"
               "  txmode cw|tones|both - what to transmit\n"
               "  txen <0|1>      - enable/disable transmission (0 stops it at once)\n"
               "  pause <sec>     - pause between repeats\n"
               "  wpm <5..60>     - keying speed, words per minute\n"
               "  led <0..100>    - status LED brightness, %%\n"
               "  save            - store config to flash\n"
               "  show            - show config\n"
               "  default         - restore defaults\n"
               "  ver             - firmware version\n");
    } else if (line[0]) {
        printf("Unknown command '%s', type 'help'\n", line);
    }
}

// Execute a complete console line, unless it was too long or contained
// bytes the console cannot accept (e.g. Cyrillic in UTF-8)
static void console_exec(char *line, bool overflow, bool bad_byte)
{
    putchar('\n');
    if (overflow)
        printf("error: line too long (max %u chars)\n", LINE_MAX_LEN - 1);
    else if (bad_byte)
        printf("error: non-ASCII characters in the line, ignored\n");
    else
        handle_command(line);
}

// Poll the console: read and echo characters, execute complete lines.
// Must be called frequently (every few ms) so no characters are lost.
static void console_poll(void)
{
    static char line[LINE_MAX_LEN];
    static int len = 0;
    static bool overflow = false, bad_byte = false;
    static absolute_time_t last_rx;
    int c;

    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        last_rx = get_absolute_time();
        if (c == '\r' || c == '\n') {
            if (len || overflow || bad_byte) {
                line[len] = '\0';
                console_exec(line, overflow, bad_byte);
                len = 0;
                overflow = bad_byte = false;
            }
        } else if (c == '\b' || c == 0x7F) {
            if (len) {
                len--;
                printf("\b \b"); // erase the last character on the terminal
            }
        } else if (isprint(c)) {
            putchar(c); // echo back so every terminal shows typing
            if (len < (int)sizeof(line) - 1)
                line[len++] = (char)c;
            else
                overflow = true;
        } else {
            // Show any unexpected (non-ASCII / control) byte so nothing
            // gets swallowed silently
            printf("<%02X>", (unsigned)c);
            bad_byte = true;
        }
    }

    // Some terminals send no line ending - execute the line after a pause in typing
    if ((len || overflow || bad_byte) &&
        absolute_time_diff_us(last_rx, get_absolute_time()) >=
        (int64_t)CONSOLE_IDLE_FLUSH_MS * 1000) {
        line[len] = '\0';
        console_exec(line, overflow, bad_byte);
        len = 0;
        overflow = bad_byte = false;
    }
}

// One idle step outside of transmission: feed the watchdog, serve the console
static void idle_poll(void)
{
    watchdog_update();
    console_poll();
    led_update();
    sleep_ms(10);
}

// ============================================================
//  Transmission control
//  The main loop only supervises: it serves the console and the LED and
//  stops the transmission when it was disabled ('txen 0') or the
//  time-out timer expired.
// ============================================================
static absolute_time_t tx_tot_end;  // PTT time-out deadline
static const char *tx_abort_reason;

static bool tx_poll(void)
{
    watchdog_update();
    console_poll();
    led_update();
    if (!cfg.tx_enabled) {
        tx_abort_reason = "transmission disabled";
        return false;
    }
    if (time_reached(tx_tot_end)) {
        tx_abort_reason = "time-out timer expired";
        return false;
    }
    sleep_ms(1);
    return true;
}

static bool tx_wait_ms(uint32_t ms)
{
    absolute_time_t t = make_timeout_time_ms(ms);
    while (!time_reached(t))
        if (!tx_poll()) return false;
    return true;
}

static bool tx_wait_player(void)
{
    while (tx_playing)
        if (!tx_poll()) return false;
    return true;
}

// ============================================================
//  main
// ============================================================
int main()
{
    // Outputs first, before any delay: PTT and key must not float at power-up
    // PTT: output, initially inactive
    gpio_init(PTT_PIN);
    gpio_set_dir(PTT_PIN, GPIO_OUT);
    ptt_set(false);

    // CW key: output, initially not keyed
    gpio_init(KEY_PIN);
    gpio_set_dir(KEY_PIN, GPIO_OUT);
    key_set(false);

    // Tone: silent (zero line) until a tone starts
    synth_init();

    cfg = cfg_default;
    stdio_init_all();

    // Status LED: steady green until transmission starts
    status_led_init();
    led_update();

    sleep_ms(2000); // give the host time to open the USB console

    print_version();
    if (watchdog_caused_reboot())
        printf("WARNING: rebooted by watchdog\n");
    watchdog_enable(WATCHDOG_MS, true); // paused while a debugger halts the core

    config_load();
    led_update();
    printf("Type 'help' for commands\n");

    while (true) {
        if (!cfg.tx_enabled) {
            // Transmission disabled (txen 0): keep the console responsive,
            // stay silent with the LED green
            idle_poll();
            continue;
        }

        sched_t s = { .ev = tx_events };
        sched_build(&s, &cfg);
        tx_event_count = s.n;
        if (s.overflow)
            printf("warning: transmission too long, truncated\n");
        if (cfg.tx_mode == MODE_BOTH)
            printf("TX: %s + tones %s\n", cfg.msg, cfg.tones);
        else
            printf("TX: %s\n", cfg.tx_mode == MODE_TONES ? cfg.tones : cfg.msg);

        tx_running = true;
        ptt_set(true);
        tx_tot_end = make_timeout_time_ms(TX_MAX_MS);
        bool ok = tx_wait_ms(PTT_LEAD_MS);
        if (ok) {
            tx_active = true; // LED: red, blinking with the keying
            if (player_start()) {
                ok = tx_wait_player();
            } else {
                ok = false;
                tx_abort_reason = "no free timer alarm";
            }
        }
        player_stop();
        tx_active = false;
        if (ok)
            ok = tx_wait_ms(PTT_TAIL_MS + RAMP_MS); // includes the last tone fade-out
        ptt_set(false);
        tx_running = false;
        led_update(); // steady green during the pause
        if (ok)
            printf("TX done, pause %u s\n", cfg.pause_s);
        else
            printf("TX aborted: %s, PTT released\n", tx_abort_reason);

        if (save_pending) {
            save_pending = false;
            config_save();
        }

        // Pause between messages while keeping the console responsive
        // (re-reads pause_s, so a new value takes effect immediately;
        // 'txen 0' ends the pause)
        absolute_time_t pause_start = get_absolute_time();
        while (cfg.tx_enabled &&
               absolute_time_diff_us(pause_start, get_absolute_time()) <
               (int64_t)cfg.pause_s * 1000000)
            idle_poll();
    }
}
