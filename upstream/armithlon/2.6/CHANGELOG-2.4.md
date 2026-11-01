# ARMITHLON 2.4 – QRX68/1 AROS/QRXDesktop App-Bridge

- Sicher begrenzter Hostdienst `integration/qrx68-bridge/qrx68_service.py`:
  Spool-Queue (`QRXBRIDGE:inbox`) -> HUNK-CPU/HLE-Hostprozess -> Text,
  Fehlerstatus und 640×256-PPM (`QRXBRIDGE:results`). Exakte
  `QRX68/1`-Protokollkennung, feste IDs, Dateisignatur, No-Follow-Dateideskriptoren,
  atomare Ergebnis-Dateien, Laufzeit-/Ausgabelimits.
- **AROS/m68k**-CLI-Quellcode `engine/guest/aros/qrx68/QRX68Run.c`: lädt die
  angeklickte HUNK-Datei, übermittelt sie mit atomischer Queue-Übergabe,
  öffnet ein Intuition-Fenster und zeigt Ausgabe bzw. gerenderten Frame.
  Eindeutige Auftrags-ID per AROS `CreateDir()` reserviert.
- Gast-Build-Skript und nichtdestruktiver Installer; prüft echten HUNK-Magic,
  überschreibt keine bestehenden AROS-Systemprogramme.
- FS-UAE-Konfiguration optional mit normalem Hostverzeichnis als
  `QRXBRIDGE:` und extrahiertem AROS-System als `AROSSYS:`.
- `./armithlon qrx68-service`, `qrx68-aros-build`, `qrx68-aros-install`,
  `qrx68-aros-start`.
- **Vorhandener QRXDesktop nutzt schon WBPROJECT `DefaultTool`;** daher
  kann `C:QRX68Run` ohne Änderung am Desktop als Doppelklick-Handler dienen.
  Optionaler, pinsicherer 2.4-Sourcepatch erweitert 6.6.20 um QRXMODE-ToolType,
  `.qrx68`-Zuordnung und C:-Fallback, ohne die AROS-/QRX86-Routen zu ersetzen.
- Neue Integrationstests mit tatsächlich ausgeführten HUNK/Exec/DOS-Programmen,
  Graphik-Snapshot, Protokollfehlern, Symlinkabwehr, Quellpatch und Installer.
- **Kein unbelegter End-to-End-Nachweis:** AROS SDK, FS-UAE, Musashi und
  realer QRXDesktop konnten hier nicht als Gesamtsystem gestartet werden.
- **Keine interaktive AmigaOS-GUI-Kompatibilität behauptet:** IDCMP, echte
  Fensterprozesse, WBStartup und laufende Bildausgabe fehlen weiterhin.
