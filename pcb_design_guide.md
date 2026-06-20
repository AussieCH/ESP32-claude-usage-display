# PCB Design Engineering Guide — Claude Usage Dashboard

## ESP32 + SSD1306 OLED Production Board (Altium Designer)

---

## 1. Architecture Overview

The target system is minimal: a WiFi-capable microcontroller fetches JSON over HTTPS, parses it, and renders text to a 128×64 I2C OLED. There is no Bluetooth requirement, no analog sensing, no high-speed peripherals, and no battery. The entire firmware fits comfortably in 4 MB of flash with no PSRAM needed for a production-dedicated build (the current N16R8 module's 16 MB flash and 8 MB PSRAM are dramatically oversized for this workload).

### Block Diagram

```
USB-C (5V)
   │
   ├── VBUS → 3.3V LDO → VCC_3V3 rail
   │              │
   │              ├── ESP32-C3 module (WiFi + MCU)
   │              │       ├── GPIO4 (SDA) ──┐
   │              │       ├── GPIO5 (SCL) ──┤── I2C bus
   │              │       ├── GPIO18 (D-)   │
   │              │       └── GPIO19 (D+)   │
   │              │                         │
   │              └── SSD1306 OLED module ───┘
   │
   ├── D- ── 22Ω ── GPIO18 (USB D-)
   └── D+ ── 22Ω ── GPIO19 (USB D+)
```

The board needs exactly four functional blocks: USB-C power/data input, 3.3V regulation, the ESP32-C3 module, and the OLED connector. Everything else is passives.

---

## 2. MCU Selection — Why ESP32-C3, Not ESP32-S3

### The Case Against ESP32-S3 for This Project

The ESP32-S3-N16R8 is a dual-core 240 MHz processor with 8 MB PSRAM, vector instructions, and 16 MB flash. Your firmware uses a single core, needs roughly 200 KB of RAM (TLS stack + JSON + HTTP buffers), stores under 1 MB of code, and runs no machine learning workloads. The S3 is like driving a semi-truck to buy groceries.

### Why ESP32-C3 Is the Right Fit

The ESP32-C3 is a single-core RISC-V processor at 160 MHz with 400 KB SRAM, WiFi b/g/n, BLE 5.0, and native USB Serial/JTAG — every feature your firmware actually uses, at roughly one-third the module cost.

### Recommended Module: ESP32-C3-MINI-1-N4

| Attribute | ESP32-S3-WROOM-1-N16R8 | ESP32-C3-MINI-1-N4 |
|---|---|---|
| Core | Dual Xtensa LX7 @ 240 MHz | Single RISC-V @ 160 MHz |
| SRAM | 512 KB + 8 MB PSRAM | 400 KB |
| Flash | 16 MB (external) | 4 MB (in-package) |
| WiFi | 802.11 b/g/n | 802.11 b/g/n |
| USB | Native USB OTG | USB Serial/JTAG |
| Module size | 18 × 25.5 mm | 13.2 × 16.6 mm |
| LCSC unit price (qty 10) | ~$3.70 | ~$1.50 |
| Antenna | PCB onboard | PCB onboard |
| GPIOs available | 36 | 15 |

The C3-MINI-1's in-package flash eliminates the external SPI flash chip entirely, meaning fewer traces, fewer decoupling caps, and a smaller layout. The module is physically 55% smaller than the S3 WROOM. Flash is embedded in the SoC die, so there are no SPI flash routing concerns at all.

### Availability Cross-Check

| Distributor | ESP32-C3-MINI-1-N4 | Status |
|---|---|---|
| LCSC | C2838502 | In stock, $1.50–$2.15 |
| DigiKey | 1965-ESP32-C3-MINI-1-N4CT-ND | In stock |
| Mouser | 356-ESP32C3MINI1N4 | In stock |

### Firmware Porting Effort

Your existing codebase already documents the C3 as a supported target — the README includes the `platformio.ini` board change (`esp32-c3-devkitm-1`) and the I2C pin remap. The only real changes for a production C3 build are removing the `-DBOARD_HAS_PSRAM` and `-DARDUINO_USB_CDC_ON_BOOT=1` flags and updating I2C pin defines.

### When to Stay on ESP32-S3

Keep the S3 only if you plan to add features that actually require it: OTG USB host mode, camera interface, PSRAM-hungry applications, or AI/ML acceleration. For a dashboard that fetches JSON and renders text, the C3 is the correct engineering choice.

---

## 3. Schematic Design — Section by Section

### 3.1 USB-C Input (Power + Data)

Use a USB-C 2.0 receptacle (16-pin mid-mount SMD is standard). For USB 2.0 device mode you need the following connections:

**CC1 and CC2 pins:** Each pulled down to GND through a 5.1 kΩ resistor. This tells the host that the device is a USB sink requesting default 5V power. Without these resistors, many USB-C hosts and cables will not deliver power at all.

**VBUS:** Connected to the input of the 3.3V regulator and through a TVS diode (USBLC6-2SC6 or equivalent) for ESD protection.

**D+ and D-:** Routed to GPIO19 (D+) and GPIO18 (D-) on the ESP32-C3 through 22Ω series resistors placed close to the MCU. The USB-C connector has two D+ pads and two D- pads (for cable flip tolerance) — short the two D+ pads together and the two D- pads together on the PCB.

**Shield/GND:** Connect the connector shield to GND through a 1 MΩ resistor in parallel with a 4.7 nF capacitor. This provides a chassis ground path while avoiding ground loops.

**Schematic snippet (key nets):**

```
USB-C VBUS ── Schottky or TVS ── VBUS_5V
USB-C D-  ── 22Ω ── ESP32 GPIO18
USB-C D+  ── 22Ω ── ESP32 GPIO19
USB-C CC1 ── 5.1kΩ ── GND
USB-C CC2 ── 5.1kΩ ── GND
USB-C GND ── GND plane
```

### 3.2 Power Regulation (5V → 3.3V)

For a WiFi MCU that draws 350–500 mA peak during TX bursts, you need an LDO rated for at least 500 mA with low dropout and stable output under transient load.

**Recommended LDO: AMS1117-3.3 or ME6211C33M5G**

The AMS1117-3.3 is the classic choice — SOT-223 package, 1A rated, universally available, and costs under $0.05 at LCSC. It works fine for a USB-powered board where you have a comfortable 1.7V headroom (5V → 3.3V). Its drawback is a relatively high quiescent current (~5 mA) and it requires a tantalum or ceramic output cap for stability.

For a more modern option, the ME6211C33M5G (SOT-23-5, 500 mA, 100 mV dropout, 40 µA quiescent) is smaller, cheaper, and works perfectly with ceramic caps. If you ever add battery power, this becomes the better choice due to its low quiescent current.

**Decoupling on the 3.3V rail:**

| Location | Capacitor | Notes |
|---|---|---|
| LDO input | 10 µF ceramic (X5R, 10V) | Close to VBUS pin |
| LDO output | 22 µF ceramic (X5R, 6.3V) + 100 nF | Close to LDO output pin |
| ESP32-C3 VDD (3V3) | 100 nF + 10 µF | Adjacent to module power pins |
| ESP32-C3 VDD_RTC | 100 nF | Adjacent to RTC power pin |

All ceramic caps should be X5R or X7R dielectric (never Y5V — the capacitance drops drastically under DC bias). Use 0402 for the 100 nF caps and 0603 for the 10 µF and 22 µF caps.

### 3.3 ESP32-C3 Boot and Enable Circuit

**EN (Chip Enable) Pin:** The EN pin must not float. Connect it through an RC delay circuit: a 10 kΩ pull-up resistor to 3V3 and a 1 µF capacitor to GND. This provides a clean power-on reset with approximately 10 ms delay, ensuring the power rail is stable before the chip starts. A tactile reset button (normally open) connects between EN and GND to allow manual reset.

**Strapping Pins (Boot Mode):**

| Pin | Normal Boot (SPI Flash) | Download Mode (USB/UART) |
|---|---|---|
| GPIO2 | High (default internal pull-up) | Don't care |
| GPIO8 | Any (has internal pull-up) | Don't care |
| GPIO9 | High (internal pull-up) | Low |

For normal operation, GPIO9 needs to be high at boot. Add a 10 kΩ pull-up to 3V3 on GPIO9. To enter download mode for flashing, add a tactile "BOOT" button between GPIO9 and GND. The user holds BOOT, presses RESET, releases BOOT — standard ESP32 flash procedure.

GPIO2 has an internal pull-up and defaults high, which selects SPI boot. No external resistor needed unless you have a peripheral that could pull it low during reset.

**Reset + Boot Button Schematic:**

```
3V3 ── 10kΩ ──┬── EN pin
              │
           1µF cap
              │
             GND
              │
         [RESET btn]
              │
             GND

3V3 ── 10kΩ ──┬── GPIO9
              │
         [BOOT btn]
              │
             GND
```

### 3.4 USB Serial/JTAG (Programming Interface)

The ESP32-C3 has a built-in USB Serial/JTAG controller on GPIO18 (D-) and GPIO19 (D+). This means you do not need an external USB-to-UART bridge chip (no CP2102, no CH340). The USB data lines from the connector go straight to the MCU through 22Ω series termination resistors. This saves a $0.30–$0.80 chip and its associated passives.

The USB Serial/JTAG interface provides both serial console output (for `Serial.print`) and firmware flashing (via `esptool.py`), and JTAG debugging — all over one USB-C connection.

**Important:** In your `platformio.ini` for the C3, you do not need `-DARDUINO_USB_CDC_ON_BOOT=1` — the C3's USB Serial/JTAG is the default serial interface when the framework detects it.

### 3.5 I2C Bus (SSD1306 OLED)

The SSD1306 OLED connects over I2C. On the ESP32-C3, any GPIO can serve as I2C SDA/SCL. Choose pins that are not strapping pins and not shared with USB. Good choices: GPIO4 (SDA) and GPIO5 (SCL), or GPIO6/GPIO7.

**Pull-up resistors:** The I2C bus requires pull-ups on both SDA and SCL. Use 4.7 kΩ to 3V3 for standard 100 kHz operation, or 2.2 kΩ if you run at 400 kHz (which works fine for the SSD1306). Place these resistors close to the MCU, not near the OLED connector.

**OLED connector options:**

Option A — **4-pin 0.96" OLED directly soldered:** Use a 1×4 2.54mm pin header footprint. The user solders the OLED module's header pins directly into the PCB. This is the simplest approach and keeps the OLED mechanically fixed.

Option B — **FPC connector for a bare OLED panel:** If you want a more integrated look, use a 0.5mm pitch FPC connector (typically 30-pin for SSD1306 bare panels). This is more complex and only justified if you're designing a custom enclosure.

For a first production prototype, Option A is strongly recommended.

**I2C schematic:**

```
3V3 ── 4.7kΩ ──┬── SDA (GPIO4) ── OLED SDA
               │
3V3 ── 4.7kΩ ──┬── SCL (GPIO5) ── OLED SCL
               │
OLED VCC ── 3V3
OLED GND ── GND
```

### 3.6 Optional: Status LED

Add a single indicator LED on an available GPIO (e.g., GPIO8 — also the conventional LED pin on C3 dev boards). Use a 0402 or 0603 LED with a 1 kΩ series resistor to 3V3. This gives you a visual heartbeat indicator for debugging and is essentially free in BOM cost.

### 3.7 Complete BOM Summary

| Ref | Component | Package | Qty | LCSC Example | Approx Cost |
|---|---|---|---|---|---|
| U1 | ESP32-C3-MINI-1-N4 | Module | 1 | C2838502 | $1.50 |
| U2 | AMS1117-3.3 | SOT-223 | 1 | C6186 | $0.05 |
| J1 | USB-C 16P SMD | Mid-mount | 1 | C168688 or similar | $0.10 |
| J2 | 1×4 Pin Header 2.54mm | Through-hole | 1 | C49661 | $0.02 |
| D1 | USBLC6-2SC6 (ESD) | SOT-23-6 | 1 | C7519 | $0.15 |
| D2 | Status LED (green) | 0603 | 1 | C72043 | $0.01 |
| R1, R2 | 5.1 kΩ (CC pull-downs) | 0402 | 2 | C25905 | $0.01 |
| R3, R4 | 22Ω (USB series) | 0402 | 2 | C25092 | $0.01 |
| R5, R6 | 4.7 kΩ (I2C pull-ups) | 0402 | 2 | C25900 | $0.01 |
| R7 | 10 kΩ (EN pull-up) | 0402 | 1 | C25744 | $0.01 |
| R8 | 10 kΩ (GPIO9 pull-up) | 0402 | 1 | C25744 | $0.01 |
| R9 | 1 kΩ (LED series) | 0402 | 1 | C11702 | $0.01 |
| C1 | 10 µF (LDO input) | 0603, X5R | 1 | C19702 | $0.01 |
| C2 | 22 µF (LDO output) | 0805, X5R | 1 | C45783 | $0.02 |
| C3 | 100 nF (LDO output) | 0402, X7R | 1 | C1525 | $0.01 |
| C4, C5 | 100 nF (ESP VDD decoupling) | 0402, X7R | 2 | C1525 | $0.01 |
| C6 | 10 µF (ESP VDD bulk) | 0603, X5R | 1 | C19702 | $0.01 |
| C7 | 1 µF (EN RC delay) | 0402, X5R | 1 | C52923 | $0.01 |
| SW1 | Reset button | 3×6mm SMD | 1 | C318884 | $0.02 |
| SW2 | Boot button | 3×6mm SMD | 1 | C318884 | $0.02 |
| **Total** | | | **~22 components** | | **~$2.00** |

This is a 22-component BOM. Compare that to a typical ESP32-S3 design with external flash, PSRAM, crystal, and USB bridge — those run 40–60 components.

---

## 4. PCB Layout Strategy — Step by Step

### 4.1 Board Dimensions and Layer Stack

**Recommended: 2-layer board, 1.6 mm thickness, FR-4.**

For this design, a 2-layer board is sufficient. The current density is low (500 mA max), signal speeds are modest (I2C at 400 kHz, USB Full Speed at 12 Mbps), and the only RF element is the module's onboard PCB antenna (which doesn't touch your PCB traces at all since it's self-contained in the module).

A 4-layer board would only be justified if you were routing bare-chip ESP32 with external flash, antenna matching, and high-density connectors. You're not — you're using a pre-certified module with an integrated antenna. Save the $5–$10 per board.

**Target board size:** approximately 35 × 25 mm (smaller than a credit card). This is achievable because the ESP32-C3-MINI-1 module is only 13.2 × 16.6 mm.

**Stack-up for 2-layer:**

| Layer | Usage |
|---|---|
| Top (F.Cu) | Signal routing, component pads, local ground fills |
| Bottom (B.Cu) | Continuous ground plane (as unbroken as possible) |

The bottom layer should be a near-solid ground pour. Route all signals on the top layer. Use the bottom layer only for short jumper traces where absolutely necessary, and fill everything else with ground copper.

### 4.2 Component Placement Strategy

Placement is the most important step. A good placement makes routing trivial; a bad placement makes it impossible. Follow this order:

**Step 1 — Place the ESP32-C3 module first.** Orient it so the antenna end overhangs the edge of the PCB (or extends to the board edge). The antenna must protrude beyond the board outline or sit at the very edge with no copper, ground plane, or components underneath or within 15 mm on all sides. This is the single most important layout decision for WiFi performance.

**Step 2 — Place the USB-C connector on the opposite edge** from the antenna. This puts the USB connector, ESD protection, and LDO at the "bottom" of the board, the digital section in the middle, and the antenna at the "top." USB and UART signals generate harmonics that can interfere with the 2.4 GHz antenna — physical distance is your best shield.

**Step 3 — Place the LDO and its capacitors** adjacent to the USB connector, forming a compact power block. Input cap on the VBUS side, output caps on the 3V3 side, all within 3–5 mm of the LDO.

**Step 4 — Place ESP32 decoupling capacitors** as close to the module's power pins as physically possible. The 100 nF caps should be within 2 mm of the VDD pin. The 10 µF bulk cap can be slightly further (within 5 mm).

**Step 5 — Place the OLED header** along one edge of the board, wherever it's most convenient for your enclosure design. Keep the I2C traces away from the antenna zone.

**Step 6 — Place buttons and LED** wherever accessible. Reset and Boot buttons should be reachable by the user.

### 4.3 Placement Diagram (Conceptual Top View)

```
┌─────────────────────────────────┐
│  ╔══════════════╗               │
│  ║  ANTENNA     ║  ← Keep-out  │
│  ║  (no copper  ║    zone      │
│  ║   under here)║               │
│  ╠══════════════╣               │
│  ║              ║               │
│  ║  ESP32-C3    ║  [C4][C5][C6]│
│  ║  MINI-1      ║               │
│  ║              ║               │
│  ╚══════════════╝               │
│                                 │
│  [R5][R6]  [OLED HEADER J2]    │
│   I2C       SDA SCL VCC GND    │
│   pull-ups                      │
│                                 │
│  [R7] [C7]    [SW1]  [SW2]    │
│   EN circuit   RST    BOOT     │
│                                 │
│  [D2][R9]  [D1]  [U2][C1][C2] │
│   LED       ESD   LDO + caps   │
│                                 │
│  ═══════════════════════════    │
│  [    USB-C Connector J1    ]   │
└─────────────────────────────────┘
```

### 4.4 Ground Plane Design

The bottom copper layer should be a continuous, unbroken ground pour. This is the single most effective thing you can do for signal integrity, power stability, and EMI performance. Rules:

**Do not split the ground plane.** There is no reason to have separate analog and digital grounds on this board. The ESP32-C3 module has a single ground reference internally, and splitting grounds creates more problems than it solves at this complexity level.

**Use stitching vias liberally.** Place ground vias every 3–5 mm around the perimeter of the board and around any signal traces on the top layer. Each component GND pad should have at least one via connecting directly to the bottom ground plane. Use 0.3 mm drill, 0.6 mm pad vias — standard for JLCPCB.

**Critical ground via locations:** Each ESP32 module GND pad (there are 13+ ground pads on the MINI-1), each decoupling capacitor GND pad, the LDO GND pad, and the USB connector GND pins.

**Thermal relief vs. direct connect:** Use direct connections (no thermal relief) for all ground pads on decoupling capacitors and the LDO thermal pad. Use thermal relief on through-hole connector pads to make soldering easier.

### 4.5 Trace Width Guidelines

| Net Type | Recommended Width | Rationale |
|---|---|---|
| Power (VBUS, 3V3) | 0.5–1.0 mm (20–40 mil) | Carries up to 500 mA; wider = lower resistance, better thermal |
| GND (top layer pours) | Fill/pour | Maximize copper coverage |
| USB D+/D- | 0.3 mm (12 mil) on 1.6mm FR-4 | Targets ~90Ω differential impedance (USB spec) |
| I2C (SDA, SCL) | 0.2–0.25 mm (8–10 mil) | Low speed, minimal concern; keep reasonably short |
| GPIO signals | 0.2 mm (8 mil) | Standard digital signals |
| EN, strapping | 0.15–0.2 mm (6–8 mil) | Minimal current, DC only |

For JLCPCB standard process, minimum trace width is 0.127 mm (5 mil) and minimum spacing is 0.127 mm. Stay above 0.15 mm (6 mil) for manufacturing margin.

### 4.6 USB Routing

The USB D+ and D- lines should be routed as a differential pair from the USB-C connector to the ESP32 GPIO18/GPIO19 pins. On a 2-layer 1.6mm FR-4 board:

For 90Ω differential impedance, use approximately 0.3 mm trace width with 0.15 mm gap between the pair (verify with your fab's impedance calculator — JLCPCB provides one). Keep the pair matched in length to within 0.5 mm. Route them on the top layer with continuous ground underneath — no ground plane breaks under the USB traces. Place the 22Ω series resistors as close to the ESP32 pins as possible, not near the connector.

At USB Full Speed (12 Mbps), impedance control is helpful but not as critical as at USB High Speed (480 Mbps). A reasonable-effort differential pair will work fine.

### 4.7 I2C Routing

I2C at 100–400 kHz is extremely forgiving. Keep traces under 100 mm total length (yours will be under 30 mm on a board this size). Route SDA and SCL with at least 0.25 mm (10 mil) spacing between them to avoid crosstalk — though at these frequencies, crosstalk is not a real concern.

Place the 4.7 kΩ pull-up resistors close to the ESP32, not near the OLED connector. This gives the cleanest edge transitions on the bus.

If the OLED module will be mounted directly above the PCB (stackup style), keep the I2C trace run as short as possible and add a ground via near each I2C pad on the connector.

### 4.8 Antenna Keep-Out Zone

This is where most first-time ESP32 PCB designs fail. The ESP32-C3-MINI-1 has a PCB antenna at one end of the module (the end without pins). Espressif's hardware design guidelines specify:

**Mandatory:** No copper (traces, pours, or fills) on any PCB layer within 15 mm of the antenna area in all directions. This means no ground plane under the antenna overhang, no signal traces, no component pads. Ideally, the antenna extends beyond the board edge entirely, so there is literally nothing underneath it.

**Recommended:** Cut out the PCB substrate under the antenna if the module cannot overhang the edge. This means the antenna portion of the module hangs over open air, which gives the best RF performance.

**Prohibition:** Never place USB traces, UART traces, the LDO, switching regulators, crystals, or LEDs near the antenna. All of these generate harmonics in or near the 2.4 GHz band.

In Altium, create a keepout region (on all copper layers) matching the antenna zone. This prevents the autorouter and copper pours from placing anything there.

### 4.9 Thermal Considerations

The AMS1117-3.3 in SOT-223 dissipates (5V − 3.3V) × 0.5A = 0.85W at peak WiFi TX current. The SOT-223 package has a thermal pad (the large tab) that should connect to a copper pour on the top layer. Extend this pour to at least 100 mm² of exposed copper, with several vias connecting to the bottom ground plane for heat spreading.

If you use the ME6211 in SOT-23-5 instead, thermal concerns are much smaller — it drops only 0.1V × 0.5A = 0.05W at peak load due to its low dropout voltage, plus the WiFi TX burst duration is short (milliseconds).

---

## 5. WiFi Performance and Signal Integrity

### 5.1 Antenna Placement is Everything

On a module-based design, you've already let Espressif handle the hard RF work — the antenna matching network, the 50Ω trace to the LNA, and the antenna geometry are all inside the module. Your job is to not ruin it.

The two most common ways to kill WiFi performance on a module-based board are placing copper under the antenna and enclosing the antenna in a metal or metallized enclosure. Follow the keep-out rules described above and ensure your enclosure (if any) uses plastic or has an opening near the antenna.

### 5.2 Power Supply Noise Filtering

WiFi TX power output is directly affected by power rail noise. During a WiFi TX burst, the ESP32-C3 draws current spikes of 300–350 mA that last microseconds. If your decoupling is inadequate, the 3V3 rail droops, and the TX power drops with it — reducing range and throughput.

The defense is bulk capacitance close to the module (the 10 µF ceramic) combined with high-frequency bypass (the 100 nF ceramics). The 22 µF on the LDO output handles the low-frequency transients, and the 100 nF caps handle the high-frequency edges.

If you observe WiFi range problems on your first prototype, the first thing to try is adding another 10 µF ceramic directly at the module's VDD pin.

### 5.3 I2C Stability

The SSD1306 is a well-behaved I2C device. The most common stability issue is noise coupling onto long I2C wires. On a PCB with 20 mm traces (not 200 mm jumper wires), this is essentially a non-issue. If you do experience I2C glitches, reduce the pull-up resistors from 4.7 kΩ to 2.2 kΩ (faster rise times = more noise margin) and ensure you have a solid ground return path under the I2C traces.

### 5.4 Boot Reliability

The most common boot failure mode on ESP32 designs is the EN pin rising too fast — the chip starts before the power rail is stable, and it enters an undefined state. The RC delay circuit (10 kΩ + 1 µF) on EN prevents this. If you're using a slow-ramping LDO, you may need to increase the cap to 10 µF for an even slower EN rise.

The second most common issue is strapping pin contention — a peripheral or pull-down on GPIO9 preventing normal boot. Keep GPIO9 clean with just the 10 kΩ pull-up and the BOOT button.

---

## 6. Manufacturing (DFM) Guidelines

### 6.1 PCB Specifications for JLCPCB / PCBWay

| Parameter | Recommended Value |
|---|---|
| Layers | 2 |
| Thickness | 1.6 mm |
| Copper weight | 1 oz (35 µm) |
| Surface finish | HASL (lead-free) or ENIG |
| Solder mask | Green (cheapest, fastest) |
| Silkscreen | White |
| Min trace/space | 6/6 mil (0.15/0.15 mm) — standard process |
| Min via drill | 0.3 mm |
| Min via pad | 0.6 mm |
| Board outline tolerance | ±0.1 mm |

HASL is the cheapest surface finish and works fine for this design. Use ENIG only if you need the flat pad surface for QFN components (not applicable here — the C3 MINI-1 module has castellated pads).

### 6.2 Assembly Approach

**Use JLCPCB's SMT assembly service.** All components in the BOM above are available in JLCPCB's parts library (they source from LCSC). The only through-hole component is the OLED pin header (J2), which can be hand-soldered after the SMD assembly is done.

For the ESP32-C3-MINI-1 module, JLCPCB can place it as a "basic" or "extended" part. The module has a flat bottom with castellated edge pads and a large ground pad — it assembles reliably with standard reflow processes. Ensure your footprint includes a paste aperture for the center ground pad (reduced to about 50% coverage to avoid tombstoning from excess solder).

### 6.3 BOM Optimization

Keep all passive components in the same package size where possible. This BOM uses 0402 for all resistors and small capacitors, and 0603 for bulk capacitors. Using consistent sizes reduces the number of pick-and-place nozzle changes and can lower assembly cost.

Minimize the number of unique part values. For example, use 10 kΩ for both the EN pull-up and the GPIO9 pull-up instead of different values — even if the exact value doesn't matter, using the same value reduces unique line items.

### 6.4 Panelization

For small boards (35 × 25 mm), panelization is recommended for cost-efficient manufacturing. JLCPCB supports V-score and tab-routed panels.

For a rectangular board this size, V-score panelization is simplest — arrange boards in a 2×5 or 3×4 grid within a 100 × 100 mm panel (JLCPCB's standard panel size). V-score lines on top and bottom allow easy snap-apart depaneling. Leave 1 mm between boards for the V-score kerf.

Add 5 mm tooling rails on two opposite edges of the panel with three fiducial marks (1 mm circles with 2 mm clearance) for the pick-and-place machine.

### 6.5 Design Rule Check (DRC) Checklist

Before generating Gerbers from Altium:

1. All copper cleared from antenna keep-out zone (both layers)
2. No unconnected nets
3. All decoupling caps within 3 mm of their associated power pins
4. Ground vias present at every GND pad
5. Bottom layer ground pour is continuous with no isolated islands
6. USB D+/D- length matched within 0.5 mm
7. Silkscreen does not overlap pads
8. All component courtyard clearances respected (0.25 mm minimum between components)
9. Mounting holes (if used) have adequate clearance from copper
10. Board outline is a closed polygon with radiused corners (0.5 mm radius minimum for handling)

---

## 7. Altium Designer Workflow

### 7.1 Project Setup

Create a new PCB Project in Altium. Add a schematic document and a PCB document. Set your design rules early:

**Clearance rules:** 0.15 mm minimum for all nets, 0.25 mm for USB D+/D- to other nets.

**Width rules:** Create net classes — "Power" (0.5 mm default), "USB" (0.3 mm), "Signal" (0.2 mm).

**Via rules:** 0.3 mm drill, 0.6 mm pad, 0.2 mm annular ring.

### 7.2 Library Management

For the ESP32-C3-MINI-1 module, download Espressif's official Altium footprint from their GitHub repository or use the footprint from SnapEDA/Ultra Librarian. Verify the pad dimensions against the datasheet (the module's recommended land pattern is in the datasheet appendix). Pay special attention to the center ground pad dimensions and paste mask aperture.

For the USB-C connector, use the exact footprint from the manufacturer's datasheet. Mid-mount USB-C connectors have precise mechanical tolerances — a generic footprint will cause assembly issues.

### 7.3 Schematic Capture

Draw the schematic in functional blocks, using net labels for inter-block connections. Suggested sheet layout:

- Power section (USB-C, ESD, LDO, decoupling) in the top-left
- ESP32-C3 module with strapping pins and EN circuit in the center
- I2C / OLED connector in the top-right
- Buttons and LED at the bottom

Run ERC (Electrical Rules Check) before moving to PCB layout. Fix all errors; warnings about unconnected module NC pins are expected.

### 7.4 PCB Layout Process

1. Import netlist from schematic (Design → Import Changes)
2. Define board outline (35 × 25 mm rectangle with 0.5 mm corner radius)
3. Place components per the strategy in Section 4.2
4. Define keepout regions for the antenna zone
5. Pour bottom layer ground plane
6. Route USB differential pair first (highest constraint)
7. Route power traces (VBUS, 3V3)
8. Route I2C and remaining signals
9. Pour top layer ground fills in remaining space
10. Add stitching vias around board perimeter and between ground fills
11. Add silkscreen labels (component references, board name, version, pin 1 indicators)
12. Run DRC — fix all violations
13. Review 3D view for mechanical clearances

### 7.5 Gerber Generation

Use Altium's Output Job File or manual Gerber export. Generate:

| File | Altium Layer |
|---|---|
| Top Copper | GTL |
| Bottom Copper | GBL |
| Top Solder Mask | GTS |
| Bottom Solder Mask | GBS |
| Top Silkscreen | GTO |
| Bottom Silkscreen | GBO |
| Top Paste | GTP |
| Board Outline | GKO (mechanical layer) |
| Drill File | Excellon format (.DRL or .XLN) |

Export Gerbers at 2:5 format (2 integer, 5 decimal digits) in imperial units — this is the JLCPCB default. Generate a drill file in Excellon format with plated and non-plated holes in separate files.

For JLCPCB assembly, also generate:

- BOM in CSV format (LCSC part number, designator, value, package)
- Pick-and-place / CPL file (designator, X, Y, rotation, layer)

---

## 8. Manufacturing-Ready Checklist

Before submitting to the fab:

- [ ] Gerbers reviewed in a Gerber viewer (e.g., Altium's CAMtastic or the free online viewer at JLCPCB)
- [ ] Board outline is correct dimension and closed
- [ ] Drill file has correct hole count
- [ ] Antenna keep-out zone is clear on all copper layers
- [ ] Ground plane has no isolated copper islands
- [ ] All component footprints verified against datasheets
- [ ] BOM matches schematic — every placed component has a part number
- [ ] CPL file coordinates and rotations verified (JLCPCB's online review tool shows this)
- [ ] Fiducials present if using SMT assembly
- [ ] Version number and date on silkscreen
- [ ] Test points accessible (at minimum: 3V3, GND, SDA, SCL, EN, TX, RX)

---

## 9. Optional Enhancements

These are not needed for the first prototype but are natural next steps:

### 9.1 Battery Power

Add a TP4056 Li-Po charging IC (or BQ24075 for more control) between USB VBUS and the LDO input. Include a JST-SH 2-pin battery connector, a battery voltage divider on an ADC-capable GPIO for monitoring, and a load-sharing MOSFET circuit. This adds roughly 8 components and $0.50 to the BOM.

### 9.2 Bluetooth

The ESP32-C3 already has BLE 5.0 built in. No hardware changes needed — it's a firmware addition. Useful if you want to configure the device via a phone app instead of the web portal.

### 9.3 Deep Sleep Power Optimization

If battery-powered, replace the AMS1117 with a low-Iq LDO (ME6211 or HT7333 at 2–4 µA quiescent) and add a MOSFET power gate for the OLED (the SSD1306 draws 10–20 mA even when "off" in sleep mode). With the C3 in deep sleep and the OLED powered down, you can get total board current under 10 µA.

### 9.4 Larger or Color Display

The same I2C bus can drive an SH1106 1.3" OLED (128×64, same protocol) for a larger display, or you could switch to an SPI-connected ST7789 color TFT. The SPI option would require 4–5 additional GPIO connections but gives you color and larger sizes (1.14" to 2.0").

### 9.5 Enclosure

Design a simple 3D-printed enclosure with an opening for the OLED display and the USB-C port. Add M2 mounting holes at two corners of the PCB (2.2 mm drill, 4 mm pad) to secure the board with screws. Keep the antenna end of the enclosure open or use thin, non-metallized plastic.

---

## 10. Common Mistakes to Avoid

**Ground plane under the antenna.** This is the number one WiFi performance killer. Even a thin trace running under the antenna area can detune it by several dB.

**Missing CC resistors on USB-C.** Without the 5.1 kΩ pull-downs on CC1 and CC2, many USB-C hosts will not deliver power. Your board will appear dead when plugged in.

**EN pin floating.** Without the RC circuit, the chip may boot erratically or not at all, depending on the power supply's ramp characteristics.

**Wrong strapping pin state.** If GPIO9 is pulled low at boot (by a connected peripheral, a debug probe, or a missing pull-up), the chip enters download mode instead of running your firmware. Users will see a "blank" device.

**Inadequate decoupling.** A single 100 nF cap on a WiFi MCU is not enough. You need both bulk (10 µF+) and bypass (100 nF) capacitance at every power pin.

**Using the AMS1117 with a battery.** The AMS1117 has a 1.0V dropout and 5 mA quiescent current. On a 3.7V Li-Po (which drops to 3.0V at discharge), the output will be below 3.0V — the ESP32-C3 minimum operating voltage is 3.0V. It will brownout. Use a low-dropout LDO if you add battery power.

**Ignoring the module's ground pad.** The ESP32-C3-MINI-1 has a large exposed pad on the bottom. It must be soldered to the PCB ground plane with adequate paste coverage and thermal vias. Without this connection, the module's thermal and electrical performance suffers.

---

## 11. Key Reference Documents

Bookmark these — you will reference them repeatedly during layout:

1. **ESP32-C3-MINI-1 Datasheet** — Pin definitions, dimensions, antenna keep-out diagram, recommended schematics, strapping pin table
2. **ESP32-C3 Hardware Design Guidelines** (Espressif) — Schematic checklist, PCB layout rules, RF tuning, power supply design
3. **ESP32-C3 Series Datasheet** — Electrical characteristics, absolute maximum ratings, power consumption modes
4. **SSD1306 Datasheet** — I2C address, timing, power consumption
5. **AMS1117 Datasheet** — Input/output cap requirements, thermal derating
6. **USB Type-C Specification (Chapter 4)** — CC resistor requirements for sink devices
7. **JLCPCB Capabilities** — Minimum trace/space, via sizes, assembly constraints

All Espressif documents are available at `docs.espressif.com`. Module datasheets and footprints are on Espressif's GitHub at `github.com/espressif`.
