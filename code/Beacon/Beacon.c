#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/status_led.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

// ============================================================
//  Hardware configuration (edit in code)
// ============================================================
#define TONE_PIN          15   // Telegraph tone output (to transmitter input)
#define PTT_PIN           14   // PTT output (to transmitter)
#define KEY_PIN           7    // CW key output (to transmitter key input via transistor)

#define PTT_ACTIVE_LEVEL  1    // PTT active level: 1 or 0
#define KEY_ACTIVE_LEVEL  1    // CW key active level (keyed = tone sounding): 1 or 0
#define PTT_LEAD_MS       100  // PTT lead-in delay before transmission, ms
#define PTT_TAIL_MS       100  // PTT tail delay after transmission, ms

// ============================================================
//  Beacon configuration (stored in flash, editable via USB console)
// ============================================================
#define MSG_MAX_LEN     96
#define DEFAULT_TONE_HZ 800    // Tone frequency, Hz (600 / 800 or other)
#define DEFAULT_PAUSE_S 10     // Pause between message repeats, s
#define DEFAULT_WPM     20     // Keying speed, words per minute

// Tone sequence limits
#define TONES_MAX_LEN   96     // Max tone sequence string length
#define TONES_MAX_COUNT 24     // Max number of tone entries

// Transmission mode
enum { MODE_CW = 0, MODE_TONES = 1, MODE_BOTH = 2 };

#define CONFIG_MAGIC    0xBEAC0AC3u

typedef struct {
    uint32_t magic;
    uint32_t tone_hz;          // CW tone frequency, Hz
    uint32_t pause_s;          // Pause between messages, s
    uint32_t ptt_active_level; // PTT active level
    uint32_t wpm;              // Keying speed, words per minute
    uint32_t tx_mode;          // Transmission mode: MODE_CW / MODE_TONES / MODE_BOTH
    uint32_t tx_enabled;       // Transmission enabled: 1 = transmit, 0 = paused
    char     msg[MSG_MAX_LEN];     // Message text (Latin letters/digits)
    char     tones[TONES_MAX_LEN]; // Tone sequence "freq:ms ..." (freq 0 = silence)
} beacon_config_t;

static beacon_config_t cfg = {
    .magic           = CONFIG_MAGIC,
    .tone_hz         = DEFAULT_TONE_HZ,
    .pause_s         = DEFAULT_PAUSE_S,
    .ptt_active_level = PTT_ACTIVE_LEVEL,
    .wpm             = DEFAULT_WPM,
    .tx_mode         = MODE_CW,
    .tx_enabled      = 1,
    .msg             = "CQ CQ CQ DE RP2040",
    .tones           = "",
};

// Dot duration in ms (standard PARIS timing: 1200 / WPM)
static inline uint32_t dot_ms(void) { return 1200u / cfg.wpm; }

// Last flash sector - non-volatile configuration storage
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

static void config_load(void)
{
    const beacon_config_t *p = (const beacon_config_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    if (p->magic == CONFIG_MAGIC &&
        p->tone_hz >= 100 && p->tone_hz <= 5000 &&
        p->pause_s <= 3600 &&
        p->wpm >= 5 && p->wpm <= 60 &&
        p->ptt_active_level <= 1 &&
        p->tx_mode <= MODE_BOTH &&
        p->tx_enabled <= 1 &&
        p->msg[0] >= ' ' &&
        (p->tones[0] == '\0' || p->tones[0] >= ' ')) {
        cfg = *p;
        cfg.msg[MSG_MAX_LEN - 1] = '\0';
        cfg.tones[TONES_MAX_LEN - 1] = '\0';
        printf("Config loaded from flash: tone=%u Hz, pause=%u s, wpm=%u, ptt_act=%u, mode=%u, txen=%u\n",
               cfg.tone_hz, cfg.pause_s, cfg.wpm, cfg.ptt_active_level, cfg.tx_mode, cfg.tx_enabled);
    } else {
        printf("No valid config in flash, using defaults\n");
    }
}

static void config_save(void)
{
    static uint8_t page_buf[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memcpy(page_buf, &cfg, sizeof(cfg));
    memset(page_buf + sizeof(cfg), 0xFF, sizeof(page_buf) - sizeof(cfg));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    printf("Config saved to flash\n");
}

// ============================================================
//  Status LED (WS2812)
//  - during transmission: red, blinking in sync with the keying
//    (red while the tone/key sounds, off in the gaps)
//  - during the pause: steady green
// ============================================================
static volatile bool tx_active = false;

#define LED_COLOR_RED   PICO_COLORED_STATUS_LED_COLOR_FROM_RGB(255, 0, 0)
#define LED_COLOR_GREEN PICO_COLORED_STATUS_LED_COLOR_FROM_RGB(0, 255, 0)

static void led_tx_on(void)   // keyed: red
{
    colored_status_led_set_on_with_color(LED_COLOR_RED);
}

static void led_gap_off(void) // gap during transmission: dark
{
    if (tx_active)
        colored_status_led_set_state(false);
}

static void led_idle_on(void) // pause / idle: steady green
{
    colored_status_led_set_on_with_color(LED_COLOR_GREEN);
}

// ============================================================
//  USB console (configuration)
// ============================================================
#define CONSOLE_IDLE_FLUSH_MS 5000 // execute an unfinished line (no Enter sent) after this idle time

// Parse and execute a command line
// Parse and execute a command line
static void handle_command(char *line);
static bool tones_validate(const char *s);

static void handle_command(char *line)
{
    static char arg[MSG_MAX_LEN] = "";
    arg[0] = '\0';

    // Split the line: first word is the command, the rest is the argument
    char *sp = strchr(line, ' ');
    if (sp) {
        *sp = '\0';
        strncpy(arg, sp + 1, sizeof(arg) - 1);
        arg[sizeof(arg) - 1] = '\0';
    }
    // Command is case-insensitive, but the argument case is preserved
    for (char *c = line; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *cmd = line;

    if (!strcmp(cmd, "msg")) {
        if (arg[0]) {
            strncpy(cfg.msg, arg, MSG_MAX_LEN - 1);
            cfg.msg[MSG_MAX_LEN - 1] = '\0';
            printf("msg set: %s (use 'save' to store)\n", cfg.msg);
        } else printf("usage: msg <text>\n");
    } else if (!strcmp(cmd, "tone")) {
        int hz = atoi(arg);
        if (hz >= 100 && hz <= 5000) {
            cfg.tone_hz = (uint32_t)hz;
            printf("tone set: %u Hz (use 'save' to store)\n", cfg.tone_hz);
        } else printf("usage: tone <100..5000>\n");
    } else if (!strcmp(cmd, "pause")) {
        int s = atoi(arg);
        if (s >= 0 && s <= 3600) {
            cfg.pause_s = (uint32_t)s;
            printf("pause set: %u s (use 'save' to store)\n", cfg.pause_s);
        } else printf("usage: pause <0..3600>\n");
    } else if (!strcmp(cmd, "wpm")) {
        int w = atoi(arg);
        if (w >= 5 && w <= 60) {
            cfg.wpm = (uint32_t)w;
            printf("wpm set: %u (dot %u ms) (use 'save' to store)\n",
                   cfg.wpm, dot_ms());
        } else printf("usage: wpm <5..60>\n");
    } else if (!strcmp(cmd, "ptt")) {
        int lvl = atoi(arg);
        if (lvl == 0 || lvl == 1) {
            cfg.ptt_active_level = (uint32_t)lvl;
            printf("ptt active level set: %u (use 'save' to store)\n", lvl);
        } else printf("usage: ptt <0|1>\n");
    } else if (!strcmp(cmd, "tones")) {
        if (arg[0]) {
            if (tones_validate(arg)) {
                strncpy(cfg.tones, arg, TONES_MAX_LEN - 1);
                cfg.tones[TONES_MAX_LEN - 1] = '\0';
                printf("tones set: %s (use 'save' to store)\n", cfg.tones);
            } else {
                printf("invalid sequence, usage: tones <freq:ms> ...\n"
                       "  e.g. tones 800:100 0:100 800:100 0:500\n"
                       "  freq 0..5000 Hz (0 = silence), ms 10..10000, max %d entries\n",
                       TONES_MAX_COUNT);
            }
        } else {
            printf("tones: %s\n", cfg.tones[0] ? cfg.tones : "(empty)");
        }
    } else if (!strcmp(cmd, "txmode")) {
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
    } else if (!strcmp(cmd, "txen")) {
        int en = atoi(arg);
        if (en == 0 || en == 1) {
            cfg.tx_enabled = (uint32_t)en;
            printf("transmission %s (use 'save' to store)\n",
                   cfg.tx_enabled ? "ENABLED" : "DISABLED");
        } else printf("usage: txen <0|1>\n");
    } else if (!strcmp(cmd, "save")) {
        config_save();
    } else if (!strcmp(cmd, "show")) {
        printf("--- Beacon config ---\n");
        printf("txen : %u\n", cfg.tx_enabled);
        printf("mode : %s\n", cfg.tx_mode == MODE_CW ? "cw" :
                              cfg.tx_mode == MODE_TONES ? "tones" : "both");
        printf("tone : %u Hz\n", cfg.tone_hz);
        printf("msg  : %s\n", cfg.msg);
        printf("tones: %s\n", cfg.tones[0] ? cfg.tones : "(empty)");
        printf("pause: %u s\n", cfg.pause_s);
        printf("wpm  : %u (dot %u ms)\n", cfg.wpm, dot_ms());
        printf("ptt  : GP%u, active level %u\n", PTT_PIN, cfg.ptt_active_level);
        printf("key  : GP%u, active level %u\n", KEY_PIN, KEY_ACTIVE_LEVEL);
        printf("tone pin: GP%u\n", TONE_PIN);
        printf("---------------------\n");
    } else if (!strcmp(cmd, "default")) {
        cfg.tone_hz = DEFAULT_TONE_HZ;
        cfg.pause_s = DEFAULT_PAUSE_S;
        cfg.wpm     = DEFAULT_WPM;
        cfg.ptt_active_level = PTT_ACTIVE_LEVEL;
        cfg.tx_mode = MODE_CW;
        cfg.tx_enabled = 1;
        strcpy(cfg.msg, "CQ CQ CQ DE RP2040");
        cfg.tones[0] = '\0';
        printf("Defaults restored (use 'save' to store)\n");
    } else if (!strcmp(cmd, "help")) {
        printf("Commands:\n"
               "  msg <text>      - set message (A-Z 0-9 . , ? / - =)\n"
               "  tone <hz>       - CW tone 100..5000 (600/800 for audio)\n"
               "  tones <f:d> ... - tone sequence: freq:ms pairs, freq 0 = silence\n"
               "                    e.g. tones 800:100 0:100 800:100 0:500\n"
               "  txmode cw|tones|both - what to transmit\n"
               "  txen <0|1>      - enable/disable transmission\n"
               "  pause <sec>     - pause between repeats\n"
               "  wpm <5..60>     - keying speed, words per minute\n"
               "  ptt <0|1>       - PTT active level\n"
               "  save            - store config to flash\n"
               "  show            - show config\n"
               "  default         - restore defaults\n");
    } else if (line[0]) {
        printf("Unknown command '%s', type 'help'\n", line);
    }
}

// Poll the console: read and echo characters, execute complete lines.
// Must be called frequently (every ~10 ms) so no characters are lost
// even while a message is being transmitted.
static void console_poll(void)
{
    static char line[128];
    static int len = 0;
    static int idle_count = 0;
    int c;

    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        idle_count = 0;
        if (c == '\r' || c == '\n') {
            if (len) {
                line[len] = '\0';
                putchar('\n');
                handle_command(line);
                len = 0;
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
        } else {
            // Show any unexpected (non-ASCII / control) byte so nothing
            // gets swallowed silently
            printf("<%02X>", (unsigned)c);
        }
    }

    // Some terminals send no line ending - execute the line after a pause in typing
    if (len && ++idle_count >= CONSOLE_IDLE_FLUSH_MS / 10) {
        line[len] = '\0';
        putchar('\n');
        handle_command(line);
        len = 0;
        idle_count = 0;
    }
}

// Blocking delay that keeps the console responsive
static void sleep_ms_poll(uint32_t ms)
{
    uint32_t elapsed = 0;
    while (elapsed < ms) {
        console_poll();
        sleep_ms(10);
        elapsed += 10;
    }
}

// ============================================================
//  Telegraph tone - hardware PWM (square wave)
// ============================================================
static uint tone_slice = 0, tone_chan = 0;

// CW key output (drives the transmitter key input via a transistor);
// active while the tone sounds
static void key_set(bool keyed)
{
    gpio_put(KEY_PIN, keyed ? (KEY_ACTIVE_LEVEL != 0)
                            : (KEY_ACTIVE_LEVEL == 0));
}

static void tone_start(uint32_t freq_hz)
{
    gpio_set_function(TONE_PIN, GPIO_FUNC_PWM);
    tone_slice = pwm_gpio_to_slice_num(TONE_PIN);
    tone_chan  = pwm_gpio_to_channel(TONE_PIN);

    uint32_t clock = clock_get_hz(clk_sys);
    float div = 1.0f;
    uint32_t wrap = (clock - 1) / freq_hz;
    while (wrap > 65535 && div < 256.0f) {
        div *= 2.0f;
        wrap = (uint32_t)((clock / div) / freq_hz) - 1;
    }
    if (wrap < 2) wrap = 2;

    pwm_set_clkdiv(tone_slice, div);
    pwm_set_wrap(tone_slice, wrap);
    pwm_set_chan_level(tone_slice, tone_chan, (wrap + 1) / 2); // 50% duty cycle
    pwm_set_enabled(tone_slice, true);
    key_set(true); // CW key follows the tone (keyed while tone sounds)
    led_tx_on();   // status LED: red while keyed
}

static void tone_stop(void)
{
    key_set(false);
    pwm_set_enabled(tone_slice, false);
    // Switch pin back to SIO and pull it low - silence on the output
    gpio_set_function(TONE_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(TONE_PIN, GPIO_OUT);
    gpio_put(TONE_PIN, false);
    if (tx_active)
        led_gap_off(); // dark in the gaps while transmitting
    else
        led_idle_on(); // steady green when not transmitting
}

// ============================================================
//  PTT
// ============================================================
static void ptt_set(bool active)
{
    gpio_put(PTT_PIN, active ? (cfg.ptt_active_level != 0)
                             : (cfg.ptt_active_level == 0));
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
    default:  return NULL;
    }
}

static void send_element(const char sym)
{
    if (sym == '.') {
        tone_start(cfg.tone_hz);
        sleep_ms_poll(dot_ms());
        tone_stop();
    } else { // '-'
        tone_start(cfg.tone_hz);
        sleep_ms_poll(dot_ms() * 3);
        tone_stop();
    }
    sleep_ms_poll(dot_ms()); // gap between elements within a character
}

static void send_message(void)
{
    for (const char *p = cfg.msg; *p; p++) {
        if (*p == ' ') {
            sleep_ms_poll(dot_ms() * 4); // word gap (4 + 3 from previous char = 7)
            continue;
        }
        const char *m = morse_char(*p);
        if (!m) continue; // skip unknown characters
        while (*m) send_element(*m++);
        sleep_ms_poll(dot_ms() * 2); // character gap (2 + 1 = 3)
    }
}

// Validate a tone sequence string: entries "freq:ms" separated by spaces,
// freq 0..5000 Hz (0 = silence), ms 10..10000, up to TONES_MAX_COUNT entries
static bool tones_validate(const char *s)
{
    uint32_t count = 0;
    char tok[16];
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        int i = 0;
        while (*s && *s != ' ') {
            if (i >= (int)sizeof(tok) - 1) return false;
            tok[i++] = *s++;
        }
        tok[i] = '\0';
        int f = 0, d = 0;
        if (sscanf(tok, "%d:%d", &f, &d) != 2) return false;
        if (f < 0 || f > 5000 || d < 10 || d > 10000) return false;
        if (++count > TONES_MAX_COUNT) return false;
    }
    return count > 0;
}

// Play the tone sequence from cfg.tones ("freq:ms ..." pairs, freq 0 = silence)
static void play_tones(void)
{
    static char buf[TONES_MAX_LEN];
    strncpy(buf, cfg.tones, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        int f = 0, d = 0;
        if (sscanf(tok, "%d:%d", &f, &d) != 2 || d <= 0) continue;
        if (f > 0) {
            tone_start((uint32_t)f);
            sleep_ms_poll((uint32_t)d);
            tone_stop();
        } else {
            sleep_ms_poll((uint32_t)d); // silence gap
        }
    }
}

// ============================================================
//  main
// ============================================================
int main()
{
    stdio_init_all();
    sleep_ms(2000);

    // PTT: output, initially inactive
    gpio_init(PTT_PIN);
    gpio_set_dir(PTT_PIN, GPIO_OUT);
    ptt_set(false);

    // CW key: output, initially not keyed
    gpio_init(KEY_PIN);
    gpio_set_dir(KEY_PIN, GPIO_OUT);
    key_set(false);

    // Tone: initially silent
    gpio_init(TONE_PIN);
    gpio_set_dir(TONE_PIN, GPIO_OUT);
    gpio_put(TONE_PIN, false);

    // Status LED: steady green until transmission starts
    status_led_init();
    led_idle_on();

    config_load();
    printf("Type 'help' for commands\n");

    while (true) {
        if (!cfg.tx_enabled) {
            // Transmission disabled (txen 0): keep the console responsive,
            // stay silent with the LED green
            for (uint32_t i = 0; i < 100; i++) {
                console_poll();
                sleep_ms(10);
            }
            continue;
        }
        printf("TX: %s\n", cfg.tx_mode == MODE_TONES ? cfg.tones : cfg.msg);
        ptt_set(true);
        sleep_ms_poll(PTT_LEAD_MS);
        tx_active = true; // LED: red, blinking with the keying
        if (cfg.tx_mode == MODE_CW || cfg.tx_mode == MODE_BOTH) {
            send_message();
            if (cfg.tx_mode == MODE_BOTH)
                sleep_ms_poll(dot_ms() * 7); // gap between message and tones
        }
        if (cfg.tx_mode == MODE_TONES || cfg.tx_mode == MODE_BOTH) {
            play_tones();
        }
        tx_active = false;
        sleep_ms_poll(PTT_TAIL_MS);
        ptt_set(false);
        led_idle_on(); // steady green during the pause
        printf("TX done, pause %u s\n", cfg.pause_s);

        // Pause between messages while keeping the console responsive
        for (uint32_t i = 0; i < cfg.pause_s * 100; i++) {
            console_poll();
            sleep_ms(10);
        }
    }
}
