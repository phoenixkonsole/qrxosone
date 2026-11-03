# ARMITHLON 2.6 – QRX68 Exec-Signale, Intuition-Gadgets, Menüs und Fensterkomposition

Neu: [README-2.6-DE.md](README-2.5-DE.md) mit Exec-Signalen, Port-/Gadget-/Menü-Vektoren, Fensterkomposition, ABI-Tests und den noch offenen AROS-Gasttests.

# ARMITHLON 2.4

**NEU: QRXDesktop/QRX68-Doppelklick-Bridge (AROS-Gast-Quellcode, Hostservice, testsicherer Transport).**

Anleitung und Einschränkungen: [README-2.4-DE.md](README-2.4-DE.md).

# ARMITHLON 2.3 — QRX68 Musashi/GUI-HLE

**Neu:** [README-2.3-DE.md](README-2.3-DE.md), [CHANGELOG-2.3.md](CHANGELOG-2.3.md). Bitte die Validierungsgrenzen im neuen README beachten.

# ARMITHLON 2.2 — QRX68 / AmigaOS 3.x Compatibility Runtime

**Jetzt ansehen:** [README-2.2-DE.md](README-2.2-DE.md) und [CHANGELOG-2.2.md](CHANGELOG-2.2.md).
Die neue QRX68-Laufzeit führt erste echte Amiga-HUNK-Binärprogramme mit Exec-/DOS-LVO-HLE aus. **Keine vollständige AmigaOS- oder AROS-Kompatibilität behauptet.**

# ARMITHLON 2.1 – RAM und Amiga-Kompatibilität

**Neue Änderungen:** [README-2.1-DE.md](README-2.1-DE.md). Der klassische QRX68100-68k-Firmware-Boot bleibt benutzbar. Die eingebundene AmigaOS3.x-Kickstart/Workbench-Umgebung ist ein externer FS-UAE-Kompatibilitätsmodus, nicht der native QRX68100-Boot. Die originale QRXDesktop-Gast-Workbench ist noch nicht nativ eingebunden.

# ARMITHLON 2.0 / QRX OS One

**Aktuell: [README-2.0-DE.md](README-2.0-DE.md)** – realer Host-Folder-Mount statt nur Disk-IMG, optionale RAM-only-Firmware, QRX-SMP-/AROS-BSP-Treiberbausteine. Native AROS-Desktop-Boot weiterhin nicht nachgewiesen.

# ARMITHLON 1.9 – neues Integrationspaket

Aktueller Teilstand: siehe **[README-1.9-DE.md](README-1.9-DE.md)** und **[CHANGELOG-1.9.md](CHANGELOG-1.9.md)**. QRXDesktop wird aus dem existierenden GitHub-Source weiterverwendet. Native AROS-Desktop-Bootfähigkeit auf QRX68100 ist **noch nicht vorhanden**.

---

# ARMITHLON / QRX68100 1.8

**Neu: wirklich startfähiges, interaktives QRX68100-68k-Firmware-System**
mit persistentem QRXFS-Datenträger und ausführbaren 68k-Demoprogrammen.

Auf dem Mac: `Start-QRX.command` doppelklicken oder im Terminal:

```bash
./armithlon boot
```

Das startet **ohne Fremd-ROM, GitHub-Authentifizierung und FS-UAE**. Gib `help`,
`ls`, `cat README.TXT`, `run HELLO.QEX`, `bench`, `write NOTES.TXT Hallo`,
`cat NOTES.TXT`, `reboot` und `shutdown` ein. Dateien bleiben über Upgrades
hinweg erhalten, weil QRXFS im persönlichen Anwendungsdatenverzeichnis liegt.

Für den bereits vorhandenen **AROS-Amiga-Desktop unter FS-UAE**:

```bash
./armithlon desktop
```

Dieser Desktop wird **nicht auf dem eigenen QRX68100 emuliert**. Das neue
Standalone-System ist eine Firmware/Shell, noch kein vollständiges AmigaOS,
AROS/Exec oder grafischer QRX-Desktop. Siehe [README-1.8-DE.md](README-1.8-DE.md)
für Architektur, Tests, Datenhaltung, Limitierungen und CLI-Befehle.

Alle bisherigen Engine-, QRX86-, Windows/Linux-Performance-, Metal-, Vulkan-,
OpenGL-, Glide-, Dual-Core- und Vampire-inspirierten Entwicklungskomponenten
sind im kumulativen Paket enthalten. Stand des vorherigen Meilensteins siehe
[README-1.7-DE.md](README-1.7-DE.md).
