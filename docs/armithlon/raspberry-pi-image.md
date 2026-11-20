# QRX ARMithlon für den Raspberry Pi 4: das fertige SD-Karten-Image

Stand 2026-11-20. Ein Image, das man auf eine SD-Karte schreibt. Der Pi 4 startet damit direkt
in QRX OS One, wie früher Amithlon: Linux ist nur der Unterbau und nie zu sehen.

## Was geprüft ist und was nicht

**Auf echter Pi-Hardware lief das Image noch nicht.** Geprüft ist, was ohne Pi geht:

| Prüfung | Ergebnis |
| --- | --- |
| Kernel 6.6.78 (`rpi-6.6.y`) gebaut, jede Option aus `qrx-pi4.config` im fertigen `.config` nachgeprüft | ok (`config check: ok`) |
| Jedes Programm des Images startet mit den Bibliotheken des Images (`qemu-aarch64 -L rootfs`): BusyBox, wpa_supplicant, wpa_cli, iw, dbus-daemon, bluetoothd, bluetoothctl, resize2fs | ok |
| Die Engine startet mit den Bibliotheken des Images, JIT an, AROS-ROM läuft an | ok (unter qemu-user zu langsam zum Durchbooten: 0,3 MIPS) |
| Das QRX-OS-One-Paket des Images bootet bis zum Desktop | ok, mit der x86-64-Variante derselben Engine-Quellen (Bildschirmfoto unten) |
| Ton: neuer AROS-Build des AHI-Treibers `qrxhost.audio`, `QRXAHITest` im Gast | `[AHITEST] PLAYED`, 48000 Frames kamen beim Host an |
| WLAN/Bluetooth-Einstellungen: Gast-App → Door-Dienst 11 → Host-Programm | ok, mit einem Test-`netctl`, das feste Antworten gibt; Mausklicks und Tastatur aus dem Gast kamen als `wifi-connect HomeNet geheim123` und `bt-pair AA:BB:CC:DD:EE:02` an |
| Engine-Release-Build `-mcpu=cortex-a72`: alle Tests | 56 direkt + 20/20 Python-Tests über die qemu-Wrapper |

Nicht geprüft, weil es nur auf dem Pi geht: Bild über KMS/HDMI, 3D über Mesa V3D, Ton über
HDMI und Klinke, WLAN- und Bluetooth-Firmware, Ethernet, Tastatur/Maus über USB, die
Vergrößerung der Partition beim ersten Start, und jede Geschwindigkeitsangabe.

## Aufbau

Keine Distribution, sondern ein eigener Kernel und genau die Programme, die das Gerät braucht.

- **Kernel** (`tools/armithlon/pi/kernel/`)
  - Raspberry-Pi-Linux `rpi-6.6.y`, `bcm2711_defconfig` plus `qrx-pi4.config`.
  - Alle nötigen Treiber fest eingebaut, keine Module, keine Initramfs:
    - Bild und 3D: `DRM_VC4`, `DRM_V3D`;
    - Ton: HDMI (`vc4-hdmi`) und Klinke (`SND_BCM2835`), USB-Audio;
    - Ethernet: `BCMGENET`;
    - WLAN: `BRCMFMAC` (SDIO);
    - Bluetooth: `HCIUART_BCM` über serdev, `BT_HIDP`, `UHID`;
    - USB (xHCI über PCIe), HID mit den gängigen Logitech-, Apple-, Microsoft-, Sony- und
      Xbox-Treibern, Touchscreens;
    - ext4, FAT.
  - Die WLAN- und Bluetooth-Firmware und die Funk-Regulierungsdatenbank (signiert) stecken im
    Kernel (`EXTRA_FIRMWARE`).
  - Leistung: Governor `performance`, 1000 Hz, `NO_HZ_FULL`, `RCU_NOCB_CPU`, transparente
    Huge Pages, Debug-Optionen aus.
  - `build-kernel.sh` prüft am Ende jede Option der Datei im fertigen `.config`.
- **Benutzerland** (`tools/armithlon/pi/image/`)
  - Rohstoff sind Ubuntu-24.04-arm64-Pakete (`fetch-debs.sh`, `packages.txt`; `exclude.txt`
    hält systemd, Paketverwaltung, Perl und Python fern).
  - `mkrootfs.py` übernimmt daraus nur die Programme der Liste und die Bibliotheken, die sie
    wirklich laden (ELF `DT_NEEDED`, rekursiv), dazu die per `dlopen` geladenen von SDL2 und Mesa.
  - BusyBox ist Init, Shell, DHCP-Client und Werkzeugkasten.
  - X11-, Wayland- und PulseAudio-*Bibliotheken* sind dabei, weil Ubuntus SDL2 und Mesa sie
    fest einbinden. Kein X-Server, Compositor oder Sound-Server läuft je.
  - Größe: etwa 375 MB, davon 133 MB `libLLVM` und 30 MB ICU, die Ubuntus Mesa mitbringt.
    Ein eigenes Mesa nur mit V3D/VC4 wäre deutlich kleiner (offen).
- **QRX OS One** (`tools/armithlon/pi/bundle/build-bundle.sh`)
  - Grundlage ist das Deadwood-AROS-Release 5b5fd4c aus dem Repository. ROM, Ext-ROM, ISO und
    Bootdiskette werden nur mit ihren gespeicherten SHA256-Summen verwendet.
  - `SystemFull` = der ISO-Baum + `Startup-Sequence-FullISO` + QRX-Desktop, QRX-Apps,
    Host-Eingabe, Host-Speicher, AHI-Treiber (Standard-Tonmodus), `QRXNetwork`, `QRXInfo`,
    `QRXLoadDevice` und die neue App `QRXWireless`.
  - `boot.adf` = Bootdiskette des Releases + `L:QRXHostFS`, `L:QRXAppsFS`, `C:QRXHostMemory`,
    `DEVS:Mountlist` und `Startup-HostFS` als `S:Startup-Sequence`.
  - **ROM:** `patch-boot-rom.py` passt nicht zum ROM des Releases (die Bytes an `0x628e2`
    stehen dort nicht). Das Paket nimmt das Release-ROM deshalb unverändert und meldet das.
    Der Desktop bootet damit. Welches ROM das Mac-Paket nutzt, steht nicht im Repository.
  - Hintergrundbilder und Design-Icons erzeugt auf dem Mac `gen-assets.swift`; unter Linux
    fehlen sie, der Desktop nimmt dann sein eingebautes Bild.
- **Image** (`tools/armithlon/pi/image/mkimage.sh`)
  - Partition 1, FAT32 „QRXBOOT“, 256 MiB: `start4.elf`, `fixup4.dat`, `kernel8.img`,
    `bcm2711-rpi-4-b.dtb`, Overlays, `config.txt`, `cmdline.txt`.
  - Partition 2, ext4 „qrxroot“: das Benutzerland. `qrx-grow` vergrößert sie beim ersten Start
    auf die ganze Karte (einmal Neustart, dann `resize2fs` im laufenden Betrieb).

## Start und Leistung

- BusyBox-Init → `rcS`: Dateisysteme, Governor `performance`, Ethernet per DHCP,
  `wpa_supplicant` für WLAN, D-Bus und `bluetoothd`.
- Danach `/qrx/session`: die Engine im Vollbild über SDL2/KMSDRM, Ton über ALSA.
  - HDMI-Ton, wenn der Bildschirm welchen hat, sonst die Klinke.
  - `/data/qrx-audio` mit einem ALSA-Kartennamen (`Headphones`, `vc4hdmi0`) legt das fest.
- **Kern 3 gehört der 68k-CPU:** `isolcpus=3 nohz_full=3 rcu_nocbs=3`, die Engine setzt ihren
  CPU-Thread per `QRX_CPU_CORE=3` dorthin, alles andere läuft auf Kern 0–2.
- `arm_boost=1` (1,8 GHz auf neueren Pi 4). Übertakten steht auskommentiert in `config.txt`.
- QRX OS One herunterfahren schaltet den Pi aus; stürzt die Engine ab, startet Init sie neu.
- Auf tty2 (Strg+Alt+F2) wartet eine Root-Shell für Notfälle. Wer physisch an den Pi kommt,
  hat damit Root.

## WLAN und Bluetooth aus QRX OS One

- Im Home-Menü des Desktops: **„Wi-Fi & BT“** (`C:QRXWireless`).
  - WLAN: Netze suchen (Signal, gesichert/offen), mit Passwort verbinden, vergessen.
    Verbundene Netze speichert `wpa_supplicant` für den nächsten Start.
  - Bluetooth: an/aus, Geräte suchen, koppeln (inkl. Vertrauen und Verbinden), entfernen.
  - Unten: Status von LAN, WLAN und Bluetooth.
- Technik:
  - Neuer Door-Dienst 11 der Engine (`full68k/qrx_hostctl.c`): der Gast übergibt Befehl und
    Argumente als getrennte Zeichenketten.
  - Die Engine startet damit `$QRX_NETCTL` direkt per `execv`, ohne Shell; Gast-Text wird nie
    Shell-Syntax. Zeitlimit 8 s, Ausgabe zurück in den Gastpuffer.
  - `/qrx/netctl` prüft SSID, Passwort und MAC-Adressen und spricht mit `wpa_cli` und
    `bluetoothctl`.
  - Auf dem Mac gibt es kein `QRX_NETCTL`; die App sagt dann, dass WLAN und Bluetooth
    Sache des Host-Systems sind.

## Bauen

Auf x86-64-Ubuntu 24.04 mit `gcc-aarch64-linux-gnu`, `qemu-user`, arm64-Multiarch,
`xorriso`, `xdftool` (amitools), `dosfstools`, `mtools`, `e2fsprogs`, `fdisk` und der
Deadwood-m68k-Toolchain:

```sh
bash tools/armithlon/pi/kernel/build-kernel.sh LINUX-SRC FIRMWARE-DIR KERNEL-OUT
bash tools/armithlon/pi/image/fetch-debs.sh DEBS          # danach alle .deb nach STAGE entpacken
bash tools/armithlon/pi/bundle/build-bundle.sh BUNDLE
bash tools/armithlon/pi/build-pi.sh --cross               # Engine; fürs Image Release + -mcpu=cortex-a72
python3 tools/armithlon/pi/image/mkrootfs.py STAGE ENGINE BUNDLE ROOTFS
bash tools/armithlon/pi/image/mkimage.sh ROOTFS KERNEL-OUT STAGE/usr/lib/linux-firmware-raspi qrx-armithlon-pi4.img
```

Auf die Karte: z. B. mit dem Raspberry Pi Imager („eigenes Image“) oder `dd`.

## Bekannte Grenzen

- Nur **Pi 4 Model B** (Gerätebaum `bcm2711-rpi-4-b.dtb`). Pi 400, CM4 und Pi 5 brauchen eigene
  Gerätebäume bzw. für den Pi 5 einen anderen Kernel (`bcm2712`).
- **Tastatur und Maus vor dem Start einstecken.** Ohne udev-Dienst sieht SDL2 die Geräte beim
  Start (es liest ihre Fähigkeiten aus sysfs), später eingesteckte aber nicht. Bluetooth-Geräte
  nach dem Koppeln: QRX OS One einmal neu starten.
- Das Passwortfeld der WLAN-App zeigt das Passwort im Klartext (GadTools hat kein
  Passwortfeld).
- Das WLAN-Land ist `DE` (`/etc/qrx/wpa_supplicant.conf`); in der App noch nicht wählbar.
- QRX OS One 64 (68100EC64) hat unter Linux noch kein Fenster; das Image startet die
  32-Bit-Maschine.
- Ein eigenes, schlankes Mesa (nur V3D/VC4, ohne LLVM) und ein SDL2 nur mit KMSDRM/ALSA würden
  das Image um gut 200 MB verkleinern.
