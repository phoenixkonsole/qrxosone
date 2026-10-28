QRX OS One Amiga 68K v0.20.3.7.4.6.6.6
Desktop Interaction / Terminal / MicroAI UX pass

Changes over 6.6.5:
- Desktop side app launcher and QRX/macOS dock interaction wired.
- Native app launch uses DOS SystemTags(SYS_Asynch) instead of Execute("Run ...") capture.
- Terminal typing repaints only the input line (no full-window flash per key).
- htop/top command: lightweight 68K task + memory view using AvailMem + Status.
- Date/time displayed in desktop top bar via AmigaDOS DateStamp/DateToStr.
- Fallback desktop app icons get distinct W/S/P/H glyphs and stronger framed icon face.
- New QRX:C/QRXAI native local chatbot.
  * friendly red QRX orb avatar with blink response animation
  * intent scoring / keyword routing / compact conversational responses
  * local RAM facts and QRX/Amiga help
  * intentionally no LLM: designed for useful latency/RAM on classic 68K
- AI/chat terminal commands launch QRXAI.
- 6.6.5 strict native AROS compiler -> ELF/m68k -> Deadwood elf2hunk -> final ISO HUNK proof retained.
