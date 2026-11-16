// Part of MIDI Muppet 4btn – Copyright (C) 2026 Jon Solberg – GPL v3 or later, see LICENSE.
// Runs the MIDI Muppet firmware in simavr and lets a script drive it over stdin/stdout.
// Commands (one per line):
//   run <ms>          run simulated time; prints "TX <hex>" for UART output, then "DONE <ms>"
//   send <text>       queue text + "\n" for the UART input (paced at ~115200 baud)
//   sendhex <hex>     queue raw bytes
//   pin <1-4> <0|1>   drive button (0 = pressed)
//   eeprom <n>        print first n EEPROM bytes as "EE <hex>"
//   reset             reset the MCU (EEPROM is kept)
//   led               print "LED g=<0|1>"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>
#include <simavr/avr_uart.h>
#include <simavr/avr_ioport.h>
#include <simavr/avr_eeprom.h>
#include <simavr/sim_hex.h>

static avr_t *avr;
static unsigned char txbuf[4096]; static int txlen;
static unsigned char inq[8192]; static int inhead, intail;
static avr_irq_t *uart_in;
static int green;
static avr_cycle_count_t next_feed;

static int host_baud = 115200;
static uint32_t freq = 16000000;   // CPU clock, SIM_MHZ=8 for 3.3 V Pro Minis
// Baud the chip's UART is set to (UBRR0, U2X0); 0 if not set up
static int mcu_baud(void) {
  int ubrr = avr->data[0xC4] | (avr->data[0xC5] << 8);
  int div = (avr->data[0xC0] & 2) ? 8 : 16;
  return freq / (div * (ubrr + 1));
}
static int baud_ok(void) { if (!host_baud) return 1; int m = mcu_baud(); return m && abs(m - host_baud) * 100 < host_baud * 4; }
static void tx_cb(struct avr_irq_t *irq, uint32_t value, void *p) {
  if (txlen < (int)sizeof txbuf) txbuf[txlen++] = baud_ok() ? value : (value ^ 0xA5);   // wrong speed: garbage
}
static void green_cb(struct avr_irq_t *irq, uint32_t value, void *p) { green = value; }

static void flush_tx(void) {
  if (!txlen) return;
  printf("TX ");
  for (int i = 0; i < txlen; i++) printf("%02X", txbuf[i]);
  printf("\n"); txlen = 0;
}

static int btn_state[4] = {1, 1, 1, 1};
static void set_btn(int b, int v);
static void run_ms(double ms) {
  avr_cycle_count_t end = avr->cycle + (avr_cycle_count_t)(ms * freq / 1000.0);
  while (avr->cycle < end) {
    if (inhead != intail && avr->cycle >= next_feed) {
      uint8_t b = inq[inhead]; inhead = (inhead + 1) % sizeof inq;
      avr_raise_irq(uart_in, baud_ok() ? b : (b ^ 0x5A));     // wrong speed: garbage
      next_feed = avr->cycle + (uint64_t)freq * 10 / (host_baud ? host_baud : 115200);  // 10 bits per byte at the host's speed
    }
    static avr_cycle_count_t next_btn;
    if (avr->cycle >= next_btn) { next_btn = avr->cycle + freq / 1000; for (int i = 0; i < 4; i++) set_btn(i, btn_state[i]); }
    int st = avr_run(avr);
    if (st == cpu_Done || st == cpu_Crashed) { printf("CRASH %d\n", st); break; }
  }
}

static void queue(const unsigned char *b, int n) { for (int i = 0; i < n; i++) { inq[intail] = b[i]; intail = (intail + 1) % sizeof inq; } }

// Buttons are declared as external drives, so simavr's pull-up emulation
// does not overwrite them when the firmware writes other pins on the port.
static void set_btn(int b, int v) {
  static const char port[4] = {'D', 'D', 'D', 'B'};
  static const int bit[4] = {2, 3, 6, 0};
  btn_state[b] = v;
  avr_ioport_external_t ext = { .name = port[b], .mask = 0, .value = 0 };
  for (int i = 0; i < 4; i++) if (port[i] == port[b]) {
    ext.mask |= 1 << bit[i];
    if (btn_state[i]) ext.value |= 1 << bit[i];
  }
  avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL(port[b]), &ext);
  avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(port[b]), bit[b]), v);
}

static elf_firmware_t fw;
static const char *boot_hex;     // optional bootloader (reset goes there, like BOOTRST)
static uint32_t boot_addr;
static uint8_t *saved_flash; static int saved_flash_len;
static const char *mcu_name;

static void boot(const uint8_t *ee, int eesize) {
  avr = avr_make_mcu_by_name(mcu_name);
  avr_init(avr);
  avr->frequency = freq;
  avr->log = 0;
  avr_load_firmware(avr, &fw);
  if (saved_flash) memcpy(avr->flash, saved_flash, saved_flash_len);       // keep what was programmed
  if (boot_hex) {
    if (!saved_flash) {
      ihex_chunk_p chunks; int n = read_ihex_chunks(boot_hex, &chunks);
      boot_addr = 0xFFFFFFFF;
      for (int i = 0; i < n; i++) { memcpy(avr->flash + chunks[i].baseaddr, chunks[i].data, chunks[i].size); if (chunks[i].baseaddr < boot_addr) boot_addr = chunks[i].baseaddr; }
    }
    avr->reset_pc = boot_addr; avr->pc = boot_addr;
    avr->codeend = avr->flashend;
    avr->data[0x54] |= 2;      // MCUSR.EXTRF: started by the reset pin, so the bootloader waits for the uploader
  }
  if (ee) { avr_eeprom_desc_t d = { .ee = (uint8_t *)ee, .offset = 0, .size = eesize }; avr_ioctl(avr, AVR_IOCTL_EEPROM_SET, &d); }
  uint32_t flags = 0;
  avr_ioctl(avr, AVR_IOCTL_UART_GET_FLAGS('0'), &flags);
  flags &= ~AVR_UART_FLAG_STDIO;
  avr_ioctl(avr, AVR_IOCTL_UART_SET_FLAGS('0'), &flags);
  avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_OUTPUT), tx_cb, NULL);
  uart_in = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_INPUT);
  avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), 4), green_cb, NULL);
  for (int i = 0; i < 4; i++) set_btn(i, 1);
  next_feed = 0; inhead = intail = 0; txlen = 0;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 3 || elf_read_firmware(argv[1], &fw)) { fprintf(stderr, "usage: harness fw.elf mcu\n"); return 1; }
  mcu_name = argv[2];
  if (getenv("SIM_MHZ")) freq = atoi(getenv("SIM_MHZ")) * 1000000;
  if (argc > 3) boot_hex = argv[3];
  boot(NULL, 0);

  char line[8192];
  while (fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!strncmp(line, "run ", 4)) { run_ms(atof(line + 4)); flush_tx(); printf("DONE\n"); }
    else if (!strncmp(line, "send ", 5)) { queue((unsigned char *)line + 5, strlen(line + 5)); queue((const unsigned char *)"\n", 1); }
    else if (!strncmp(line, "sendhex ", 8)) {
      const char *h = line + 8; unsigned char b[4096]; int n = 0;
      while (h[0] && h[1]) { unsigned v; sscanf(h, "%2x", &v); b[n++] = v; h += 2; }
      queue(b, n);
    }
    else if (!strncmp(line, "pin ", 4)) { int b, v; sscanf(line + 4, "%d %d", &b, &v); set_btn(b - 1, v); }
    else if (!strncmp(line, "eeprom ", 7)) {
      int n = atoi(line + 7); avr_eeprom_desc_t d = { .ee = NULL, .offset = 0, .size = n };
      avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &d);
      printf("EE "); for (int i = 0; i < n; i++) printf("%02X", d.ee[i]); printf("\n");
    }
    else if (!strcmp(line, "reset")) {   // power cycle: new chip, same EEPROM
      static uint8_t ee[1024];
      int n = avr->e2end + 1; if (n > 1024) n = 1024;
      avr_eeprom_desc_t d = { .ee = NULL, .offset = 0, .size = n };
      avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &d);
      memcpy(ee, d.ee, n);
      if (boot_hex) {                 // a real reset keeps the flash
        saved_flash_len = avr->flashend + 1;
        if (!saved_flash) saved_flash = malloc(saved_flash_len);
        memcpy(saved_flash, avr->flash, saved_flash_len);
      }
      boot(ee, n);
    }
    else if (!strncmp(line, "flash ", 6)) {      // flash <start> <len>
      unsigned a, n; sscanf(line + 6, "%u %u", &a, &n);
      printf("FL "); for (unsigned i = 0; i < n; i++) printf("%02X", avr->flash[a + i]); printf("\n");
    }
    else if (!strncmp(line, "baud ", 5)) host_baud = atoi(line + 5);
    else if (!strcmp(line, "pc")) printf("PC %04X MCUSR %02X state %d\n", avr->pc, avr->data[0x54], avr->state);
    else if (!strcmp(line, "led")) printf("LED g=%d\n", green);
    else if (!strcmp(line, "quit")) break;
  }
  return 0;
}
