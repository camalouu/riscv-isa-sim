# Contract Spike Atom Replay

This Spike checkout contains a bounded, paired RISC-V atom replay used by
contractgen. It is available as:

- `build/contract-spike-diff`, a standalone JSON command;
- `build/libcontract_spike_atom.so`, the native library used by Java.

The replay consumes contractgen testcase JSON directly. It does not require an
ELF, VCD, RVFI trace, or result JSON.

## Build and invocation

```sh
make -C build -j2 contract-spike-diff libcontract_spike_atom.so

build/contract-spike-diff \
  --testcases ../1000-IBEX-testcases.json \
  --all \
  --json-out /tmp/spike-atoms.json
```

The shared-library entry point is
`contract_spike_atoms_json(testcases, isa, ordinal)`.

## Program and execution model

Each side contains:

1. 31 register-initialization instructions;
2. one NOP gap;
3. the testcase program;
4. NOP padding.

Replay requests `31 + maxInstructionCount` retirements. Spike stores and runs
the synthetic image at `0x1000`. For compatibility with the Ibex instruction
memory, absolute JALR targets in the harness image beginning at `0x80` are
mapped to the corresponding private-image offset. PC-relative branch and JAL
displacements are not remapped: relocating both the instruction and its target
preserves their encoded displacement.

Spike follows architectural control flow without a core-specific fetch-budget
heuristic. Taken transfers may enter the register-loader prologue, re-enter the
testcase, or loop within it. Targets outside the finite replay image read as NOP.
The core-specific end of useful execution is supplied separately by the RTL
harness, keeping the Spike replay usable with Ibex, CVA6, and other cores.

Both sides continue along their own architectural paths after next-PC or
branch-taken divergence. This is required to observe atoms in different
following instructions. It also means dependency atoms in later instructions
can reflect the distinct histories of the two paths.

The replay implements the RV32I/RV32M instruction semantics used by generated
testcases while using Spike as the native instruction-image host. Arbitrary
architectural data addresses do not enter Spike's MMU.

## Memory model

Data memory is sparse, byte-addressed, and private to each side:

- untouched bytes read as zero;
- stores update only bytes selected by the instruction width;
- later loads observe stored bytes;
- byte and halfword loads apply the architectural sign/zero extension;
- the observed read/write bus values follow the Ibex RVFI byte-lane layout.

This policy is deliberately not tied to random address-derived data. It is a
portable deterministic harness model that can be implemented by Ibex, CVA6, or
another core. The RTL memory used for comparison must use the same untouched
byte and store semantics.

## Atom output and retirement metadata

For one case the output is:

```json
{
  "case_index": 0,
  "atoms": [
    {
      "type": "ADDI",
      "observation": "OPCODE",
      "first_retire": 34
    }
  ],
  "instruction_pairs": [
    {
      "left": "JALR",
      "right": "ADDI",
      "first_retire": 36
    }
  ]
}
```

`first_retire` is the earliest absolute, one-based retirement at which that
exact `(type, observation)` atom distinguishes the two executions. It includes
the 31 initialization retirements. If an atom occurs repeatedly, only its
earliest retirement is retained. When the two sides retire different
instruction types, `instruction_pairs` similarly records the ordered type pair
and its earliest retirement. The native response contains compact evidence
metadata only, never a full instruction trace.

Java preserves the ordinary atom-set API for callers that do not need timing.
Cutoff handling is selected by the attacker integration. For integrations using
failure/execution cutoffs, such as `IBEX_TEST`, replay synthesis uses
`first_retire` internally:

- attacker-positive executed case: keep atoms and instruction pairs whose
  `first_retire <= cutoff`;
- attacker-negative executed case: discard structural/control evidence and
  instruction pairs after the RTL's last non-NOP retirement, while retaining
  the possible four-retirement ADDI dependency tail;
- adaptively skipped evidence: retain the existing signature-level behavior,
  because no per-case RTL cutoff exists.

A missing positive cutoff is an error only when the integration requires one;
an attacker-positive result with no evidence remaining after filtering is also
an error. `CV32E40S_TEST` uses `requireFailureCutoff=false` and supplies no
execution cutoff, so it retains the complete bounded Spike atom and
instruction-pair sets for both positive and negative cases. Later path
differences can therefore help cover an earlier timing divergence. Final result
JSON and synthesized-contract formats do not expose `first_retire`; they retain
the existing `distinguishingInstructions` representation for instruction pairs.

## Atom semantics

The replay supports the current contractgen observation groups:

- structural: `FORMAT`, `OPCODE`, `FUNCT3`, `FUNCT7`, `RD`, `RS1`, `RS2`,
  `IMM`;
- values: `REG_RS1`, `REG_RS2`, `REG_RD`, `MEM_ADDR`, `MEM_R_DATA`,
  `MEM_W_DATA`, their `ZERO`/`LOG2` register variants, `REG_RS2_LOW5`, and
  alignment atoms;
- control: `IS_BRANCH`, `BRANCH_TAKEN`, `NEW_PC`;
- dependencies: `RAW_RS1_1..4`, `RAW_RS2_1..4`, and `WAW_1..4`.

Dependency distance is measured in dynamic retirements on each side. At
distance `d`, RAW compares the current source register with the destination
retired `d` steps earlier, and WAW compares current and prior destinations.
Consequently, a control-flow divergence can legitimately create later
dependency distinctions even if both paths eventually execute NOPs.

ZERO observations identify zero versus nonzero. LOG2 observations compare
`floor(log2(unsigned_value))`, with zero in a separate sentinel bucket.
`REG_RS2_LOW5` compares `rs2_value[4:0]`, modeling operand-dependent register
shift latency without exposing the rest of the source value.
`IS_ALIGNED` means `address[1:0] == 0`; `IS_HALF_ALIGNED` means
`address[1:0] != 3`, matching the current Ibex extractor.

JAL and JALR are always taken control instructions. Conditional branch
taken-ness is evaluated from each side's sampled source values, and `NEW_PC`
uses the architectural target address.

### Dependency atoms, x0, and hardware hazards

`compare_dependency` implements **syntactic register-number equality**. For
each side, it checks whether the current source (or destination for WAW) and
the earlier destination fields exist, then compares their decoded numbers.
Register `x0` participates in this comparison exactly like any other number.
This matches the equality convention in contractgen's
[RVFI extractor](../src/main/java/contractgen/riscv/isa/extractor/RVFIExtractor.java)
and the dependency signals in the
[Ibex contract monitor](../src/main/resources/ibex/verif/ctr.sv).

The comparison has the following consequences:

- `ADDI x0,x0,0` (NOP) has decoded `rd=x0` and `rs1=x0`. Its destination
  contributes to subsequent RAW/WAW comparisons, including in the initialization
  sequence, spacer, and padding.
- A syntactic match with `x0` does not imply an architectural write or a hardware
  hazard. Execution still discards writes to `x0` and reads it as zero.
- Each distance is compared independently. The extractor does not check for
  an intervening overwrite, the most recent producer, forwarding, or a pipeline
  stall. The suffix is a number of dynamic retirements, not a number of cycles.
- If the required fields exist on both sides, unequal equality predicates emit
  the atom for both current instruction types. Equal predicates emit nothing,
  including when both predicates are true. If the fields exist on only one
  side, the atom is emitted for that side's current type even if its register
  numbers do not match; if neither side has the fields, nothing is emitted.

For example, suppose `x18=0`, the preceding three instructions are NOPs, and
the target at `0x200` contains only NOPs:

```asm
# Left                         # Right
andi x0, x0, -4                andi x18, x18, -4
jalr x1, x0, 512               jalr x1, x18, 512
```

At JALR, the distance-one predicates are `0 == 0` and `18 == 18`. Both are
true, so `JALR: RAW_RS1_1` does not distinguish. At distances two through four,
the prior destinations are zero on both sides: `0 == 0` differs from
`18 == 0`, so `JALR: RAW_RS1_2`, `_3`, and `_4` distinguish. These are the
defined contract observations even though only the right execution has a real
adjacent producer/consumer dependency.

CV32E40S's
[JALR bypass logic](../src/main/resources/cv32e40s-test/core/rtl/cv32e40s_controller_bypass.sv)
excludes `rs1=x0` from its hazard matches and stalls JALR in ID when a matching
producer is in EX. Its ALU result can be forwarded from WB after an intervening
NOP. The `dataindtiming` setting does not gate this JALR stall condition.
Focused replays with the existing native Spike and CV32E40S libraries produced:

| Pair (all JALR targets equal) | Distinguishing JALR RAW_RS1 distances | RTL timing off / on |
| --- | --- | --- |
| `x0` versus `x18` example above | 2, 3, 4 | distinguishable / distinguishable |
| Same pair, one NOP inserted before both JALRs | 1, 3, 4 | equivalent / equivalent |
| Replace both ANDIs with NOP | 1, 2, 3, 4 | equivalent / equivalent |
| Left `ANDI x16,x16,-4; JALR x1,x17,512`, right `ANDI x18,x18,-4; JALR x1,x18,512`, all registers zero | 1 | distinguishable / distinguishable |
| Previous pair with one intervening NOP on both sides | 2 | equivalent / equivalent |

An atom can therefore distinguish when the attacker cannot, and a hardware
RAW hazard can exist without a distinguishing distance-one RAW atom. This is
a limitation of interpreting the current contract vocabulary as a description
of physical hazards, rather than evidence of an incorrect Spike implementation
of that vocabulary. Excluding `x0` would change contract semantics and would
need coordinated changes to the contract monitor, extractors, validation, and
affected result sets.

### Interpreting the CV32E40S seed-31 result

The 10,000-case timing-on run in
`../results/cv32e40s_test-10000-seed31/` illustrates this distinction. Replaying
the three attacker-positive cases reproduced their saved Spike atom and
instruction-pair sets. Thirteen original/controlled pairs were also checked
in both RTL timing modes; the classification of every pair was the same with
timing off and on.

- **8767:** the `x0` versus `x18` adjacent-producer case. Spike reports
  `JALR: RAW_RS1_2`, `_3`, and `_4` at retirement 34, but not `_1`.
  Inserting one NOP removes attacker distinguishability while `_4` remains
  distinguishing in Spike. The target is identical on both sides.
- **9701:** `x0` versus `x21` with an adjacent ANDI and different targets.
  Spike reports `JALR: RAW_RS1_2` and `_4` at retirement 34. Equalizing the
  targets to `0x170` retains attacker distinguishability; inserting one NOP
  removes it even with the original different targets. These controls isolate
  the adjacent JALR hazard in the tested pair.
- **3450:** both sides use the same `ANDI x22,x22,-4; JALR x10,x22,imm`
  dependency pattern, but jump to `0x3d0` and `0x0f8`. The latter path re-enters
  the testcase through two NOPs, then ANDI and JALR. Spike records
  `(ADDI,ANDI)` at retirement 37 and `(ADDI,JALR)` at retirement 38. There is
  no `JALR: RAW_RS1_4` atom in this case. Changing the second target to a
  NOP-only region (`0x300`) removes attacker distinguishability.

The synthesized singleton `JALR: RAW_RS1_4` covers 8767 and 9701 by atom
membership. It covers 3450 through the `(ADDI,JALR)` instruction pair: the
contract observes JALR but has no corresponding observation for ADDI, which
[Contract.covers](../src/main/java/contractgen/Contract.java) treats as
distinguishing. The selected singleton has zero false positives on the original
10,000 cases, whereas `JALR: RAW_RS1_2` covers the same three positives with 24
false positives. This explains the optimizer's choice without attributing a
four-instruction hardware hazard to it. The NOP variant of 8767 would be a
false positive for that original singleton, demonstrating its limited
generalization beyond the synthesis samples.

### Shift-immediate caveat

Contractgen currently represents `SLLI`, `SRLI`, and `SRAI` with an R-type
encoding shortcut: the shift amount is stored in the Java `rs2` field. Replay
uses that field to encode instruction bits `[24:20]`, but it does not read
`x[shamt]` or emit runtime `REG_RS2`/`RAW_RS2` behavior for it. Architecturally
the operand is an immediate, not a source register. A future Java ISA cleanup
should model it as `shamt`/`IMM` and regenerate oracle artifacts.

## Comparison workflow

For a focused oracle comparison:

```sh
mvn -q exec:java -Dexec.mainClass=contractgen.Main \
  -Dexec.args='compare_spike_rvfi_atoms -i BASE,M -c BASE,ALIGNED,BRANCH,DEPENDENCIES -t 4 -e 1000-IBEX-testcases.json -o spike-rvfi-compare.json'
```

For synthesis replay, Spike runs once and Java applies the integration's cutoff
policy described above. The IBEX_TEST attacker is invoked once per executed
testcase. An attacker-positive invocation internally reruns fresh native models
with decreasing fetch bounds to reproduce the old Ibex/RVFI prefix minimization.
Its cutoff is the
final paired retirement count of the smallest failing run; no VCD is written or
parsed. Add `--disable-adaptive-skipping` to execute the RTL attacker for every
testcase.

After changing the native replay or Ibex harness, rebuild both native
libraries before comparing results. No Dockerfile or Docker-image change is
required solely for these source changes.

## Files

- `spike_main/contract-spike-diff.cc`: JSON parsing, instruction encoding,
  bounded execution, atom extraction, CLI, and C ABI.
- `spike_main/spike_main.mk.in`: native binary/shared-library build targets.
- `../src/main/java/contractgen/riscv/isa/spike/SpikeAtomClient.java`: JNA
  client and timed-atom parsing.
- `../src/main/java/contractgen/riscv/isa/spike/SpikeAtomParallelRunner.java`:
  chunked, process-isolated replay.
