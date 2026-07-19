# docs/

Design, research, and verification documentation for the Rust/Embassy rewrite
of the UNO R4 Minima (RA4M1) CAN + USB-DFU bootloader.

**Convention:** file names are lowercase, hyphenated, no spaces. Milestone
implementation plans live at the `docs/` root; supporting material is grouped
into subfolders by kind.

## Layout

```
docs/
├── plan.md                 # M0–M2 master implementation plan
├── plan-m2.5-renode.md     # M2.5 — Renode emulation validation (design only)
├── plan-m3-hal.md          # M3 — from-scratch RA4M1 HAL (design only)
├── research/               # background: reverse-engineered spec, ecosystem, audits
│   ├── master-feature-spec.md
│   ├── embassy-ra4m1-ecosystem.md
│   └── ra4m1-pac-coverage.md
└── bench/                  # on-hardware bring-up checklists
    └── m0-spikes.md
```

## Plans (`docs/`)

| Doc | What |
|---|---|
| [plan.md](plan.md) | Master plan (M0–M2): memory map, workspace layout, concurrency architecture, spikes S0–S5, milestone/unit tables, verification strategy. The plan of record cited throughout the source. |
| [plan-m2.5-renode.md](plan-m2.5-renode.md) | M2.5 — validate the firmware in the Renode emulator. Per-peripheral feasibility, C#-model scope, CI plan, spikes S-R1..S-R5. Design only. |
| [plan-m3-hal.md](plan-m3-hal.md) | M3 — a from-scratch, LGPL-free (`ra4m1-hal`) application HAL. Crate layout, trait surface, peripheral phasing, spikes S-A..S-D. Design only. |

## Research (`docs/research/`)

| Doc | What |
|---|---|
| [master-feature-spec.md](research/master-feature-spec.md) | Implementation-independent behavior spec reverse-derived from the C bootloader on `main` (SPEC §n / R1–R43). |
| [embassy-ra4m1-ecosystem.md](research/embassy-ra4m1-ecosystem.md) | Sourced ecosystem readiness study (HAL/PAC options, tooling) behind the stack choices (ECO §n). |
| [ra4m1-pac-coverage.md](research/ra4m1-pac-coverage.md) | M3 spike S-B: which RA4M1 peripheral register blocks the `ra4m1` PAC exposes (all typed — no raw MMIO needed). |

## Bench (`docs/bench/`)

| Doc | What |
|---|---|
| [m0-spikes.md](bench/m0-spikes.md) | Hardware bring-up checklist for the M0 spikes (clocks, VBTBKR, executor) on the physical board via `probe-rs`. |

> Note: some docs cite files that live only on the `main` branch (the original C
> bootloader) — e.g. `docs/build.md`, `docs/research/r4-minima-can-bootloader.md`.
> Those are intentional `git show main:<path>` references, not broken links.
