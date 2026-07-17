/* ==========================================================================
 * memory.x — bootloader region (docs/plan.md §1.2, spike S1)
 *
 * Single source of truth for the bootloader's flash/RAM partition (values
 * from R01UM0007 / docs/plan.md §1 — reviewed like unsafe code):
 *
 *   0x0000_0000 .vector_table  cortex-m-rt: SP + reset + 14 exceptions
 *                              (0x40) + 240 default IRQ vectors (0x3C0)
 *                              = exactly 0x400 bytes
 *   0x0000_0400 .ofs           OFS0/OFS1/Security-MPU words (src/ofs.rs,
 *                              SPEC §1.5 — brick risk, byte-checked in CI)
 *   0x0000_0440 .text          _stext moved past the option words
 *   ...
 *   0x0000_8000                end of the 32 KB bootloader region; the app
 *                              slot starts here (APP_BASE, docs/plan.md §1)
 *
 * RAM: 32 KB SRAM at 0x2000_0000 (whole chip; the bootloader never hands
 * RAM to the app — the app re-owns all of it after the jump).
 * ========================================================================== */

MEMORY
{
  FLASH : ORIGIN = 0x00000000, LENGTH = 32K
  RAM   : ORIGIN = 0x20000000, LENGTH = 32K
}

/* Code resumes after the option/ID words; overrides the PROVIDE(_stext =
 * end of .vector_table) default in cortex-m-rt's link.x. */
_stext = ORIGIN(FLASH) + 0x440;

/* Option-setting words at their hardware-mandated flash address
 * (R01UM0007, Option-Setting Memory: OFS0 at 0x400, OFS1 at 0x404,
 * Security-MPU words through 0x43F). */
SECTIONS
{
  .ofs ORIGIN(FLASH) + 0x400 :
  {
    KEEP(*(.ofs));
  } > FLASH
} INSERT AFTER .vector_table;

ASSERT(ADDR(.ofs) == ORIGIN(FLASH) + 0x400, "
ERROR(memory.x): the .ofs option-byte section must sit at flash 0x400 —
flashing an image with it elsewhere risks bricking the board (SPEC §1.5).");

ASSERT(SIZEOF(.ofs) == 0x40, "
ERROR(memory.x): .ofs must be exactly 64 bytes (OFS0/OFS1 + Security-MPU
words, 0x400..0x43F) — check bootloader/src/ofs.rs.");
