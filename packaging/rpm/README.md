# libfprint-cs9711 RPM (Fedora 44, x86_64)

Release 0.2 passed physical-device QA on Fedora 44 on 2026-11-08: fprintd
enrollment, same-finger match, different-finger rejection and passwordless
`sudo -k id -u`. [QA.json](QA.json) records the exact tested RPM hashes and
remaining limits. These results apply to those artifacts, not to later builds.

A private build of this fork for the LANStar LS-WINFG / "USB FingPintAcl16"
reader (USB `2541:2549`, fingerprint interface 1), plus the
`fprint-cs9711-diag` tool. It is test software for one reader model and has
not been verified for production authentication.

## What gets installed

| Package | Files | Effect on the system |
| --- | --- | --- |
| `libfprint-cs9711` | `/usr/lib64/libfprint-cs9711/lib/libfprint-2.so.2*`, `/usr/lib64/libfprint-cs9711/bin/fprint-cs9711-diag`, `/usr/lib64/libfprint-cs9711/share/metainfo/…`, docs, license | None. The library is not on the linker path. |
| `libfprint-cs9711-fprintd` (optional) | `/usr/lib/systemd/system/fprintd.service.d/50-libfprint-cs9711.conf` | fprintd, and only fprintd, loads the private library the next time it starts. |

The packages never touch the distribution `libfprint` or `fprintd` files,
`/etc/ld.so.conf*`, udev rules or hwdb, PAM/authselect, SELinux policy, or
`/var/lib/fprint`. They have no install scriptlets. Fedora's systemd file
triggers run `systemctl daemon-reload` when the drop-in is added or removed;
nothing restarts fprintd.

The private library keeps the upstream SONAME `libfprint-2.so.2`, which is
what lets fprintd use it through `LD_LIBRARY_PATH`. The spec filters it out
of the package's Provides and Requires, so it can never satisfy another
package's `libfprint-2.so.2()(64bit)` dependency.

## Limits to know before installing the fprintd subpackage

* **Fewer readers.** This fork is libfprint 1.94.10 plus the CS9711 driver.
  The distribution libfprint 1.94.100 supports 21 USB ids that the fork does
  not (several Elan `04f3`, Synaptics `06cb`, Goodix `27c6`, FocalTech
  `2808`, and others). With the drop-in active, fprintd cannot use those
  readers. Check the system first:

  ```sh
  lsusb
  comm -23 \
    <(grep -oiE 'usb:v[0-9a-f]{4}p[0-9a-f]{4}' /usr/share/metainfo/org.freedesktop.libfprint.metainfo.xml | tr A-F a-f | sort -u) \
    <(grep -oiE 'usb:v[0-9a-f]{4}p[0-9a-f]{4}' /usr/lib64/libfprint-cs9711/share/metainfo/org.freedesktop.libfprint.metainfo.xml | tr A-F a-f | sort -u)
  ```

  If any id in the command output is present on the machine, do not install
  `libfprint-cs9711-fprintd`.
* **ABI.** The tested fprintd 1.94.5 completed enrollment and verification
  with release 0.2. All 47 `fp_*`
  symbols that `/usr/libexec/fprintd` (fprintd 1.94.5) imports are exported
  by the fork, and the fork exports everything the distribution library
  does, plus two SIGFM functions. Recheck after any fprintd update:
  `nm -D --undefined-only /usr/libexec/fprintd | grep ' fp_'`.
* **Prints are per driver.** Prints enrolled through fprintd with this
  library belong to the `cs9711` driver. They stay in `/var/lib/fprint` when
  the packages are removed, but the distribution libfprint cannot use them.
  Delete them with `fprintd-delete <user>` *before* removal if they are no
  longer wanted; the packages never delete them.
* **Matcher.** Acl16 uses the SIGFM matcher with the default score threshold
  (40). Its false accept and false reject rates on this sensor have not been
  measured.
* **fprintd hardening is unchanged** (`MemoryDenyWriteExecute`,
  `SystemCallFilter=@system-service`, `ProtectSystem=strict`, …). The private
  library additionally needs OpenCV (`libopencv_core`, `libopencv_features2d`
  and, in the RPM build, `libopencv_stitching`; see `rpm -qp --requires`).
  Enrollment and authentication passed under this sandbox and enforcing
  SELinux (`fprintd_t`). A startup read of `/proc/sys/vm/nr_hugepages` was
  denied without preventing these operations; no policy was changed.

## Build

Needs `rpm-build`, `systemd-rpm-macros` and the `BuildRequires` of the spec
(meson, gcc-c++, glib2/libgusb/pixman/openssl/libgudev/opencv/cairo devel).

```sh
sh packaging/rpm/make-srpm.sh            # SRPM only
sh packaging/rpm/make-srpm.sh --binary   # SRPM and binary RPMs
RPMBUILD=/path/to/rpmbuild sh packaging/rpm/make-srpm.sh --binary
```

The script packs the working tree as it is (tracked plus untracked,
non-ignored files; build directories are ignored) and builds in
`packaging/rpm/out/`, which git ignores. A tree with uncommitted changes gets
`.dirty` in the release. `%check` runs the unit tests, including
`cs9711-driver`, and fails if any file would be installed outside
`/usr/lib64/libfprint-cs9711` apart from the drop-in.

Check the built packages before installing:

```sh
rpm -qplv packaging/rpm/out/RPMS/x86_64/libfprint-cs9711-[0-9]*.rpm
rpm -qp --provides packaging/rpm/out/RPMS/x86_64/libfprint-cs9711-*.rpm   # no libfprint-2.so.2
rpm -qp --requires packaging/rpm/out/RPMS/x86_64/libfprint-cs9711-*.rpm   # no libfprint-2.so.2
rpm -qp --scripts  packaging/rpm/out/RPMS/x86_64/libfprint-cs9711-*.rpm   # empty
```

## Install

```sh
sudo dnf install ./libfprint-cs9711-<ver>.x86_64.rpm          # base only: no effect on login
# optional, after the checks above:
sudo dnf install ./libfprint-cs9711-fprintd-<ver>.x86_64.rpm
sudo systemctl restart fprintd     # or wait until fprintd exits on its own
```

Confirm which library a running fprintd uses:

```sh
systemctl show fprintd -p Environment
sudo grep libfprint /proc/$(pidof fprintd)/maps    # expect /usr/lib64/libfprint-cs9711/lib/...
```

## Uninstall and rollback

```sh
sudo dnf remove libfprint-cs9711-fprintd     # fprintd back to the distribution libfprint
sudo systemctl restart fprintd               # drop the private library from the running daemon
sudo dnf remove libfprint-cs9711             # removes everything below /usr/lib64/libfprint-cs9711
```

After removal, `rpm -V libfprint fprintd` should be clean,
`/usr/lib/systemd/system/fprintd.service.d/50-libfprint-cs9711.conf` and
`/usr/lib64/libfprint-cs9711` should not exist, and
`/proc/$(pidof fprintd)/maps` (once fprintd runs again) should list
`/usr/lib64/libfprint-2.so.2`. `/var/lib/fprint` is left as it was.

## fprint-cs9711-diag

Runs against the reader directly, not through fprintd. The reader must not
be held by fprintd at the same time (no enroll/verify in progress), and the
user needs access to the USB device node.

```sh
D=/usr/lib64/libfprint-cs9711/bin/fprint-cs9711-diag
$D list
$D capture --count 3 --timeout 20
$D enroll-verify --count 3 --timeout 30     # template only in memory
$D cycle --count 10
$D --debug capture                          # driver/state machine logs on stderr
```

* stdout carries one JSON object per line. `t` is seconds since start. Key
  events: `opened`, `op-start`, `waiting-for-contact` (the sensor is armed;
  for 2541:2549 it is emitted only after the scan command has been sent),
  `contact` (a frame arrived), `enroll-progress`, `match-report`, `image`
  (size and intensity statistics only), `result` with `outcome` = `match`,
  `no-match`, `enrolled`, `captured`, `retry`, `timeout`, `interrupted` or
  `error`, and finally `exit`.
* stderr carries hints for the person at the reader ("sensor armed - place
  your finger now", "Frame received - lift your finger").
* `--timeout S` bounds each result or enroll stage (default 30 s). On
  timeout or Ctrl-C the operation is cancelled and the reader is closed. If
  cancellation is ignored for 10 s the tool reports `cancel-unresponsive` and
  exits 6.
* Exit status: 0 success, 1 no match (`verify`), 2 error, 3 timeout,
  4 interrupted, 5 retry, 6 cancellation ignored, 64 usage, 69 no reader.
  For `enroll-verify`, 0 means every verification produced a definite match
  or no-match. The first failure decides the code; a failed close turns an
  otherwise successful (or no-match) run into 2, and `cycle` stops at the
  first failed open or close. A signal during `cycle` exits 4.
* No image is ever written. `FP_DEBUG_TRANSFER` is cleared at start because
  libfprint would hex-dump raw frames with it. A template is written only
  with `--save-template FILE`: the file is created with mode 0600, never
  overwritten and never followed through a symlink. Verify with
  `--template FILE` and delete the file afterwards (`rm FILE`).

## Verified results (2026-11-08)

[QA.json](QA.json) is the durable record of the tested release, environment,
package hashes and remaining limits. Detailed local logs are in
`packaging/rpm/out/qa/`; that directory is ignored by Git and is not included
in a fresh clone. No fingerprint images or templates are part of this record.

| Environment / artifact | Result |
| --- | --- |
| Direct ASAN+UBSAN build, real `2541:2549` | 15-stage enrollment; index MATCH, middle NO MATCH, index MATCH; no sanitizer findings |
| Direct device lifecycle checks | Three cancel/reopen cycles, 20 open/close cycles, clean 180 s and 5 s timeouts |
| Final driver ASAN+UBSAN suite | 19 scripted USB tests passed |
| Virtual-driver CLI harness | 22 checks passed |
| Final RPM `%check` | Five unit suites passed |
| Release 0.1 install/remove/reinstall | Passed in a Fedora 44 container and on the real host; stock library restored on removal |
| Host upgrade from 0.1 to 0.2 | Passed; stock libfprint/fprintd/PAM and offline-update files unchanged |
| Release 0.2 with system fprintd | All 16 stages completed, then `verify-match` for the index and `verify-no-match` for the middle |
| Release 0.2 with sudo | `sudo -k /usr/bin/id -u` returned `0` using only the fingerprint, with the temporary debug override removed |

The Acl16 init response observed was `ea 01 62 a2 00 00 c1 ea`.
The legacy ids use `ea 01 62 a0 00 00 c3 ea`.

### Consecutive-action timing

Release 0.1 stalled when fprintd's duplicate-check scan immediately started
registration: INIT followed the previous RESET within milliseconds. Standalone
registration worked because its stages already waited 1.5 s after RESET.
Release 0.2 preserves that quiet period before the next action's INIT, checks
the monotonic deadline after each timer wakeup, and tests chained actions and
cancellation during the wait. The measured RESET-to-INIT gap was 1.581837 s
in the successful fprintd run. This supports the timing fix on the tested
reader; it is not a vendor protocol specification.

### Reproducing installation QA

Inspect package paths, dependencies and scriptlets first. Confirm that no
other attached reader needs an id missing from this fork. After installing,
check the mapped library, enroll through fprintd, verify both the same and a
different finger, and test sudo without its authentication cache. Check the
service sandbox and SELinux audit log. Removal should restore the stock
library and leave its files and registered fingerprints intact.

When dependencies are already installed, the tested host used dependency-checked
RPM transactions to preserve its existing DNF offline-update state:

```sh
sudo rpm -Uvh --test ./libfprint-cs9711-<ver>.x86_64.rpm ./libfprint-cs9711-fprintd-<ver>.x86_64.rpm
sudo rpm -Uvh ./libfprint-cs9711-<ver>.x86_64.rpm ./libfprint-cs9711-fprintd-<ver>.x86_64.rpm
sudo systemctl daemon-reload
sudo systemctl restart fprintd
```

Equivalent removal without deleting registered fingerprints:

```sh
sudo rpm -e libfprint-cs9711-fprintd libfprint-cs9711
sudo systemctl daemon-reload
sudo systemctl restart fprintd
```

## Remaining validation

* False accept/reject rates and spoof resistance have not been measured.
* 1Password integration has not been tested.
* Disconnect during an active read has scripted USB coverage only. The
  physical unplug attempt occurred after capture had already completed.
* Behavior when a finger is already on the sensor before it is armed still
  needs a dedicated check.
* Legacy `2541:0236` and `2541:9711` readers have unit coverage only.

The installed 0.2 RPMs were built before the final documentation and agent
instructions. Their `.dirty` filenames and hashes remain historical evidence;
committing the source does not rebuild or reinstall those packages.
