# Working on ios-topaz → Nothing Phone (3a) (`asteroids` branch)

You are probably an AI coding agent helping with this port. Read this first.

## Hard rules (phone safety)

- **Never write to the phone's storage.** No `fastboot flash`, no `fastboot erase`,
  no writes to `userdata` or any partition, no `rd=ufsroot0`, no `sweet-ufs` ADT node.
  Everything boots from RAM with `fastboot boot`; a reboot must always bring Android back.
  (The topaz rule "userdata may be erased" does NOT apply to this branch.)
- **No fixed SM6225 (topaz) addresses on this phone.** Any driver that maps a hardcoded
  physical address must be gated so it cannot run on `asteroids` (see patches 0953, 0954
  and the ADT: no `wlan`, `disp0`, `sweet-ufs` nodes). New MMIO must come from the
  phone's own device tree and be explained in the patch.
- Keep Apple artifacts (IPSW, device trees, root images), device dumps and serial
  numbers out of git.

## Where things are

| Path | What |
| --- | --- |
| `README.md` (top section) | Phone facts, status, patch list |
| `tools/mkadt.py --asteroids` | Apple Device Tree for the phone |
| `shim-asteroids/` | Boot shim: reads the ABL DTB, prepares the GIC, loads XNU (LLVM: `clang --target=aarch64-none-elf`) |
| `xnu/patches/0951-0954` | asteroids XNU patches; the rest are topaz/sweet |
| `.github/workflows/xnu.yml` | Kernel build on macOS runners: `gh workflow run xnu.yml --ref asteroids` |

## How to work

- Patches apply in filename order on xnu-8796.141.3 (macOS 13.5). To write a new one,
  apply the series to that tag, change the tree, and `git diff` into `xnu/patches/NNNN-asteroids-*.patch`
  with LF line endings. A patch that does not apply is silently skipped by CI: check the log.
- Prefer one small patch per change, with a comment that says why.
- Mark what you observed on the phone versus what you guess. Open issues are the task list.
