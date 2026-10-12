# VITA4 1.01: known-good reference

Saved 2026-10-06 as the reference point for future changes.

**Confirmed on a real PS4** (jailbroken, GoldHEN): the Vita is detected and
working.

## What changed from 1.00 to 1.01

| Problem in 1.00 | Fix in 1.01 |
|---|---|
| The PS4 USB library (`sceUsbd`) listed **no devices**, so the Vita was never found. The PS4 system claims the Vita itself and shows "Device not supported" when its stream stops. | At startup the app asks GoldHEN to lift its sandbox (`src/goldhen.cpp`). It then reaches the Vita through raw FreeBSD `/dev/ugen*` files, running the PS5 build's proven USB code (`core/src/usb_vita.c`, `usb_transfer.c`, `uvc_stream.c`, `uac_audio.c`) unchanged. `sceUsbd` is still tried first. |
| `sceAudioOutOpen failed: 0x809b0001` | Main-port audio is opened for the system user (`0xFF`), as in the OpenOrbis sample. |
| Very little logged about USB | Every scan is logged: the `sceUsbd` list, the `/dev` survey, and each `ugen` node's ID. |

`compat/` holds FreeBSD 9's USB ioctl headers, because the OpenOrbis libc
headers are Linux-style (musl) while the PS4 kernel is FreeBSD.

## Files

- `VITA4-1.01.pkg`: the exact package that was tested
- `ps4/`: complete source; rebuilds this package with `make` (see README.md)

Package SHA-256 is in `VITA4-1.01.pkg.sha256`.
