# Third-Party Notices

`qlc` (Qualcomm Log Collector) is distributed under the
[BSD-3-Clause Clear License](LICENSE.txt) and incorporates or links
the following third-party components. Each component is covered by
its own license; those licenses remain in effect for the respective
component and do **not** apply to `qlc`'s own source code.

---

## libusb (1.0.x)

- Upstream project : <https://libusb.info>
- Upstream source  : <https://github.com/libusb/libusb>
- Version used     : **1.0.27**
- Exact source     : <https://github.com/libusb/libusb/releases/tag/v1.0.27>
- Modifications    : none (the upstream release is used unmodified)
- License          : **GNU Lesser General Public License v2.1 or later**
                     (LGPL-2.1-or-later)
- Link mode        : `qlc` **dynamically loads** libusb at runtime
                     (`libusb-1.0.dll` on Windows, `libusb-1.0.so` on
                     Linux). Pre-built import libraries and the public
                     header are vendored under
                     `src/qds/libusb/lib-1.0.27/` for build
                     convenience only.

> Notice required by LGPL §6: *This product uses libusb, a library
> licensed under the GNU Lesser General Public License version 2.1
> or (at your option) any later version. The corresponding source
> code of the libusb version used by this product is available from
> <https://github.com/libusb/libusb/releases/tag/v1.0.27>.*

---

## kLogger

- Upstream project : vendored under `src/external/kLogger/`
- Modifications    : none
- License          : **MIT**. See
                     [`src/external/kLogger/LICENSE`](src/external/kLogger/LICENSE).

---

## Adding a new third-party dependency?

1. Drop the verbatim upstream license text into a `LICENSES/` directory
   (create it if it does not yet exist) as
   `LICENSES/<name>-<SPDX-id>.txt`.
2. Add an entry to this file with: name, upstream URL, version,
   license SPDX identifier, link mode (static / dynamic / header-only),
   and a pointer to the license copy.
3. If the new component is under a copyleft license (LGPL, GPL,
   MPL, …), document the source-availability and relinking
   obligations for downstream redistributors.
