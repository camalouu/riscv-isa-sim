# Contract Spike Diff

This Spike clone adds a small standalone CLI for atom distinguishability:

```sh
build/contract-spike-diff \
  --testcases ../1000-IBEX-testcases.json \
  --all \
  --json-out /tmp/spike-atoms.json
```

It consumes contractgen testcase JSON directly. It does not consume ELF files and it does not consume `1000-IBEX-results.json`; that results file was only used externally as a validation oracle.

## Files Changed

- `spike_main/contract-spike-diff.cc`
  Implements the JSON parser, testcase instruction encoder, in-process Spike runner, atom extractor, and CLI output.

- `spike_main/spike_main.mk.in`
  Adds `contract-spike-diff.cc` to the Spike main-program build list so `make contract-spike-diff` produces the binary.

## Input And Output

Input is a contractgen testcase JSON array. Each testcase must contain:

- `index`
- `registers1`, `program1`
- `registers2`, `program2`
- `maxInstructionCount`

The output is JSON. For `--case-index N`, the shape is:

```json
{
  "case_index": 0,
  "atoms": [
    {"type": "LUI", "observation": "RD"}
  ]
}
```

For `--all`, the shape is:

```json
{
  "cases": [...],
  "summary": {"total": 1000, "failed": 0}
}
```

A nonzero exit status means at least one testcase had an execution/extraction error. Successful atom mismatches against an external oracle are not checked by Spike itself.

## Main Implementation Choices

The tool runs Spike in-process instead of launching `spike` on an ELF. It constructs two synthetic RV32 programs from each testcase:

1. 31 register-initialization instructions, matching contractgen's init layout.
2. One NOP gap, matching the existing feasibility prototype layout.
3. The testcase program instructions.
4. NOP padding.

The extractor compares retired samples after the 31 init instructions, so the synthetic initialization code is not reported as atom-distinguishing behavior.

The tool encodes the symbolic contractgen instructions itself. The supported instruction set currently covers the RV32 base integer instructions used by the testcase file plus RV32M arithmetic operations.

Spike is configured with:

- ISA default: `RV32IM_Zicclsm`
- privilege mode: `M`
- memory mapped at `0x1000`
- PC start at `0x1000`
- HTIF argument `none`, so Spike does not require an ELF payload

## Assumptions

This implementation is intentionally calibrated to the current contractgen/IBEX testcase workflow.

- Testcases are RV32.
- Register initialization follows contractgen's `ADDI xN, x0, imm` behavior, including 12-bit signed immediate effects.
- Root oracle files stay outside the Spike clone.
- The tool reports current base atoms:
  `RD`, `RS1`, `RS2`, `IMM`, `REG_RS1`, `REG_RS2`, `REG_RD`, `MEM_ADDR`, `MEM_R_DATA`, `MEM_W_DATA`.
- Branch target execution follows the harness-style finite instruction image: out-of-image instruction fetches are modeled as NOPs.
- If a branch-taken decision differs between the two sides, comparison stops after the branch sample. This matches the observed oracle behavior for the current 1000 IBEX cases.
- Loads and stores are modeled for atom observation compatibility with the IBEX harness, not as general Spike memory semantics. The custom IBEX `data_mem.sv` returns address-derived read data (`addr % 0x1000`) and exposes byte-enable-masked RVFI memory data; the tool mirrors that convention.
- Writes to `x0` do not produce `REG_RD`, matching the oracle behavior.

## Shift-Immediate Operand Caveat

`SLLI`, `SRLI`, and `SRAI` are architecturally shift-immediate instructions:

```text
SLLI rd, rs1, shamt
SRLI rd, rs1, shamt
SRAI rd, rs1, shamt
```

The last operand is `shamt`, not `rs2`. It is encoded in instruction bits `[24:20]`, the same bit position used by `rs2` in R-type instructions, but it is an immediate field and does not name a source register.

The current Java contractgen ISA model represents these three instructions as `RISCV_FORMAT.RTYPE` and stores `shamt` in the `rs2` field. That is a convenient encoding shortcut because R-type encoding already has `funct7 | rs2 | rs1 | funct3 | rd | opcode`, which matches the shift-immediate bit layout as `funct7 | shamt | rs1 | funct3 | rd | opcode`. Semantically, however, this pollutes the contract atom model:

- `SLLI/RS2`, `SRLI/RS2`, and `SRAI/RS2` should not be valid ISA-level atoms.
- `SLLI/REG_RS2`, `SRLI/REG_RS2`, and `SRAI/REG_RS2` should not be valid runtime register-value atoms.
- `RAW_RS2_*` dependency atoms should not apply to these instructions.
- A differing shift amount should be reported as `IMM`, or as a future dedicated `SHAMT` atom if the contract vocabulary is made more precise.

This explains why generated IBEX result files can contain atoms such as:

```json
{ "type": "SLLI", "observation": "REG_RS2" }
```

in `ALL_ATOMS`. In the checked `1000-IBEX-results.json` and `10000-IBEX-results.json` files, this atom appears in the atom universe, not as an actual per-test observation. It is admitted because `RISCVObservation.isApplicable()` delegates to `RISCVInstruction.hasRS2(type)`, and `hasRS2(SLLI)` currently returns true due to the R-type shortcut.

For Spike-side atom distinguishability, the desired behavior is architectural:

- Treat `SLLI/SRLI/SRAI` as having `rd`, `rs1`, and `imm/shamt`.
- Do not read or compare `x[shamt]` as an `rs2` register.
- Do not emit `RS2`, `REG_RS2`, `REG_RS2_ZERO`, `REG_RS2_LOG2`, or `RAW_RS2_*` for these instructions.
- Emit `IMM` when the shift amount differs.

Until contractgen's Java ISA model is corrected and oracle files are regenerated, exact oracle comparisons involving these shift-immediate `RS2` atoms should be interpreted as oracle/model mismatches, not necessarily Spike implementation bugs.

## Validation

The final validation command was:

```sh
build/contract-spike-diff \
  --testcases ../1000-IBEX-testcases.json \
  --all \
  --json-out /tmp/spike-atoms.json
```

The generated `/tmp/spike-atoms.json` was compared externally against `../1000-IBEX-results.json`.

Result:

```text
summary {'total': 1000, 'failed': 0}
mismatches 0
```

## Build Notes

This upstream Spike checkout required `dtc` during configure/build. In this workspace, the working build command was:

```sh
nix shell nixpkgs#dtc -c make -C build -j2 contract-spike-diff
```

The built binary is:

```text
build/contract-spike-diff
```

## Limitations

This is not yet a shared-library integration with contractgen. It is a standalone CLI proving that Spike can consume contractgen testcase JSON directly and produce atom distinguishability without generating `.dat` files or parsing Spike text traces.

The memory and branch edge behavior is currently matched to the IBEX harness oracle. If the next target is a processor-independent ISA-level atom oracle, those conventions should be separated behind a selectable compatibility mode.
