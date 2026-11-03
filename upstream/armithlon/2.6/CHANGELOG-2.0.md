# ARMITHLON 2.0 – 2026-11-01

- Physischer Host-Ordner als virtuelles QRXHostFS-MMIO-Device (`FF000800`)
  inklusive 4 KiB begrenzter DMA-Transfers, offsetbasierter I/O, LIST/STAT,
  regulärer Datei-I/O, Truncate, Delete, Status, Write-Opt-in und Schutz
  vor Pfad-Traversal/Symlink-Öffnung.
- Firmware-Shell: `hostinfo`, `hostls`, `hostcat`, `hostget`, `hostput`,
  `hostrm`, `run HOST:...QEX` über tatsächlich ausgeführte 68k-Firmware-
  Kommandoübergabe und HostFS-Port.
- `--volatile-disk`: booten und Dateien aus HOST: nutzen, **ohne Image-Datei**.
- `--ram-mib 1..8`: auf bis zu 8 MiB erweiterbarer linearer Low-RAM ohne
  ROM-Kollision (kein vollständiger MMU-Mapper).
- AROS-/QRX-Gastquellen: QRXShare-Filesystemtransfer, QRXBoardInfo,
  QRXSmpInfo, obere MMIO-Adresse für QRX-68040-Gasttreiber,
  separates SDK-Build-Skript, **nicht** als fertige AROS Geräte gemeldet.
- SMP: kleine, echte synchronisierte gemeinsame Ready-Queue mit CPU-Affinität
  und Priorität; parallele Hosttests; kein Exec-SMP-/68k-SMP-Bootnachweis.
- AROS native Readiness Gate um neue Quellen und ausstehende Aufgaben ergänzt.
- Legacy-Standard: 68k UP und QRXFS .img unverändert; AROS/QRXDesktop
  weiterhin ausschließlich über externen AROS-Compat-Boot bzw. offen.
- Mindestens 33 CTests unter Linux x86-64 geprüft, siehe build/test logs.
