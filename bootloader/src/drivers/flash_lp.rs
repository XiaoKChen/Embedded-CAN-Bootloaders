//! Code+data flash program/erase from RAM; data-flash read fail-safe gate
//! (SPEC §2.4/R34); never-write-below-`0x8000` guard (R3).
//!
//! Filled by U1.1 (grows spike S3 into production). `embedded-storage`
//! `NorFlash`-shaped blocking API.
