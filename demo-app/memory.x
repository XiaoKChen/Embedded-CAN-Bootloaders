/* ==========================================================================
 * memory.x — demo app, linked into the bootloader's app slot
 * (docs/plan.md §1.2). ORIGIN/LENGTH must stay in agreement with the
 * bootloader's `BOOT_SIZE`/`APP_SLOT_SIZE` (crates/boot-logic/src/memory_map.rs,
 * owned by U0.4) — every app for this board links here, not at the stock
 * Arduino IDE offset 0x4000.
 *
 * Deliberately NO `.ofs` section and no `_stext` bump: the OFS0/OFS1/
 * Security-MPU option bytes live at 0x400 inside the *bootloader* image
 * (bootloader/src/ofs.rs) — the app slot sits entirely past them.
 * ========================================================================== */

MEMORY
{
  FLASH : ORIGIN = 0x00008000, LENGTH = 0x37800
  RAM   : ORIGIN = 0x20000000, LENGTH = 32K
}
