# Contract Spike Atom Replay

## Shared architectural reference (`rv32-v1`)

The adapter exposes `contract_spike_atoms_json` and
`contract_spike_atoms_json_v2(json, isa, ordinal, model)`. Both execute the same
shared reference. The compatibility option `--execution-model rv32-v1` is
accepted, while `legacy-v1` is rejected. No core identity is accepted by Spike.

`rv32-v1` executes Spike's own RV32I/M handlers at architectural addresses:
2048 words at `0x80`, 31 register initializer slots, a NOP spacer, and testcase
instructions at `0x100`. It runs `31 + maxInstructionCount` retirements. Instruction
and data storage are separate; untouched data is zero and stores persist for the
whole testcase. Fetches outside the image return NOP, including RV32 wraparound.

Misaligned scalar loads/stores are supported through Spike's Zicclsm MMU paths
for all accepted ISA spellings (`RV32I`, `RV32IM`, and their `_Zicclsm` forms).
This shared execution environment is independent of any RTL core's alignment
capability. Unsupported instructions and misaligned instruction/control-flow
targets still invalidate a testcase; compressed and privileged instructions are
outside this specification.

Canonical responses identify `execution_model` and each case reports
`requires_misaligned_data_access`, true if either side actually executed a data
access not naturally aligned to its width. Unreachable instructions do not set
the requirement. The flag is metadata, not a contract atom or attacker label.
Consumers must check target-core capabilities separately: a core which traps on
such accesses excludes the case, while its reference evidence remains reusable.

`MEM_R_DATA` places each accessed byte in lane `(address + byte_offset) mod 4`;
unaccessed lanes are zero. This preserves aligned observations and defines split
accesses without depending on a core's bus width. `MEM_ADDR` is the RV32 effective
address and `MEM_W_DATA` is the source register value.

The native architectural regression lives in
`spike_main/contract-spike-execution-model-test.cc`. It covers misaligned byte
lanes, signed/unsigned halfwords, page splits, RV32 wraparound, persistent stores,
dynamic requirements and reset isolation, alongside the existing reference tests.
The parent project's Nix patch mirrors the local adapter/test source against the
unchanged pinned revision, so Nix builds and a local rebuild use the same code.


The old manual evaluator is archived by the parent project in
`results/spike-reference-migration-20261012/archive/`. Historical results require
the archived reference and harness environment; they are not assumed identical
to the current architectural reference. See the parent migration report.
