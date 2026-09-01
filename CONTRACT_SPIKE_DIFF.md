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
During replay synthesis it uses `first_retire` internally:

- attacker-positive executed case: keep atoms and instruction pairs whose
  `first_retire <= cutoff`;
- attacker-negative executed case: discard structural/control evidence and
  instruction pairs after the RTL's last non-NOP retirement, while retaining
  the possible four-retirement ADDI dependency tail;
- adaptively skipped evidence: retain the existing signature-level behavior,
  because no per-case RTL cutoff exists.

An attacker-positive result without a cutoff, or with no evidence remaining
after filtering, is an error. Final result JSON and synthesized-contract
formats do not expose `first_retire`; they retain the existing
`distinguishingInstructions` representation for instruction pairs.

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

For synthesis replay, Spike runs once and Java performs cutoff filtering. The
IBEX_TEST attacker is invoked once per executed testcase, but an attacker-
positive invocation internally reruns fresh native models with decreasing fetch
bounds to reproduce the old Ibex/RVFI prefix minimization. Its cutoff is the
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
