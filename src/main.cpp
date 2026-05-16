/*
 * Ethan O'Connor and Robert Griffin
 * ECE 304 JDP26 
 * HW12 - Final Submission
 * 5/11/26
 * 
 * PWM Interactive Test — Nucleo F303RE, Arduino framework
 * PWM output on PA8 (TIM1 CH1) and PA9 (TIM1 CH2), serial at 115200 baud.
 * User selects a step (1–16) from a table; both channels update together.
 * Type "Input X" (X = 0–7) to control a 3-bit MUX via PB0/PB1/PB3.
 * Type "mute" / "unmute" to drive INH (MUX enable) via PC7.
 */

#include <Arduino.h>

// ── Pin definitions ──────────────────────────────────────────────────────────
const int PWM_PIN1    = PA8;
const int PWM_PIN2    = PA9;

const int MUX_A       = PB0;
const int MUX_B       = PB1;
const int MUX_C       = PB3;
const int MUX_INH     = PC7;   // LOW = enabled, HIGH = muted
const int MUX_ZERO    = PC8;   // LOW = disabled, high = enabled

const int ENC_CLK     = PC10;  // rotary encoder A (interrupt)
const int ENC_DT      = PC11;  // rotary encoder B

const int SWITCH_MUTE = PC13;  // knife-switch, active-LOW

// ── Timer handle ─────────────────────────────────────────────────────────────
HardwareTimer *pwmTimer;

// ── PWM lookup table ─────────────────────────────────────────────────────────
struct PwmStep {
  uint8_t  num;
  uint32_t pwm1;
  uint32_t pwm2;
};

const PwmStep PWM_TABLE[] = {
  {  1,   60,   67 },
  {  2,   65,   72 },
  {  3,   71,   81 },
  {  4,   81,   94 },
  {  5,   95,  110 },
  {  6,  116,  130 },
  {  7,  140,  162 },
  {  8,  180,  207 },
  {  9,  236,  273 },
  { 10,  317,  368 },
  { 11,  444,  511 },
  { 12,  628,  710 },
  { 13,  894, 1020 },
  { 14, 1300, 1485 },
  { 15, 1920, 2270 },
  { 16, 2850, 3450 },
};
const uint8_t TABLE_LEN = sizeof(PWM_TABLE) / sizeof(PWM_TABLE[0]);

// ── LPF frequency table (16 steps, exponential 100–12000 Hz) ─────────────────
// f(i) = round(100 * 120^(i/15)),  i = 0..15
const uint16_t LPF_FREQ[16] = {
    100,  136,  185,  252,
    343,  467,  635,  865,
   1177, 1602, 2180, 2968,
   4040, 5498, 7483, 12000
};

// ── State ─────────────────────────────────────────────────────────────────────
uint32_t currentPWM1       = 0;
uint32_t currentPWM2       = 0;
int8_t   currentMuxOutput  = 1; // default to nice speaker / 3.5mm output

bool swMuted        = false;   // set by serial "mute"/"unmute"
bool hwMuted        = false;   // set by knife-switch polling
bool effectiveMuted = false;   // swMuted || hwMuted — the only thing driving INH

// Encoder
volatile int8_t  encRaw   = 0;      // ticks accumulated in ISR
volatile uint8_t encLastA = HIGH;   // last stable CLK level seen by ISR
int8_t           lpfStep  = 0;      // 0-based index into LPF_FREQ / PWM_TABLE

// ── INH management ───────────────────────────────────────────────────────────
// Single chokepoint: only this function ever touches MUX_INH after setup().
void applyMuteState() {
  bool want = swMuted || hwMuted;
  if (want != effectiveMuted) {
    effectiveMuted = want;
    digitalWrite(MUX_INH, effectiveMuted ? HIGH : LOW);
  }
}

// ── Rotary-encoder ISR ────────────────────────────────────────────────────────
// Fires on every CHANGE of CLK. Records a tick only on the genuine rising edge.
void encoderISR() {
  uint8_t a = digitalRead(ENC_CLK);
  if (a == HIGH && encLastA == LOW) {           // true rising edge only
    encRaw += (digitalRead(ENC_DT) == HIGH) ? 1 : -1;
  }
  encLastA = a;
}

// ── Apply a PWM step (0-based index) ─────────────────────────────────────────
void applyLpfStep(int8_t idx) {
  const PwmStep &s = PWM_TABLE[idx];
  currentPWM1 = s.pwm1;
  currentPWM2 = s.pwm2;
  pwmTimer->setCaptureCompare(1, currentPWM1, RESOLUTION_12B_COMPARE_FORMAT);
  pwmTimer->setCaptureCompare(2, currentPWM2, RESOLUTION_12B_COMPARE_FORMAT);

  Serial.print(F("LPF step "));
  Serial.print(idx + 1);
  Serial.print(F("/16  ->  "));
  Serial.print(LPF_FREQ[idx]);
  Serial.print(F(" Hz   PWM1="));
  Serial.print(currentPWM1);
  Serial.print(F("  PWM2="));
  Serial.println(currentPWM2);
  Serial.flush();
}

// ── MUX output ────────────────────────────────────────────────────────────────
void applyMuxOutput(uint8_t x) {
  digitalWrite(MUX_A, (x & 0b001) ? HIGH : LOW);
  digitalWrite(MUX_B, (x & 0b010) ? HIGH : LOW);
  digitalWrite(MUX_C, (x & 0b100) ? HIGH : LOW);
  currentMuxOutput = x;
  digitalWrite(MUX_ZERO, (x == 0) ? HIGH : LOW);

  Serial.print(F("MUX -> "));
  Serial.print(x);
  Serial.print(F("  (A="));
  Serial.print((x & 0b001) ? 1 : 0);
  Serial.print(F(" B="));
  Serial.print((x & 0b010) ? 1 : 0);
  Serial.print(F(" C="));
  Serial.print((x & 0b100) ? 1 : 0);
  Serial.println(F(")"));
  if (effectiveMuted)
    Serial.println(F("  (note: audio is muted - outputs inactive until unmuted)"));
  Serial.flush();
}

// ── Print LPF table ───────────────────────────────────────────────────────────
void printLpfTable() {
  Serial.println(F("\n Step | Freq (Hz) | PWM1 | PWM2"));
  Serial.println(F(" -----|-----------|------|------"));
  for (uint8_t i = 0; i < TABLE_LEN; i++) {
    Serial.print(i == (uint8_t)lpfStep ? F(" >> ") : F("    "));
    if (i + 1 < 10) Serial.print(' ');
    Serial.print(i + 1);
    Serial.print(F("  |  "));
    uint16_t f = LPF_FREQ[i];
    if (f < 10000) Serial.print(' ');
    if (f <  1000) Serial.print(' ');
    if (f <   100) Serial.print(' ');
    Serial.print(f);
    Serial.print(F("      |  "));
    Serial.print(PWM_TABLE[i].pwm1);
    Serial.print(F("  |  "));
    Serial.println(PWM_TABLE[i].pwm2);
  }
  Serial.println();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Reset error codes, and print out old errors. Prevents forced reset every minute that the STM32 is on.
  Serial.println(F("\n--- Reset cause ---"));
  uint32_t csr = RCC->CSR;
  if (csr & RCC_CSR_LPWRRSTF)  Serial.println(F("  LOW POWER reset"));
  if (csr & RCC_CSR_WWDGRSTF)  Serial.println(F("  WINDOW WATCHDOG reset  <--"));
  if (csr & RCC_CSR_IWDGRSTF)  Serial.println(F("  INDEPENDENT WATCHDOG reset  <--"));
  if (csr & RCC_CSR_SFTRSTF)   Serial.println(F("  SOFTWARE reset"));
  if (csr & RCC_CSR_PORRSTF)   Serial.println(F("  POWER-ON / PDR reset"));
  if (csr & RCC_CSR_PINRSTF)   Serial.println(F("  NRST PIN reset (button or external)"));
  if (csr & RCC_CSR_OBLRSTF)   Serial.println(F("  OPTION BYTE LOADER reset"));
  RCC->CSR |= RCC_CSR_RMVF; // Clear all flags so the next reset is reported cleanly
  Serial.println(F("-------------------\n"));

  // Wait a little bit to prevent COM port from not setting up properly.
  delay(2000);

  // MUX selector pins, mux_zero is really
  pinMode(MUX_A,    OUTPUT); digitalWrite(MUX_A,    LOW);
  pinMode(MUX_B,    OUTPUT); digitalWrite(MUX_B,    LOW);
  pinMode(MUX_C,    OUTPUT); digitalWrite(MUX_C,    LOW);
  pinMode(MUX_ZERO, OUTPUT); digitalWrite(MUX_ZERO, LOW);

  // INH — explicitly driven LOW before anything else, shadow vars match
  swMuted        = false;
  hwMuted        = false;
  effectiveMuted = false;
  pinMode(MUX_INH, OUTPUT);
  digitalWrite(MUX_INH, LOW);   // unconditional physical write at boot

  // Encoder — settle pull-ups before arming ISR to avoid spurious ticks
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  delay(10);                           // let line capacitance charge
  encLastA = digitalRead(ENC_CLK);     // snapshot before ISR sees any edges
  encRaw   = 0;
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);

  // Knife-switch — read state AFTER INH is already pinned LOW
  pinMode(SWITCH_MUTE, INPUT_PULLUP);
  delay(1);
  hwMuted = (digitalRead(SWITCH_MUTE) == LOW);
  applyMuteState();   // only raises INH if switch is already closed at boot

  // TIM1
  pwmTimer = new HardwareTimer(TIM1);
  pwmTimer->setMode(1, TIMER_OUTPUT_COMPARE_PWM1, PWM_PIN1);
  pwmTimer->setMode(2, TIMER_OUTPUT_COMPARE_PWM1, PWM_PIN2);
  pwmTimer->setOverflow(17578, HERTZ_FORMAT);

  // Apply step 0 (100 Hz) as power-on default
  lpfStep = 0;
  applyLpfStep(lpfStep);   // also writes CCR registers
  pwmTimer->resume();

  Serial.println(F("=== PWM / LPF Control ==="));
  Serial.println(F("Rotate encoder  -> change LPF cutoff (steps 1-16, 100-12000 Hz)"));
  Serial.println(F("Type 'T'        -> print LPF table with current step marked"));
  Serial.println(F("Type 'Input X'  -> set MUX input (X = 0-7)"));
  Serial.println(F("Type 'mute'     -> software mute ON"));
  Serial.println(F("Type 'unmute'   -> software mute OFF"));
  Serial.println(F("Knife switch    -> hardware mute (closed=muted, open=unmuted)\n"));

  printLpfTable();

  if (hwMuted)
    Serial.println(F("[!] Knife switch is CLOSED — hardware mute active"));
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {

  // ── 1. Knife-switch polling ────────────────────────────────────────────────
  {
    bool closed = (digitalRead(SWITCH_MUTE) == LOW);
    if (closed != hwMuted) {
      hwMuted = closed;
      applyMuteState();
      if (hwMuted) {
        Serial.println(F("[Knife switch CLOSED - hardware mute ON]"));
      } else {
        Serial.println(F("[Knife switch OPEN   - hardware mute OFF]"));
        if (swMuted)
          Serial.println(F("  (software mute still active)"));
      }
      Serial.flush();
    }
  }

  // ── 2. Encoder tick processing ─────────────────────────────────────────────
  {
    noInterrupts();
    int8_t ticks = encRaw;
    encRaw = 0;
    interrupts();

    if (ticks != 0) {
      int8_t next = lpfStep + ticks;
      if (next < 0)  next = 0;
      if (next > 15) next = 15;
      if (next != lpfStep) {
        lpfStep = next;
        applyLpfStep(lpfStep);
      }
    }
  }

  // ── 3. Serial commands ─────────────────────────────────────────────────────
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) return;

    // T — table
    if (cmd.equalsIgnoreCase("T")) {
      printLpfTable();
      return;
    }

    // mute
    if (cmd.equalsIgnoreCase("mute")) {
      if (swMuted) {
        Serial.println(F("Already software-muted."));
      } else {
        swMuted = true;
        applyMuteState();
        Serial.println(F("Software mute ON."));
      }
      Serial.flush();
      return;
    }

    // unmute
    if (cmd.equalsIgnoreCase("unmute")) {
      if (!swMuted) {
        Serial.println(F("Already software-unmuted."));
      } else {
        swMuted = false;
        applyMuteState();
        if (hwMuted)
          Serial.println(F("Software mute OFF — knife switch still closed, audio remains muted."));
        else
          Serial.println(F("Software mute OFF — audio enabled."));
      }
      Serial.flush();
      return;
    }

    // Input X — MUX select
    if (cmd.length() >= 7 && cmd.substring(0, 6).equalsIgnoreCase("Input ")) {
      String arg = cmd.substring(6);
      arg.trim();
      if (arg.length() > 0 && arg[0] >= '0' && arg[0] <= '9') {
        long x = arg.toInt();
        if (x >= 0 && x <= 7) {
          applyMuxOutput((uint8_t)x);
          return;
        }
      }
      Serial.println(F("Invalid. Use 'Input X' where X = 0-7. Note: Input actually refers to the output, 0 = speaker, 1 = 3.5mm jack."));
      return;
    }

    // Anything else (including old step numbers) — guide the user
    Serial.println(F("Unknown command. Valid: T | Input X | mute | unmute"));
    Serial.println(F("  Use the rotary encoder to change the LPF step."));
    Serial.flush();
  }
}
