/*
 * Flash and RAM regions for the ra4m1-hal examples, consumed via cortex-m-rt's link.x.
 *
 * The examples link at the app base 0x0000_8000, the same slot every app for this board links
 * into, so they can be flashed over or behind the bootloader and run under probe-rs. Mirrors
 * demo-app/memory.x (plan-m3-hal.md section 3).
 *
 * Owns:        the flash and RAM regions the example binaries link into.
 * Not here:    the bootloader's own map; the target and runner (workspace .cargo/config.toml).
 * Invariants:  the bootloader owns 0x0000_0000..0x0000_8000, so FLASH starts at the app slot.
 *              There is no .ofs section: the option bytes live inside the bootloader image,
 *              and the app slot sits entirely past them.
 * See also:    build.rs (copies this file to OUT_DIR); demo-app/memory.x.
 */

MEMORY
{
  FLASH : ORIGIN = 0x00008000, LENGTH = 0x37800
  RAM   : ORIGIN = 0x20000000, LENGTH = 32K
}
