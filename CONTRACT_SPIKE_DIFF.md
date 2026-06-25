# Contract Spike Diff

This Spike clone adds atom distinguishability for contractgen testcase JSON. It now has both:

- a standalone CLI, `build/contract-spike-diff`
- a shared library, `build/libcontract_spike_atom.so`, used from Java through `SpikeAtomClient`

Example standalone run:

```sh
build/contract-spike-diff \
  --testcases ../1000-IBEX-testcases.json \
  --all \
  --json-out /tmp/spike-atoms.json
```

It consumes contractgen testcase JSON directly. It does not consume ELF files and it does not consume `1000-IBEX-results.json`; that results file was only used externally as a validation oracle.

## Files Changed

- `spike_main/contract-spike-diff.cc`
  Implements the JSON parser, testcase instruction encoder, in-process Spike runner, atom extractor, CLI output, and C ABI used by the Java shared-library client.

- `spike_main/spike_main.mk.in`
  Adds `contract-spike-diff.cc` to the Spike main-program build list so `make contract-spike-diff libcontract_spike_atom.so` produces the binary and shared library.

- `src/main/java/contractgen/riscv/isa/spike/SpikeAtomClient.java`
  Loads `libcontract_spike_atom.so` with JNA, passes testcase JSON directly, and parses Spike atom JSON back into `RISCVObservation` values.

- `src/main/java/contractgen/Main.java`
  Adds/extends `compare_spike_rvfi_atoms`, which runs Spike atom extraction and the existing IBEX_TEST RVFI extractor side by side and emits one JSON comparison report.

- `src/main/java/contractgen/riscv/isa/tests/RISCVTestCaseIO.java`
  Adds testcase JSON serialization helpers so generated in-memory testcases can be passed to Spike without writing `.dat` instruction-memory files.

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

A nonzero exit status means at least one testcase had an execution/extraction error.
Successful atom mismatches against an external oracle are not checked by Spike itself.

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

Spike memory remains mapped at `0x1000`, but the IBEX harness fetches from an instruction-memory view based at `0x80`. After each step, branch/jump targets in the harness range are translated from `0x80 + offset` to `0x1000 + offset`. This is a compatibility shim for the current IBEX_TEST harness, not a general RISC-V platform model.

The Java command can either consume an existing testcase file:

```sh
mvn -q exec:java -Dexec.mainClass=contractgen.Main \
  -Dexec.args='compare_spike_rvfi_atoms -i BASE,M -c BASE,ALIGNED,BRANCH,DEPENDENCIES -t 4 -e 12000-IBEX-testcases.json -o spike-rvfi-compare.json'
```

or generate tests when `-e/--testcases` is omitted:

```sh
mvn -q exec:java -Dexec.mainClass=contractgen.Main \
  -Dexec.args='compare_spike_rvfi_atoms -i BASE,M -c BASE,ALIGNED,BRANCH,DEPENDENCIES -t 4 -n 1000 -s 1 -o spike-rvfi-compare.json'
```

Generated or loaded testcases are sent to Spike in fixed internal chunks of 1000 cases. This is intentionally not a CLI option.

## Assumptions

This implementation is intentionally calibrated to the current contractgen/IBEX testcase workflow.

- Testcases are RV32.
- Register initialization follows contractgen's `ADDI xN, x0, imm` behavior, including 12-bit signed immediate effects.
- Root oracle files stay outside the Spike clone.
- The tool reports the `BASE,ALIGNED,BRANCH,DEPENDENCIES` atom groups:
  `FORMAT`, `OPCODE`, `FUNCT3`, `FUNCT7`, `RD`, `RS1`, `RS2`, `IMM`,
  `REG_RS1`, `REG_RS2`, `REG_RD`, `MEM_ADDR`, `MEM_R_DATA`, `MEM_W_DATA`,
  `IS_ALIGNED`, `IS_HALF_ALIGNED`, `IS_BRANCH`, `BRANCH_TAKEN`, `NEW_PC`,
  `RAW_RS1_1` through `RAW_RS1_4`, `RAW_RS2_1` through `RAW_RS2_4`, and
  `WAW_1` through `WAW_4`.
- Branch target execution follows the harness-style finite instruction image: out-of-image instruction fetches are modeled as NOPs.
- If a control-flow observation diverges through taken-ness or next-PC, comparison stops after the control sample. This matches most observed IBEX_TEST oracle behavior and avoids treating later synthetic NOP padding as the primary atom source.
- Loads and stores are modeled for atom observation compatibility with the IBEX harness, not as general Spike memory semantics. The custom IBEX `data_mem.sv` returns address-derived read data (`addr % 0x1000`) and exposes byte-enable-masked RVFI memory data; the tool mirrors that convention.
- Writes to `x0` expose `REG_RD == 0` in the Spike compatibility model. This matches the current oracle better than reporting the computed writeback value for `rd == 0`: changing Spike to report computed `x0` writeback values increased the saved 12k BASE mismatch count from 8 to 101, mostly extra `ADDI/REG_RD`.
- `IS_ALIGNED` is computed as `mem_addr[1:0] == 0`; `IS_HALF_ALIGNED` is computed as `mem_addr[1:0] != 3`, matching the Ibex `ctr.sv` helper signals.
- Branch observations are computed like the Ibex `ctr.sv` helper signals: JAL and JALR are control instructions and are always branch-taken; conditional branch taken-ness is recomputed from the sampled source-register values.
- Dependency atoms use a four-retirement window, matching `RVFIExtractor.compareDependencies`: `RAW_RS1_d` and `RAW_RS2_d` compare the current source register against the destination register `d` retirements earlier; `WAW_d` compares the current destination against the earlier destination.

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

For this temporary IBEX_TEST oracle-compatibility mode, Spike currently follows the Java encoding shortcut for structural shift-immediate bits but suppresses runtime `REG_RS2` reads for shift-immediate instructions:

- `SLLI/SRLI/SRAI` are encoded from the testcase `rs2` field because that is where current contractgen stores `shamt`.
- Spike does not read or compare `x[shamt]` as a runtime `REG_RS2` value.
- The long-term architectural cleanup should move these instructions to `rd`, `rs1`, and `imm/shamt` in Java and regenerate the oracle files.

Until contractgen's Java ISA model is corrected and oracle files are regenerated, exact oracle comparisons involving these shift-immediate `RS2` atoms should be interpreted as oracle/model mismatches, not necessarily Spike implementation bugs.

## Validation

The original base-template validation command was:

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

Earlier full-template validation against `12000-IBEX-results.json` showed larger dependency and shift-immediate differences. After adding the Java shared-library command, the current local reference is the saved BASE-only comparison file `spike-rvfi-compare.json` and the 12k testcase file:

```sh
build/contract-spike-diff \
  --testcases ../12000-IBEX-testcases.json \
  --all \
  --json-out /tmp/spike-atoms-12000-fixed.json
```

Result:

```text
spike summary: total 12000, failed 0
BASE-only mismatches against saved spike-rvfi-compare.json: 8 / 12000
```

The remaining BASE mismatch classes in that saved 12k run are:

```text
5 cases where RVFI reports ADDI padding/NOP atoms after control-flow behavior and Spike does not.
3 cases where Spike reports LH/LHU MEM_R_DATA and REG_RD differences for unaligned halfword loads and RVFI does not.
```

The load mismatches are not safely fixed by a simple aligned-default-memory rule: that removes the 3 overreports but introduces 5 new `LH/LHU REG_RD` underreports in the same saved 12k file. For now they are documented as an IBEX_TEST RVFI oracle compatibility gap, not hidden by a heuristic.

The full Java command could not be end-to-end rerun in this local workspace because `IBEXTest` tries to create simulation output under the hardcoded `/home/yosys/output/...` path. The Java code was compile-checked with `javac`; the native Spike CLI/shared library was rebuilt successfully.

## Build Notes

This upstream Spike checkout required `dtc` during configure/build. In this workspace, the working build command was:

```sh
make -C build -j2 contract-spike-diff libcontract_spike_atom.so
```

The built artifacts are:

```text
build/contract-spike-diff
build/libcontract_spike_atom.so
```

## Limitations

This is now a shared-library integration for comparison, but not yet the main synthesis path. The existing RVFI extractor still exists and `compare_spike_rvfi_atoms` deliberately reports both Spike atoms and old RVFI atoms.

The memory and branch edge behavior is currently matched to the IBEX harness oracle. If the next target is a processor-independent ISA-level atom oracle, those conventions should be separated behind a selectable compatibility mode.
