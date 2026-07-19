/* ==========================================================================
 * memory.x — ra4m1-hal examples (plan-m3-hal.md §3)
 *
 * HAL examples link at the app base 0x0000_8000 (the same slot every app for
 * this board links into — the bootloader owns 0x0000_0000..0x0000_8000), so
 * they can be flashed over/behind the bootloader and run under probe-rs.
 * Mirrors demo-app/memory.x. No `.ofs` section: the option bytes live inside
 * the bootloader image, and the app slot sits entirely past them.
 * ========================================================================== */

MEMORY
{
  FLASH : ORIGIN = 0x00008000, LENGTH = 0x37800
  RAM   : ORIGIN = 0x20000000, LENGTH = 32K
}
