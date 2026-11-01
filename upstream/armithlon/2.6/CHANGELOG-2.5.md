# ARMITHLON 2.5 – QRX68/2 Live-Bridge

- Echte 68k-IntuiMessage-Warteschlange mit Window.UserPort (+86), GetMsg,
  ReplyMsg, WaitPort, IDCMP-Maus/Tastatur/Close; expliziter Wait-Yield.
- 640×256-Frame-Dirty-Tracking und laufende, atomische PPM-Bildupdates.
- Musashi-Laufzeit für fortsetzbare Sessions strukturiert (noch kein Mac-CPU-Nachweis).
- QRX68-Hostdienst führt bis zu vier HUNK-Sitzungen in separaten Prozessen,
  validiert Eingangsnachrichten und leitet sie ohne Gastpfad-/Shellzugriff weiter.
- AROS `QRX68Run` zeigt jetzt eine laufende Frame-Ansicht und sendet echte
  IDCMP-Ereignisse zurück; vorhandener QRXDesktop bleibt erhalten.
- Neues reales 68k-HUNK-Testprogramm mit Maus-Redraw und CloseWindow.
- CMake-Integrationstest mit zwei Live-Gastprozessen, Input-Isolation,
  falschen Ereignissen und ordentlichem Gastabschluss.
- Altes QRX68/1-Protokoll, QRX86, QRXFS, ARM64-JIT, Metal und andere Bestandteile
  weiterhin im kumulativen Archiv.
- Keine Behauptung einer getesteten AROS-Gast-Crosskompilierung oder eines
  nativen AROS-Boots auf QRX68100.
