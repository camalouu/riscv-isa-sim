// Compile against the adapted Spike checkout; assertions inspect architectural samples.
#define main contract_spike_cli_main
#include "contract-spike-diff.cc"
#undef main
#include <cassert>

int main() {
  spike_runner reference("RV32IM", execution_model::rv32_v1);
  auto run = [&](std::initializer_list<uint32_t> program, int count = -1) {
    std::vector<uint32_t> words(2048, NOP);
    std::copy(program.begin(), program.end(), words.begin() + 32);
    return reference.run_side(words, count < 0 ? 32 + program.size() : count);
  };
  auto a = run({0x00000097, 0x0080016f, NOP, 0x000101e7}, 36);
  assert(a[32].reg_rd == 0x100); // AUIPC uses architectural PC
  assert(a[33].reg_rd == 0x108 && a[33].next_pc == 0x10c);
  assert(a[34].reg_rd == 0x110 && a[34].next_pc == 0x108); // absolute JALR
  auto b = run({encode_addi(1,0,-128), 0x00101123, 0x00201103, 0x00205183});
  assert(uint32_t(b[34].reg_rd) == 0xffffff80u && b[35].reg_rd == 0xff80);
  auto c = run({encode_addi(1,0,127), 0x001001a3, 0x00304103});
  assert(c[34].reg_rd == 127); // lane three
  std::vector<uint32_t> words(2048, NOP);
  words[32] = encode_addi(1,0,123);
  for (int i = 0; i < 40; ++i) words[33+i] = encode(program_insn{"SB", {}, 0, 1, i});
  words[73] = 0x00004103;
  auto d = reference.run_side(words, 74);
  assert(d.back().reg_rd == 123); // no 32-byte FIFO eviction
  auto e = run({0x00002103});
  assert(e.back().reg_rd == 0); // fresh testcase data
  // Odd halfwords, every word offset, page splits and RV32 address wrap use
  // Spike's actual load/store handlers. The observation lanes wrap modulo four.
  for (uint32_t address : {1u,2u,3u,4093u,4094u,4095u,0xfffffffdu,0xfffffffeu,0xffffffffu}) {
    std::vector<uint32_t> image(2048,NOP);
    image[32]=encode(program_insn{"LUI",1,{},{},uint32_t(address+0x800)&0xfffff000u});
    image[33]=encode_addi(1,1,int32_t(sext(address&4095,12)));
    image[34]=encode(program_insn{"LUI",2,{},{},0x80ff8000u});
    image[35]=encode_addi(2,2,-511); // 0x80ff7e01
    image[36]=encode(program_insn{"SW",{},1,2,0});
    image[37]=encode(program_insn{"LW",3,1,{},0});
    image[38]=encode(program_insn{"LH",4,1,{},1});
    image[39]=encode(program_insn{"LHU",5,1,{},1});
    image[40]=encode(program_insn{"SH",{},1,2,1});
    image[41]=encode(program_insn{"LW",6,1,{},0});
    auto trace=reference.run_side(image,42);
    assert(uint32_t(trace[37].reg_rd)==0x80ff7e01u);
    assert(uint32_t(trace[38].reg_rd)==0xffffff7eu && trace[39].reg_rd==0xff7eu);
    assert(uint32_t(trace[41].reg_rd)==0x807e0101u);
    uint32_t expected=0;
    for(unsigned byte=0;byte<4;++byte) expected|=((0x80ff7e01u>>(8*byte))&255)<<(8*((address+byte)&3));
    assert(trace[37].mem_r_data==expected);
    assert(reference.architectural->requires_misaligned_data_access);
  }
  run({0x00002103});
  assert(!reference.architectural->requires_misaligned_data_access);
  test_case requirement_case;
  requirement_case.max_instruction_count=2;
  requirement_case.program1={{"LH",1,0,{},1}};
  assert(run_case(requirement_case,reference).requires_misaligned_data_access);
  requirement_case.program1.clear();requirement_case.program2={{"LH",1,0,{},1}};
  auto required=run_case(requirement_case,reference);
  assert(!required.error && required.requires_misaligned_data_access);
  // Unexecuted instructions must not affect eligibility.
  requirement_case.program2={{"JAL",0,{},{},8},{"LH",1,0,{},1}};
  requirement_case.max_instruction_count=4;
  assert(!run_case(requirement_case,reference).requires_misaligned_data_access);
  bool jump = false;
  try { run({0x0020006f}); } catch (const std::runtime_error&) { jump = true; }
  assert(jump);
  // Stores at the instruction address cannot overwrite the fetch image.
  auto f = run({encode_addi(1,0,0x108), 0x0000a023, encode_addi(2,0,7)});
  assert(f.back().reg_rd == 7);
  // Out-of-image NOPs must wrap at XLEN and eventually reenter the image.
  auto wrap = run({encode_addi(1,0,-4), 0x00008067}, 100);
  assert(wrap[34].pc == 0xfffffffcu && wrap[34].next_pc == 0);
  assert(wrap[35].pc == 0 && wrap[67].pc == 0x80);
  // Fetch/data pages overlap in address, but remain architecturally separate.
  auto high = run({encode_addi(1,0,-4),encode_addi(2,0,-17),0x0020a023,0x0000a183});
  assert(uint32_t(high.back().reg_rd) == uint32_t(-17));
  // I-cache and register state must not survive a new testcase.
  assert(run({encode_addi(1,0,9)}).back().reg_rd == 9);
  assert(run({encode_addi(1,0,11)}).back().reg_rd == 11);
  assert(run({encode_addi(2,1,0)}).back().reg_rd == 0);
  spike_runner base_only("rv32i", execution_model::rv32_v1);
  auto mul_words = words_for({}, {{"MUL",3,1,2,{}}}, 2, execution_model::rv32_v1);
  bool disabled_m = false;
  try { base_only.run_side(mul_words, 34); } catch (const std::runtime_error&) { disabled_m = true; }
  assert(disabled_m);
  bool unknown = false;
  try { parse_execution_model("processor-specific"); } catch (const std::runtime_error&) { unknown = true; }
  assert(unknown);
  // Decode/execute all supported RV32M corner cases through Spike.
  auto arithmetic = run({0x800000b7,encode_addi(2,0,-1),0x0220c1b3,0x0220e233,
                         encode_addi(2,0,0),0x0220c2b3,0x0220e333});
  assert(uint32_t(arithmetic[34].reg_rd) == 0x80000000u);
  assert(arithmetic[35].reg_rd == 0);
  assert(uint32_t(arithmetic[37].reg_rd) == 0xffffffffu);
  assert(uint32_t(arithmetic[38].reg_rd) == 0x80000000u);
  // Every writer uses the same reference and records its provenance.
  std::ostringstream single, batch, default_output;
  case_result result;
  write_result(single, result, execution_model::rv32_v1);
  write_results(batch, {result}, execution_model::rv32_v1);
  write_results(default_output, {result});
  assert(single.str().find("\"execution_model\":\"rv32-v1\"") != std::string::npos);
  assert(batch.str().find("\"execution_model\":\"rv32-v1\"") != std::string::npos);
  assert(default_output.str().find("execution_model") != std::string::npos);
  // Invalid imports must return a native error, including fields which the old
  // parser dereferenced without checking and integers which previously wrapped.
  for (const std::string input : {
      "[{}]",
      "[{\"registers1\":{},\"registers2\":{},\"program1\":[{}],\"program2\":[]}]",
      "[{\"maxInstructionCount\":4294967296}]",
      "[{\"maxInstructionCount\":2.5}]",
      "[{\"maxInstructionCount\":1e100}]",
      "[{\"registers1\":{\"1oops\":0}}]",
      "[{\"index\":0,\"index\":1}]"}) {
    char* response = contract_spike_atoms_json_v2(input.c_str(), "RV32IM", -1, "rv32-v1");
    assert(response && std::strstr(response, "\"error\""));
    contract_spike_free(response);
  }
  char* rejected = contract_spike_atoms_json_v2("[]", "RV32IM", -1, "legacy-v1");
  assert(rejected && std::strstr(rejected,"legacy"));
  contract_spike_free(rejected);
  char* canonical = contract_spike_atoms_json("[]", "RV32IM", -1);
  assert(canonical && std::string(canonical).find("rv32-v1") != std::string::npos);
  contract_spike_free(canonical);
  char* valid = contract_spike_atoms_json_v2("[]", "RV32IM", -1, "rv32-v1");
  assert(valid && !std::strstr(valid, "\"error\""));
  contract_spike_free(valid);
  std::cout << "Shared RV32 execution model architectural checks passed\n";
}
