# Benutzer, verschlüsselte Homes und das Netz

Stand 2026-11-17. Dieses Dokument gilt für QRX OS One 64 auf dem 68100EC64.

## Benutzer und Admin

- **Wo die Daten liegen:** Die Engine verwaltet die Benutzer auf dem Mac, in `~/Library/Application Support/QRX OS One/users/` (Modus 0700).
  - `users.db` enthält die Benutzerliste.
  - `<name>.qrxu` ist das Home-Image eines Benutzers.
- **Der erste Benutzer ist immer Admin.** Weitere Benutzer kann nur ein Admin anlegen.
  - Der erste Benutzer lässt sich nicht löschen.
  - Ein Admin kann Passwörter zurücksetzen und Benutzer löschen.
- **Login:** QRXDesktop fragt vor dem Desktop nach Name und Passwort.
  - Beim ersten Start legt man dort das Admin-Konto an.
  - Danach hängt die Shell das Home des Benutzers als `HOME:` ein. `HOME:` liegt auf `qrxdisk.device` Unit 1, die Shell formatiert es beim ersten Mal als FFS „Home“ und legt `Documents` und `Pictures` an.
- **Abmelden:** Das Startmenü hat unter Power „Log out“.
  - Die Shell schreibt zuerst die Puffer von `HOME:` (Inhibit).
  - Dann wirft die Engine den Schlüssel weg, und das System startet neu.
  - Neustart und Ausschalten schreiben `HOME:` ebenfalls vorher.
- **Benutzerverwaltung:** In der App „Users“ ändert jeder sein Passwort. Admins legen dort außerdem Benutzer an, setzen Passwörter zurück und löschen Benutzer.
- **Netzwerk-Einstellungen:** `QRXNetwork` darf sie nur noch als Admin ändern. Solange noch niemand angelegt ist, darf es jeder.

## Verschlüsselung (`full68k/qrx_vault.c`)

| Was | Wie |
| --- | --- |
| Home-Image | AES-256-XTS, ein 512-Bit-Volumeschlüssel pro Benutzer, die Sektornummer als Tweak. Neue Images bestehen ganz aus verschlüsselten Nullen, man sieht also nicht, welche Teile belegt sind |
| Passwort | Argon2id, 64 MiB, 3 Durchgänge, 16 Byte Salz. Der Schlüssel daraus verpackt den Volumeschlüssel mit AES-256-GCM. Name und Zweck sind dabei Associated Data |
| Wiederherstellung | Das System hat ein ML-KEM-1024-Schlüsselpaar (FIPS 203, gegen Quantenrechner). Der Volumeschlüssel jedes Benutzers ist zusätzlich unter einem ML-KEM-Geheimnis verpackt. Den privaten Schlüssel hat jeder Admin, unter seinem eigenen Passwort verpackt |
| Im Gast | Die Schlüssel kommen nie in den Gastspeicher. Der Gast liest und schreibt Klartext-Sektoren über Host-Call-Dienst 9; die Engine ver- und entschlüsselt sie |

- **Bibliothek:** OpenSSL 3.5 oder neuer liefert ML-KEM und Argon2id. Die Engine linkt `libcrypto.a` statisch, aus `QRX_OPENSSL_ROOT`; Vorgabe ist `/opt/homebrew/opt/openssl@3.6`. Ohne OpenSSL gibt es keine Benutzer, und das System läuft wie bisher.
- **Test:** `qrx_vault_tests` prüft Admin-Regeln, XTS-Rundlauf, falsche Passwörter, Reset über ML-KEM durch einen zweiten Admin und das Neuladen aus `users.db`.
- **Testumgebungen:** `QRX_VAULT_DIR` legt das Verzeichnis woanders hin. `QRX_VAULT_TRACE=1` protokolliert jeden Aufruf, ohne Inhalte. `SYS:QRX/logintest` mit „Name\nPasswort“ füllt den Login einmal aus.

## Netz: direkt, Tor, WireGuard, offline

- **Einstellung:** `QRXNetwork` wählt den Modus. Die Engine speichert ihn in `net.cfg` (0600), neben `users/`.
- **Tor und WireGuard:** Die Engine startet `qrxnetd` aus `ARMithlon.app/Contents/MacOS/`. Das Programm stammt aus `tools/armithlon/qrxnetd` und baut auf arti, boringtun und smoltcp auf.
- **Kein Ausweg am Tunnel vorbei:**
  - Jede TCP-Verbindung des Gasts geht als SOCKS5 über qrxnetd.
  - DNS geht durch den Tunnel.
  - UDP nach außen ist gesperrt.
  - Solange qrxnetd nicht „READY“ meldet oder nicht läuft, schlagen alle Verbindungen fehl. Die Mac-Adresse wird also nie benutzt.
- **Getestet:**
  - Tor: Eine HTTP-Abfrage aus dem Gast kam mit einer anderen IP als der des Macs an.
  - Sperre ohne qrxnetd: Weder Namensauflösung noch direkte IP-Verbindung kamen durch.
  - **WireGuard ist ungetestet,** weil kein Server zur Verfügung stand.
