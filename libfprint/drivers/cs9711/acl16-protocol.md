# 2541:2549 (LS-WINFG, "USB FingPintAcl16") protocol notes

Derived facts that the Acl16 path of `cs9711.c` relies on. No vendor
documentation or datasheet was found. The vendor binaries below were only
inspected statically (disassembly, never executed); no vendor code or
disassembly is part of this repository.

## Sources

| Source | Identity | Trust |
| --- | --- | --- |
| LANSTAR LS-WINFG Windows driver, page https://www.lanstar.co.kr/board/view.php?bdId=lanstardownload&sno=210 ("LS-WINFG(FpmDevAc16)", 2025-10-20) | ZIP sha256 `87c6ab2100225cb26723c4a86f377396caba8c3d93ae0eda5b5450e1f7f3fadd`; `WudfBioUsb.dll` sha256 `f001a74eabc2d8a235bf04a567870a2a152273a0d1863e4e368e9a78f8a5e6a1`; INF matches `USB\VID_2541&PID_2549&REV_0200&MI_01` | Official download for this product |
| Vendor Linux libfprint 1.0.3, attachment of https://github.com/archeYR/libfprint-CS9711/issues/6 | ZIP sha256 `4a8f752295132c9a29d4deb8eb741e43adbbb224dcd39e419acd285651d507d0`; `libfprint-2.so.2.0.0` sha256 `bce4933be5279b463e40263462c1b8102147b2f80ba1057d229adf238ff332bd` (unstripped, driver `chipsailing2549`) | Not authenticated by the vendor |
| Original Acl16 support, https://github.com/archeYR/libfprint-CS9711/pull/15 | 8192-byte read, 1500 ms waits | Initial author tested one reader; the PR does not establish a minimum timing requirement |
| Community installer, https://github.com/mmhfarooque/chipsailing-cs9711-fingerprint-linux/blob/main/install.sh | Changes 250 ms to 1500 ms to allow finger repositioning | UX rationale, not a hardware specification |
| Physical probes on one LS-WINFG, 2026-11-08 | user-mode PyUSB; only lengths, status and times recorded | This reader only |

## Wire protocol

- Interface 1, bulk OUT 0x01, bulk IN 0x81, full speed, 64-byte packets.
- Command: `ea <op> 00 00 00 00 <x> ea`, `x` = XOR of bytes 1..5 (here `op`).
  Used opcodes: `01` init (chip id query), `02` reset (stop scanning),
  `04` scan. The Windows driver sends `03` instead of `04` for enrollment
  captures and `07` on power-down; their meaning is unknown and Linux uses
  neither.
- Init reply: `ea 01 <id hi> <id lo> 00 00 <x> ea`. This reader answers
  chip id 0x62a2 (`ea 01 62 a2 00 00 c1 ea`).
- Chip 0x62a2 frame: 112 x 88 8-bit pixels = 9856 bytes, sent as one bulk
  transfer once a finger touches the sensor after a scan command. Neither
  scan nor reset is acknowledged on the IN pipe.

## Sequence used by the vendors

- Windows: init only once when the device starts (chip id and frame size).
  Per capture: start reading, send scan at once, wait without timeout for
  exactly 9856 bytes, stop reading, send reset. On cancellation no reset.
  No fixed sleep or timed settling wait was found in this capture path.
- Vendor Linux: no init at all. Activation sends scan and reads 0x2680
  (9856) bytes with a 4 s timeout; after a frame or a timeout it sends reset
  and immediately the next scan. Its template code uses 112 x 88.

## Physical observations (one reader, 2026-11-08)

- With 8192-byte reads every frame left exactly 1664 bytes queued
  (8192 + 1664 = 9856); single 9856-byte reads return whole frames.
- No init command is needed, also right after unplugging and replugging.
- Reset followed by scan 0.08 ms later works.
- A finger kept on the sensor for 10 s after reset and an immediate new scan
  produced no further frame; lifting and touching again did. The sensor
  therefore does not count one held finger as several placements.
- Release 0.1 failure: init sent milliseconds after the reset that followed
  a frame was answered normally, but the next scan never delivered a frame.

## Consequences in the driver

- Acl16 sends no init command and reads exactly 9856 bytes per frame. Only
  the exact scan echo `ea 04 00 00 00 00 04 ea` in front of a frame is
  skipped (its last 8 bytes are then read separately). A shorter read is
  never used: the scan is reset, the pipe drained and the sensor armed
  again, at most three times, then the action fails. Finger contact is
  reported only once the whole frame is in.
- Reset follows the frame at once; the next scan (next enroll stage or next
  action) follows without delay.
- Only when the pipe state is unknown (after open, a cancelled or a failed
  scan) the activation sends reset and drains until the pipe has been quiet
  for 80 ms, so that a frame queued by an earlier owner is discarded. A pipe
  that stays busy for seven reads fails the action instead.
- Images keep the top 112 x 73 rows (8176 bytes), the crop of the first
  Acl16 support, so prints enrolled with it stay comparable. Using all 88
  rows would change the images behind stored prints and needs an explicit
  migration decision.
- The legacy ids 2541:0236 and 2541:9711 keep their own sequence.

## Open questions

- Meaning of opcodes `03` and `07`.
- The vendor Linux driver drops frames with too few dark (< 0xa0) pixels;
  whether partial touches deliver such frames on this reader is unknown.
