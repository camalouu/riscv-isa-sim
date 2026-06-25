// See LICENSE for license details.

#include "config.h"
#include "cfg.h"
#include "devices.h"
#include "mmu.h"
#include "processor.h"
#include "sim.h"
#include "trap.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr reg_t TEXT_BASE = 0x1000;
constexpr reg_t HARNESS_TEXT_BASE = 0x80;
constexpr size_t MEM_SIZE = 16 * 1024 * 1024;
constexpr int INIT_COUNT = 31;
constexpr uint32_t NOP = 0x00000013;
constexpr size_t DATA_MEM_HISTORY = 32;

struct json_value {
  using object_t = std::map<std::string, json_value>;
  using array_t = std::vector<json_value>;
  std::variant<std::nullptr_t, bool, double, std::string, object_t, array_t> value;

  const object_t& object() const { return std::get<object_t>(value); }
  const array_t& array() const { return std::get<array_t>(value); }
  const std::string& string() const { return std::get<std::string>(value); }
  int64_t integer() const { return static_cast<int64_t>(std::get<double>(value)); }
  bool is_null() const { return std::holds_alternative<std::nullptr_t>(value); }
};

class json_parser {
public:
  explicit json_parser(std::string input) : input(std::move(input)) {}

  json_value parse()
  {
    auto v = parse_value();
    skip_ws();
    if (pos != input.size())
      throw std::runtime_error("Trailing data in JSON input");
    return v;
  }

private:
  json_value parse_value()
  {
    skip_ws();
    if (pos >= input.size())
      throw std::runtime_error("Unexpected end of JSON input");
    const char c = input[pos];
    if (c == '{') return json_value{parse_object()};
    if (c == '[') return json_value{parse_array()};
    if (c == '"') return json_value{parse_string()};
    if (c == 't') return parse_literal("true", json_value{true});
    if (c == 'f') return parse_literal("false", json_value{false});
    if (c == 'n') return parse_literal("null", json_value{nullptr});
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return json_value{parse_number()};
    throw std::runtime_error("Unexpected JSON token");
  }

  json_value parse_literal(const char* literal, json_value value)
  {
    const std::string s(literal);
    if (input.compare(pos, s.size(), s) != 0)
      throw std::runtime_error("Invalid JSON literal");
    pos += s.size();
    return value;
  }

  json_value::object_t parse_object()
  {
    expect('{');
    json_value::object_t out;
    skip_ws();
    if (consume('}')) return out;
    while (true) {
      skip_ws();
      std::string key = parse_string();
      skip_ws();
      expect(':');
      out.emplace(std::move(key), parse_value());
      skip_ws();
      if (consume('}')) return out;
      expect(',');
    }
  }

  json_value::array_t parse_array()
  {
    expect('[');
    json_value::array_t out;
    skip_ws();
    if (consume(']')) return out;
    while (true) {
      out.push_back(parse_value());
      skip_ws();
      if (consume(']')) return out;
      expect(',');
    }
  }

  std::string parse_string()
  {
    expect('"');
    std::string out;
    while (pos < input.size()) {
      char c = input[pos++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos >= input.size())
        throw std::runtime_error("Invalid JSON escape");
      const char esc = input[pos++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u':
          if (pos + 4 > input.size())
            throw std::runtime_error("Invalid JSON unicode escape");
          out.push_back('?');
          pos += 4;
          break;
        default:
          throw std::runtime_error("Invalid JSON escape");
      }
    }
    throw std::runtime_error("Unterminated JSON string");
  }

  double parse_number()
  {
    const size_t start = pos;
    if (input[pos] == '-') pos++;
    while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) pos++;
    if (pos < input.size() && input[pos] == '.') {
      pos++;
      while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) pos++;
    }
    if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
      pos++;
      if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) pos++;
      while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) pos++;
    }
    return std::stod(input.substr(start, pos - start));
  }

  void skip_ws()
  {
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) pos++;
  }

  bool consume(char c)
  {
    if (pos < input.size() && input[pos] == c) {
      pos++;
      return true;
    }
    return false;
  }

  void expect(char c)
  {
    skip_ws();
    if (!consume(c))
      throw std::runtime_error(std::string("Expected JSON token ") + c);
  }

  std::string input;
  size_t pos = 0;
};

enum class format_t { r, i, s, b, u, j };

struct insn_def {
  const char* name;
  format_t format;
  uint32_t opcode;
  std::optional<uint32_t> funct3;
  std::optional<uint32_t> funct7;
};

const std::vector<insn_def>& insn_defs()
{
  static const std::vector<insn_def> defs = {
    {"LUI", format_t::u, 0x37, {}, {}},
    {"AUIPC", format_t::u, 0x17, {}, {}},
    {"JAL", format_t::j, 0x6f, {}, {}},
    {"JALR", format_t::i, 0x67, 0x0, {}},
    {"BEQ", format_t::b, 0x63, 0x0, {}},
    {"BNE", format_t::b, 0x63, 0x1, {}},
    {"BLT", format_t::b, 0x63, 0x4, {}},
    {"BGE", format_t::b, 0x63, 0x5, {}},
    {"BLTU", format_t::b, 0x63, 0x6, {}},
    {"BGEU", format_t::b, 0x63, 0x7, {}},
    {"LB", format_t::i, 0x03, 0x0, {}},
    {"LH", format_t::i, 0x03, 0x1, {}},
    {"LW", format_t::i, 0x03, 0x2, {}},
    {"LBU", format_t::i, 0x03, 0x4, {}},
    {"LHU", format_t::i, 0x03, 0x5, {}},
    {"SB", format_t::s, 0x23, 0x0, {}},
    {"SH", format_t::s, 0x23, 0x1, {}},
    {"SW", format_t::s, 0x23, 0x2, {}},
    {"ADDI", format_t::i, 0x13, 0x0, {}},
    {"SLTI", format_t::i, 0x13, 0x2, {}},
    {"SLTIU", format_t::i, 0x13, 0x3, {}},
    {"XORI", format_t::i, 0x13, 0x4, {}},
    {"ORI", format_t::i, 0x13, 0x6, {}},
    {"ANDI", format_t::i, 0x13, 0x7, {}},
    {"SLLI", format_t::r, 0x13, 0x1, 0x00},
    {"SRLI", format_t::r, 0x13, 0x5, 0x00},
    {"SRAI", format_t::r, 0x13, 0x5, 0x20},
    {"ADD", format_t::r, 0x33, 0x0, 0x00},
    {"SUB", format_t::r, 0x33, 0x0, 0x20},
    {"SLL", format_t::r, 0x33, 0x1, 0x00},
    {"SLT", format_t::r, 0x33, 0x2, 0x00},
    {"SLTU", format_t::r, 0x33, 0x3, 0x00},
    {"XOR", format_t::r, 0x33, 0x4, 0x00},
    {"SRL", format_t::r, 0x33, 0x5, 0x00},
    {"SRA", format_t::r, 0x33, 0x5, 0x20},
    {"OR", format_t::r, 0x33, 0x6, 0x00},
    {"AND", format_t::r, 0x33, 0x7, 0x00},
    {"MUL", format_t::r, 0x33, 0x0, 0x01},
    {"MULH", format_t::r, 0x33, 0x1, 0x01},
    {"MULHSU", format_t::r, 0x33, 0x2, 0x01},
    {"MULHU", format_t::r, 0x33, 0x3, 0x01},
    {"DIV", format_t::r, 0x33, 0x4, 0x01},
    {"DIVU", format_t::r, 0x33, 0x5, 0x01},
    {"REM", format_t::r, 0x33, 0x6, 0x01},
    {"REMU", format_t::r, 0x33, 0x7, 0x01},
  };
  return defs;
}

const insn_def& def_by_name(const std::string& name)
{
  for (const auto& def : insn_defs())
    if (name == def.name)
      return def;
  throw std::runtime_error("Unsupported instruction type " + name);
}

const insn_def* decode_def(uint32_t bits)
{
  const uint32_t opcode = bits & 0x7f;
  const uint32_t funct3 = (bits >> 12) & 0x7;
  const uint32_t funct7 = (bits >> 25) & 0x7f;
  for (const auto& def : insn_defs()) {
    if (def.opcode != opcode) continue;
    if (def.funct3 && *def.funct3 != funct3) continue;
    if (def.funct7 && *def.funct7 != funct7) continue;
    return &def;
  }
  return nullptr;
}

struct program_insn {
  std::string type;
  std::optional<int> rd;
  std::optional<int> rs1;
  std::optional<int> rs2;
  std::optional<int64_t> imm;
};

struct test_case {
  int index = 0;
  int max_instruction_count = 0;
  std::map<int, std::optional<int64_t>> registers1;
  std::map<int, std::optional<int64_t>> registers2;
  std::vector<program_insn> program1;
  std::vector<program_insn> program2;
};

const json_value* find_field(const json_value::object_t& obj, const std::string& key)
{
  auto it = obj.find(key);
  return it == obj.end() ? nullptr : &it->second;
}

int64_t int_field(const json_value::object_t& obj, const std::string& key, int64_t fallback = 0)
{
  const auto* field = find_field(obj, key);
  return field ? field->integer() : fallback;
}

std::optional<int> opt_int_field(const json_value::object_t& obj, const std::string& key)
{
  const auto* field = find_field(obj, key);
  if (!field || field->is_null()) return std::nullopt;
  return static_cast<int>(field->integer());
}

std::optional<int64_t> opt_i64_field(const json_value::object_t& obj, const std::string& key)
{
  const auto* field = find_field(obj, key);
  if (!field || field->is_null()) return std::nullopt;
  return field->integer();
}

std::map<int, std::optional<int64_t>> parse_registers(const json_value& value)
{
  std::map<int, std::optional<int64_t>> out;
  for (const auto& entry : value.object()) {
    int reg = std::stoi(entry.first);
    if (entry.second.is_null())
      out[reg] = std::nullopt;
    else
      out[reg] = entry.second.integer();
  }
  return out;
}

std::vector<program_insn> parse_program(const json_value& value)
{
  std::vector<program_insn> out;
  for (const auto& raw : value.array()) {
    const auto& obj = raw.object();
    program_insn insn;
    insn.type = find_field(obj, "type")->string();
    insn.rd = opt_int_field(obj, "rd");
    insn.rs1 = opt_int_field(obj, "rs1");
    insn.rs2 = opt_int_field(obj, "rs2");
    insn.imm = opt_i64_field(obj, "imm");
    out.push_back(std::move(insn));
  }
  return out;
}

std::vector<test_case> parse_testcases_json(const std::string& json)
{
  json_parser parser(json);
  const auto root = parser.parse();
  std::vector<test_case> out;
  for (const auto& raw : root.array()) {
    const auto& obj = raw.object();
    test_case tc;
    tc.index = static_cast<int>(int_field(obj, "index"));
    tc.max_instruction_count = static_cast<int>(int_field(obj, "maxInstructionCount"));
    tc.registers1 = parse_registers(*find_field(obj, "registers1"));
    tc.registers2 = parse_registers(*find_field(obj, "registers2"));
    tc.program1 = parse_program(*find_field(obj, "program1"));
    tc.program2 = parse_program(*find_field(obj, "program2"));
    out.push_back(std::move(tc));
  }
  return out;
}

std::vector<test_case> parse_testcases(const std::string& path)
{
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("Could not open " + path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return parse_testcases_json(buffer.str());
}

uint32_t imm_bits(std::optional<int64_t> imm, unsigned bits)
{
  if (!imm) throw std::runtime_error("Missing immediate field");
  return static_cast<uint32_t>(*imm) & ((1u << bits) - 1u);
}

uint32_t encode(const program_insn& insn)
{
  const auto& def = def_by_name(insn.type);
  const uint32_t rd = insn.rd.value_or(0) & 0x1f;
  const uint32_t rs1 = insn.rs1.value_or(0) & 0x1f;
  const uint32_t rs2 = insn.rs2.value_or(0) & 0x1f;
  switch (def.format) {
    case format_t::r:
      return ((*def.funct7 & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) |
             ((*def.funct3 & 0x7) << 12) | (rd << 7) | def.opcode;
    case format_t::i: {
      const uint32_t imm = imm_bits(insn.imm, 12);
      return (imm << 20) | (rs1 << 15) | ((*def.funct3 & 0x7) << 12) |
             (rd << 7) | def.opcode;
    }
    case format_t::s: {
      const uint32_t imm = imm_bits(insn.imm, 12);
      return ((imm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) |
             ((*def.funct3 & 0x7) << 12) | ((imm & 0x1f) << 7) | def.opcode;
    }
    case format_t::b: {
      const uint32_t imm = imm_bits(insn.imm, 13);
      return (((imm >> 12) & 0x1) << 31) | (((imm >> 5) & 0x3f) << 25) |
             (rs2 << 20) | (rs1 << 15) | ((*def.funct3 & 0x7) << 12) |
             (((imm >> 1) & 0xf) << 8) | (((imm >> 11) & 0x1) << 7) |
             def.opcode;
    }
    case format_t::u: {
      const uint32_t imm = static_cast<uint32_t>(insn.imm.value_or(0));
      return (imm & 0xfffff000u) | (rd << 7) | def.opcode;
    }
    case format_t::j: {
      const uint32_t imm = imm_bits(insn.imm, 21);
      return (((imm >> 20) & 0x1) << 31) | (((imm >> 1) & 0x3ff) << 21) |
             (((imm >> 11) & 0x1) << 20) | (((imm >> 12) & 0xff) << 12) |
             (rd << 7) | def.opcode;
    }
  }
  abort();
}

uint32_t encode_addi(int rd, int rs1, int64_t imm)
{
  return ((static_cast<uint32_t>(imm) & 0xfff) << 20) |
         ((static_cast<uint32_t>(rs1) & 0x1f) << 15) |
         (0x0 << 12) |
         ((static_cast<uint32_t>(rd) & 0x1f) << 7) |
         0x13;
}

std::vector<uint32_t> words_for(const std::map<int, std::optional<int64_t>>& regs,
                                const std::vector<program_insn>& program,
                                int max_instruction_count)
{
  std::vector<uint32_t> words;
  for (int reg = 1; reg < 32; reg++) {
    auto it = regs.find(reg);
    if (it != regs.end() && it->second)
      words.push_back(encode_addi(reg, 0, *it->second));
    else
      words.push_back(NOP);
  }
  words.push_back(NOP);
  for (const auto& insn : program)
    words.push_back(encode(insn));
  while (static_cast<int>(words.size()) < INIT_COUNT + 1 + max_instruction_count + 16)
    words.push_back(NOP);
  return words;
}

struct decoded_insn {
  std::string type;
  format_t format;
  uint32_t opcode = 0;
  std::optional<uint32_t> funct3;
  std::optional<uint32_t> funct7;
  std::optional<uint32_t> rd;
  std::optional<uint32_t> rs1;
  std::optional<uint32_t> rs2;
  std::optional<uint32_t> imm;
};

decoded_insn decode(uint32_t bits)
{
  const auto* def = decode_def(bits);
  if (!def)
    throw std::runtime_error("Unsupported executed instruction");
  decoded_insn out;
  out.type = def->name;
  out.format = def->format;
  out.opcode = def->opcode;
  out.funct3 = def->funct3;
  out.funct7 = def->funct7;
  switch (def->format) {
    case format_t::r:
      out.rd = (bits >> 7) & 0x1f;
      out.rs1 = (bits >> 15) & 0x1f;
      out.rs2 = (bits >> 20) & 0x1f;
      break;
    case format_t::i:
      out.rd = (bits >> 7) & 0x1f;
      out.rs1 = (bits >> 15) & 0x1f;
      out.imm = (bits >> 20) & 0xfff;
      break;
    case format_t::s:
      out.rs1 = (bits >> 15) & 0x1f;
      out.rs2 = (bits >> 20) & 0x1f;
      out.imm = ((bits >> 7) & 0x1f) | (((bits >> 25) & 0x7f) << 5);
      break;
    case format_t::b:
      out.rs1 = (bits >> 15) & 0x1f;
      out.rs2 = (bits >> 20) & 0x1f;
      out.imm = (((bits >> 31) & 0x1) << 12) | (((bits >> 7) & 0x1) << 11) |
                (((bits >> 25) & 0x3f) << 5) | (((bits >> 8) & 0xf) << 1);
      break;
    case format_t::u:
      out.rd = (bits >> 7) & 0x1f;
      out.imm = bits & 0xfffff000u;
      break;
    case format_t::j:
      out.rd = (bits >> 7) & 0x1f;
      out.imm = (((bits >> 31) & 0x1) << 20) | (((bits >> 12) & 0xff) << 12) |
                (((bits >> 20) & 0x1) << 11) | (((bits >> 21) & 0x3ff) << 1);
      break;
  }
  return out;
}

bool is_load(const decoded_insn& insn)
{
  return insn.type == "LB" || insn.type == "LH" || insn.type == "LW" ||
         insn.type == "LBU" || insn.type == "LHU";
}

bool is_store(const decoded_insn& insn)
{
  return insn.type == "SB" || insn.type == "SH" || insn.type == "SW";
}

bool is_mem(const decoded_insn& insn)
{
  return is_load(insn) || is_store(insn);
}

bool is_branch(const decoded_insn& insn)
{
  return insn.type == "BEQ" || insn.type == "BNE" || insn.type == "BLT" ||
         insn.type == "BGE" || insn.type == "BLTU" || insn.type == "BGEU";
}

bool is_jump(const decoded_insn& insn)
{
  return insn.type == "JAL" || insn.type == "JALR";
}

bool is_shift_imm(const decoded_insn& insn)
{
  return insn.type == "SLLI" || insn.type == "SRLI" || insn.type == "SRAI";
}

bool is_control(const decoded_insn& insn)
{
  return is_branch(insn) || is_jump(insn);
}

uint32_t sext(uint32_t value, unsigned bits)
{
  const uint32_t mask = 1u << (bits - 1);
  return (value ^ mask) - mask;
}

std::optional<uint32_t> semantic_rd_value(const decoded_insn& insn, reg_t pc,
                                          uint32_t rs1, uint32_t rs2)
{
  const uint32_t imm12 = insn.imm ? sext(*insn.imm, 12) : 0;
  const uint32_t shamt = insn.imm.value_or(insn.rs2.value_or(0)) & 0x1f;
  if (insn.type == "LUI") return insn.imm.value_or(0);
  if (insn.type == "AUIPC") return static_cast<uint32_t>(pc) + insn.imm.value_or(0);
  if (insn.type == "JAL" || insn.type == "JALR") return static_cast<uint32_t>(pc + 4);
  if (insn.type == "ADDI") return rs1 + imm12;
  if (insn.type == "SLTI") return static_cast<int32_t>(rs1) < static_cast<int32_t>(imm12);
  if (insn.type == "SLTIU") return rs1 < imm12;
  if (insn.type == "XORI") return rs1 ^ imm12;
  if (insn.type == "ORI") return rs1 | imm12;
  if (insn.type == "ANDI") return rs1 & imm12;
  if (insn.type == "SLLI") return rs1 << shamt;
  if (insn.type == "SRLI") return rs1 >> shamt;
  if (insn.type == "SRAI") return static_cast<uint32_t>(static_cast<int32_t>(rs1) >> shamt);
  if (insn.type == "ADD") return rs1 + rs2;
  if (insn.type == "SUB") return rs1 - rs2;
  if (insn.type == "SLL") return rs1 << (rs2 & 0x1f);
  if (insn.type == "SLT") return static_cast<int32_t>(rs1) < static_cast<int32_t>(rs2);
  if (insn.type == "SLTU") return rs1 < rs2;
  if (insn.type == "XOR") return rs1 ^ rs2;
  if (insn.type == "SRL") return rs1 >> (rs2 & 0x1f);
  if (insn.type == "SRA") return static_cast<uint32_t>(static_cast<int32_t>(rs1) >> (rs2 & 0x1f));
  if (insn.type == "OR") return rs1 | rs2;
  if (insn.type == "AND") return rs1 & rs2;
  if (insn.type == "MUL") return rs1 * rs2;
  if (insn.type == "MULH") {
    const int64_t r = static_cast<int64_t>(static_cast<int32_t>(rs1)) *
                      static_cast<int64_t>(static_cast<int32_t>(rs2));
    return static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
  }
  if (insn.type == "MULHSU") {
    const int64_t r = static_cast<int64_t>(static_cast<int32_t>(rs1)) *
                      static_cast<uint64_t>(rs2);
    return static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
  }
  if (insn.type == "MULHU") {
    const uint64_t r = static_cast<uint64_t>(rs1) * static_cast<uint64_t>(rs2);
    return static_cast<uint32_t>(r >> 32);
  }
  if (insn.type == "DIV") {
    if (rs2 == 0) return UINT32_MAX;
    if (rs1 == 0x80000000u && rs2 == UINT32_MAX) return rs1;
    return static_cast<uint32_t>(static_cast<int32_t>(rs1) / static_cast<int32_t>(rs2));
  }
  if (insn.type == "DIVU") return rs2 == 0 ? UINT32_MAX : rs1 / rs2;
  if (insn.type == "REM") {
    if (rs2 == 0) return rs1;
    if (rs1 == 0x80000000u && rs2 == UINT32_MAX) return 0;
    return static_cast<uint32_t>(static_cast<int32_t>(rs1) % static_cast<int32_t>(rs2));
  }
  if (insn.type == "REMU") return rs2 == 0 ? rs1 : rs1 % rs2;
  return std::nullopt;
}

uint32_t byte_mask_for(const decoded_insn& insn, uint32_t addr)
{
  const unsigned offset = addr & 0x3u;
  unsigned bytes = 0;
  if (insn.type == "LB" || insn.type == "LBU" || insn.type == "SB")
    bytes = 1;
  else if (insn.type == "LH" || insn.type == "LHU" || insn.type == "SH")
    bytes = 2;
  else if (insn.type == "LW" || insn.type == "SW")
    bytes = 4;
  uint32_t mask = 0;
  for (unsigned i = 0; i < bytes && offset + i < 4; i++)
    mask |= 1u << (offset + i);
  return mask;
}

uint32_t apply_byte_enable_mask(uint32_t value, uint32_t byte_mask)
{
  uint32_t out = 0;
  for (unsigned i = 0; i < 4; i++) {
    if (byte_mask & (1u << i))
      out |= value & (0xffu << (i * 8));
  }
  return out;
}

struct data_mem_model {
  std::vector<std::pair<uint32_t, uint8_t>> history;

  data_mem_model() : history(DATA_MEM_HISTORY, {0, 0}) {}

  uint32_t load_bus_value(const decoded_insn& insn, uint32_t addr) const
  {
    uint32_t value = addr % 0x1000u;
    const unsigned offset = addr & 0x3u;
    if (insn.type == "LH" || insn.type == "LHU") {
      if (offset == 2)
        return 0;
      return value & 0xffffu;
    }
    if (insn.type == "LW")
      return value;
    const uint32_t byte_mask = byte_mask_for(insn, addr);
    for (const auto& [stored_addr, stored_value] : history) {
      for (unsigned lane = 0; lane < 4; lane++) {
        if ((byte_mask & (1u << lane)) && addr + lane == stored_addr) {
          value &= ~(0xffu << (lane * 8));
          value |= static_cast<uint32_t>(stored_value) << (lane * 8);
        }
      }
    }
    return apply_byte_enable_mask(value, byte_mask);
  }

  void store(const decoded_insn& insn, uint32_t addr, uint32_t value)
  {
    const uint32_t byte_mask = byte_mask_for(insn, addr);
    const unsigned offset = addr & 0x3u;
    for (unsigned lane = 0; lane < 4; lane++) {
      if (!(byte_mask & (1u << lane))) continue;
      const unsigned source_lane = lane >= offset ? lane - offset : lane;
      const uint8_t byte = static_cast<uint8_t>((value >> (source_lane * 8)) & 0xffu);
      history.erase(history.begin());
      history.push_back({addr + lane, byte});
    }
  }
};

uint32_t synthetic_load_rd_value(const decoded_insn& insn, uint32_t bus_value, uint32_t addr)
{
  const unsigned offset = addr & 0x3u;
  if (insn.type == "LB") return sext((bus_value >> (offset * 8)) & 0xffu, 8);
  if (insn.type == "LBU") return (bus_value >> (offset * 8)) & 0xffu;
  if (insn.type == "LH") return sext(bus_value & 0xffffu, 16);
  if (insn.type == "LHU") return bus_value & 0xffffu;
  return bus_value;
}

bool branch_taken(const decoded_insn& insn, uint32_t rs1, uint32_t rs2)
{
  if (insn.type == "JAL" || insn.type == "JALR") return true;
  if (insn.type == "BEQ") return rs1 == rs2;
  if (insn.type == "BNE") return rs1 != rs2;
  if (insn.type == "BLT") return static_cast<int32_t>(rs1) < static_cast<int32_t>(rs2);
  if (insn.type == "BGE") return static_cast<int32_t>(rs1) >= static_cast<int32_t>(rs2);
  if (insn.type == "BLTU") return rs1 < rs2;
  if (insn.type == "BGEU") return rs1 >= rs2;
  return false;
}

struct sample {
  uint64_t retire = 0;
  uint32_t instr = NOP;
  uint64_t pc = 0;
  uint64_t next_pc = 0;
  uint64_t reg_rs1 = 0;
  uint64_t reg_rs2 = 0;
  uint64_t reg_rd = 0;
  std::optional<uint64_t> mem_addr;
  uint64_t mem_r_data = 0;
  uint64_t mem_w_data = 0;
};

std::vector<std::pair<reg_t, abstract_mem_t*>> make_mems(mem_t** mem_out)
{
  auto* mem = new mem_t(MEM_SIZE);
  *mem_out = mem;
  return {{TEXT_BASE, mem}};
}

void store_words(mem_t* mem, const std::vector<uint32_t>& words)
{
  for (size_t i = 0; i < words.size(); i++) {
    const uint32_t word = words[i];
    uint8_t bytes[4] = {
      static_cast<uint8_t>(word & 0xff),
      static_cast<uint8_t>((word >> 8) & 0xff),
      static_cast<uint8_t>((word >> 16) & 0xff),
      static_cast<uint8_t>((word >> 24) & 0xff),
    };
    if (!mem->store(i * 4, sizeof(bytes), bytes))
      throw std::runtime_error("Could not load program into Spike memory");
  }
}

std::vector<sample> run_side(const std::vector<uint32_t>& words, int retire_count,
                             const std::string& isa)
{
  mem_t* mem = nullptr;
  cfg_t cfg;
  cfg.isa = isa.c_str();
  cfg.priv = "M";
  cfg.mem_layout = {mem_cfg_t(TEXT_BASE, MEM_SIZE)};
  cfg.start_pc.set_global(TEXT_BASE);
  auto mems = make_mems(&mem);
  store_words(mem, words);

  std::vector<device_factory_sargs_t> plugin_devices;
  std::vector<std::string> htif_args{"none"};
  debug_module_config_t dm_config;
  sim_t sim(&cfg, false, mems, plugin_devices, false, htif_args, dm_config,
            "/dev/null", false, nullptr, false, nullptr, std::nullopt);
  processor_t* proc = sim.get_core(0);
  proc->enable_log_commits();

  data_mem_model data_mem;
  std::vector<sample> out;
  out.reserve(retire_count);
  for (int retired = 1; retired <= retire_count; retired++) {
    state_t* state = proc->get_state();
    const reg_t pc = state->pc;
    uint32_t instr = NOP;
    bool synthetic_nop = pc < TEXT_BASE || pc >= TEXT_BASE + words.size() * 4;
    if (!synthetic_nop) {
      try {
        instr = static_cast<uint32_t>(proc->get_mmu()->load_insn(pc).insn.bits());
      } catch (trap_t&) {
        synthetic_nop = true;
      } catch (trap_debug_mode&) {
        synthetic_nop = true;
      }
    }
    const auto decoded = decode(instr);
    sample s;
    s.retire = retired;
    s.instr = instr;
    s.pc = pc;
    s.next_pc = pc + 4;
    if (synthetic_nop) {
      state->pc = pc + 4;
      out.push_back(s);
      continue;
    }
    if (decoded.rs1) s.reg_rs1 = state->XPR[*decoded.rs1];
    if (decoded.rs2 && !is_shift_imm(decoded)) s.reg_rs2 = state->XPR[*decoded.rs2];
    if (is_mem(decoded) && decoded.imm)
      s.mem_addr = static_cast<uint32_t>(s.reg_rs1) + sext(*decoded.imm, 12);
    if (is_store(decoded))
      s.mem_w_data = s.reg_rs2;
    if (is_load(decoded) && s.mem_addr) {
      s.mem_r_data = data_mem.load_bus_value(decoded, static_cast<uint32_t>(*s.mem_addr));
      if (decoded.rd && *decoded.rd != 0)
        s.reg_rd = synthetic_load_rd_value(decoded, static_cast<uint32_t>(s.mem_r_data),
                                           static_cast<uint32_t>(*s.mem_addr));
    }
    if (decoded.rd) {
      if (*decoded.rd != 0) {
        if (auto value = semantic_rd_value(decoded, pc, static_cast<uint32_t>(s.reg_rs1),
                                           static_cast<uint32_t>(s.reg_rs2)))
          s.reg_rd = *value;
      }
    }

    bool step_trapped = false;
    try {
      proc->step(1);
    } catch (trap_t&) {
      step_trapped = true;
    } catch (trap_debug_mode&) {
      step_trapped = true;
    }

    for (const auto& write : state->log_reg_write) {
      if ((write.first & 0xf) == 0) {
        const uint32_t rd = write.first >> 4;
        if (decoded.rd && rd == *decoded.rd && rd != 0)
          s.reg_rd = write.second.v[0];
      }
    }
    if (!state->log_mem_read.empty()) {
      const auto& item = state->log_mem_read.front();
      s.mem_addr = std::get<0>(item);
      s.mem_r_data = std::get<1>(item);
    }
    if (!state->log_mem_write.empty()) {
      const auto& item = state->log_mem_write.front();
      s.mem_addr = std::get<0>(item);
      s.mem_w_data = std::get<1>(item);
    }
    if (is_store(decoded) && s.mem_addr)
      data_mem.store(decoded, static_cast<uint32_t>(*s.mem_addr), static_cast<uint32_t>(s.reg_rs2));
    if (state->pc >= HARNESS_TEXT_BASE &&
        state->pc < HARNESS_TEXT_BASE + static_cast<reg_t>(words.size() * 4)) {
      state->pc = TEXT_BASE + (state->pc - HARNESS_TEXT_BASE);
    }
    s.next_pc = state->pc;
    out.push_back(s);
    if (step_trapped)
      break;
  }
  return out;
}

struct atom {
  std::string type;
  std::string observation;

  bool operator<(const atom& other) const
  {
    return std::tie(type, observation) < std::tie(other.type, other.observation);
  }
};

void add_if(std::set<atom>& atoms, const decoded_insn& i1, const decoded_insn& i2,
            const std::string& observation,
            bool has1, bool has2)
{
  if (has1) atoms.insert({i1.type, observation});
  if (has2) atoms.insert({i2.type, observation});
}

template <typename T>
void compare_value(std::set<atom>& atoms, const decoded_insn& i1, const decoded_insn& i2,
                   const std::string& observation, bool has1, bool has2, T v1, T v2)
{
  if (has1 && has2 && v1 != v2) {
    atoms.insert({i1.type, observation});
    atoms.insert({i2.type, observation});
  } else if (has1 && !has2) {
    atoms.insert({i1.type, observation});
  } else if (!has1 && has2) {
    atoms.insert({i2.type, observation});
  }
}

void compare_dependency(std::set<atom>& atoms, const decoded_insn& i1, const decoded_insn& i2,
                        const decoded_insn& p1, const decoded_insn& p2,
                        const std::string& observation,
                        std::optional<uint32_t> r1, std::optional<uint32_t> r2)
{
  const bool has1 = r1.has_value() && p1.rd.has_value();
  const bool has2 = r2.has_value() && p2.rd.has_value();
  const bool dep1 = has1 && *r1 == *p1.rd;
  const bool dep2 = has2 && *r2 == *p2.rd;

  if (has1 && has2 && dep1 != dep2) {
    atoms.insert({i1.type, observation});
    atoms.insert({i2.type, observation});
  } else if (has1 && !has2) {
    atoms.insert({i1.type, observation});
  } else if (!has1 && has2) {
    atoms.insert({i2.type, observation});
  }
}

std::set<atom> extract_atoms(const std::vector<sample>& left, const std::vector<sample>& right)
{
  const size_t count = std::min(left.size(), right.size());
  std::set<atom> atoms;
  for (size_t idx = INIT_COUNT; idx < count; idx++) {
    const auto i1 = decode(left[idx].instr);
    const auto i2 = decode(right[idx].instr);

    if (i1.format != i2.format) {
      atoms.insert({i1.type, "FORMAT"});
      atoms.insert({i2.type, "FORMAT"});
    }
    if (i1.opcode != i2.opcode) {
      atoms.insert({i1.type, "OPCODE"});
      atoms.insert({i2.type, "OPCODE"});
    }
    if (i1.funct3 != i2.funct3) add_if(atoms, i1, i2, "FUNCT3", i1.funct3.has_value(), i2.funct3.has_value());
    if (i1.funct7 != i2.funct7) add_if(atoms, i1, i2, "FUNCT7", i1.funct7.has_value(), i2.funct7.has_value());
    if (i1.rd != i2.rd) add_if(atoms, i1, i2, "RD", i1.rd.has_value(), i2.rd.has_value());
    if (i1.rs1 != i2.rs1) add_if(atoms, i1, i2, "RS1", i1.rs1.has_value(), i2.rs1.has_value());
    if (i1.rs2 != i2.rs2) add_if(atoms, i1, i2, "RS2", i1.rs2.has_value(), i2.rs2.has_value());
    if (i1.imm != i2.imm) add_if(atoms, i1, i2, "IMM", i1.imm.has_value(), i2.imm.has_value());

    compare_value(atoms, i1, i2, "REG_RS1", i1.rs1.has_value(), i2.rs1.has_value(), left[idx].reg_rs1, right[idx].reg_rs1);
    compare_value(atoms, i1, i2, "REG_RS2",
                  i1.rs2.has_value() && !is_shift_imm(i1),
                  i2.rs2.has_value() && !is_shift_imm(i2),
                  left[idx].reg_rs2, right[idx].reg_rs2);
    compare_value(atoms, i1, i2, "REG_RD", i1.rd.has_value(), i2.rd.has_value(), left[idx].reg_rd, right[idx].reg_rd);
    compare_value(atoms, i1, i2, "MEM_ADDR", is_mem(i1), is_mem(i2),
                  left[idx].mem_addr.value_or(0), right[idx].mem_addr.value_or(0));
    compare_value(atoms, i1, i2, "MEM_R_DATA", is_load(i1), is_load(i2),
                  left[idx].mem_r_data, right[idx].mem_r_data);
    compare_value(atoms, i1, i2, "MEM_W_DATA", is_store(i1), is_store(i2),
                  left[idx].mem_w_data, right[idx].mem_w_data);

    compare_value(atoms, i1, i2, "IS_ALIGNED", is_mem(i1), is_mem(i2),
                  (left[idx].mem_addr.value_or(0) & 0x3u) == 0,
                  (right[idx].mem_addr.value_or(0) & 0x3u) == 0);
    compare_value(atoms, i1, i2, "IS_HALF_ALIGNED", is_mem(i1), is_mem(i2),
                  (left[idx].mem_addr.value_or(0) & 0x3u) != 3,
                  (right[idx].mem_addr.value_or(0) & 0x3u) != 3);

    compare_value(atoms, i1, i2, "IS_BRANCH", is_control(i1), is_control(i2),
                  is_control(i1), is_control(i2));
    const bool taken1 = branch_taken(i1, static_cast<uint32_t>(left[idx].reg_rs1),
                                     static_cast<uint32_t>(left[idx].reg_rs2));
    const bool taken2 = branch_taken(i2, static_cast<uint32_t>(right[idx].reg_rs1),
                                     static_cast<uint32_t>(right[idx].reg_rs2));
    compare_value(atoms, i1, i2, "BRANCH_TAKEN", is_control(i1), is_control(i2),
                  taken1, taken2);
    compare_value(atoms, i1, i2, "NEW_PC", is_control(i1), is_control(i2),
                  left[idx].next_pc, right[idx].next_pc);

    for (size_t distance = 1; distance <= 4; distance++) {
      if (idx < distance) break;
      const auto p1 = decode(left[idx - distance].instr);
      const auto p2 = decode(right[idx - distance].instr);
      const std::string suffix = "_" + std::to_string(distance);
      compare_dependency(atoms, i1, i2, p1, p2, "RAW_RS1" + suffix, i1.rs1, i2.rs1);
      compare_dependency(atoms, i1, i2, p1, p2, "RAW_RS2" + suffix, i1.rs2, i2.rs2);
      compare_dependency(atoms, i1, i2, p1, p2, "WAW" + suffix, i1.rd, i2.rd);
    }

    if ((is_control(i1) || is_control(i2)) &&
        (taken1 != taken2 || left[idx].next_pc != right[idx].next_pc))
      break;
  }
  return atoms;
}

struct case_result {
  int ordinal = -1;
  int index = 0;
  std::set<atom> atoms;
  std::optional<std::string> error;
};

case_result run_case(const test_case& tc, const std::string& isa, int ordinal = -1)
{
  case_result result;
  result.ordinal = ordinal;
  result.index = tc.index;
  try {
    const int retire_count = INIT_COUNT + tc.max_instruction_count;
    const auto left_words = words_for(tc.registers1, tc.program1, tc.max_instruction_count);
    const auto right_words = words_for(tc.registers2, tc.program2, tc.max_instruction_count);
    const auto left = run_side(left_words, retire_count, isa);
    const auto right = run_side(right_words, retire_count, isa);
    result.atoms = extract_atoms(left, right);
  } catch (const std::exception& e) {
    result.error = e.what();
  }
  return result;
}

std::string json_escape(const std::string& s)
{
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

void write_atoms(std::ostream& out, const std::set<atom>& atoms)
{
  out << "[";
  bool first = true;
  for (const auto& a : atoms) {
    if (!first) out << ",";
    first = false;
    out << "{\"type\":\"" << json_escape(a.type) << "\",\"observation\":\""
        << json_escape(a.observation) << "\"}";
  }
  out << "]";
}

void write_result(std::ostream& out, const case_result& result)
{
  out << "{\"ordinal\":" << result.ordinal
      << ",\"case_index\":" << result.index << ",\"atoms\":";
  write_atoms(out, result.atoms);
  if (result.error)
    out << ",\"error\":\"" << json_escape(*result.error) << "\"";
  out << "}";
}

void write_results(std::ostream& out, const std::vector<case_result>& results)
{
  size_t failed = 0;
  for (const auto& result : results)
    if (result.error) failed++;
  out << "{\"cases\":[";
  for (size_t i = 0; i < results.size(); i++) {
    if (i) out << ",";
    write_result(out, results[i]);
  }
  out << "],\"summary\":{\"total\":" << results.size()
      << ",\"failed\":" << failed << "}}\n";
}

std::string run_testcases_json(const std::string& testcases_json, const std::string& isa,
                               int ordinal)
{
  const auto cases = parse_testcases_json(testcases_json);
  std::vector<case_result> results;
  if (ordinal < 0) {
    results.reserve(cases.size());
    for (size_t i = 0; i < cases.size(); i++)
      results.push_back(run_case(cases[i], isa, static_cast<int>(i)));
  } else {
    if (static_cast<size_t>(ordinal) >= cases.size())
      throw std::runtime_error("No testcase with requested ordinal");
    results.push_back(run_case(cases[ordinal], isa, ordinal));
  }
  std::ostringstream out;
  write_results(out, results);
  return out.str();
}

void usage(const char* name)
{
  std::cerr << "usage: " << name
            << " --testcases <path> (--case-index <n> | --all)"
               " [--json-out <path>] [--isa <isa>]\n";
}

} // namespace

extern "C" char* contract_spike_atoms_json(const char* testcases_json,
                                           const char* isa,
                                           int ordinal)
{
  try {
    if (!testcases_json)
      throw std::runtime_error("Missing testcase JSON input");
    const std::string result = run_testcases_json(
        testcases_json,
        isa && *isa ? std::string(isa) : std::string("RV32IM_Zicclsm"),
        ordinal);
    char* out = static_cast<char*>(std::malloc(result.size() + 1));
    if (!out)
      return nullptr;
    std::memcpy(out, result.c_str(), result.size() + 1);
    return out;
  } catch (const std::exception& e) {
    std::ostringstream out;
    out << "{\"cases\":[],\"summary\":{\"total\":0,\"failed\":1},\"error\":\""
        << json_escape(e.what()) << "\"}\n";
    const std::string result = out.str();
    char* error = static_cast<char*>(std::malloc(result.size() + 1));
    if (!error)
      return nullptr;
    std::memcpy(error, result.c_str(), result.size() + 1);
    return error;
  }
}

extern "C" void contract_spike_free(char* ptr)
{
  std::free(ptr);
}

int main(int argc, char** argv)
{
  std::string testcases_path;
  std::string json_out_path;
  std::string isa = "RV32IM_Zicclsm";
  std::optional<int> case_index;
  bool all = false;

  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& option) -> std::string {
      if (i + 1 >= argc)
        throw std::runtime_error("Missing value for " + option);
      return argv[++i];
    };
    try {
      if (arg == "--testcases") {
        testcases_path = require_value(arg);
      } else if (arg == "--case-index") {
        case_index = std::stoi(require_value(arg));
      } else if (arg == "--all") {
        all = true;
      } else if (arg == "--json-out") {
        json_out_path = require_value(arg);
      } else if (arg == "--isa") {
        isa = require_value(arg);
      } else if (arg == "-h" || arg == "--help") {
        usage(argv[0]);
        return 0;
      } else {
        throw std::runtime_error("Unknown option " + arg);
      }
    } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      usage(argv[0]);
      return 1;
    }
  }

  if (testcases_path.empty() || (all == case_index.has_value())) {
    usage(argv[0]);
    return 1;
  }

  try {
    const auto cases = parse_testcases(testcases_path);
    std::vector<case_result> results;
    if (all) {
      results.reserve(cases.size());
      for (size_t i = 0; i < cases.size(); i++)
        results.push_back(run_case(cases[i], isa, static_cast<int>(i)));
    } else {
      auto it = std::find_if(cases.begin(), cases.end(), [&](const test_case& tc) {
        return tc.index == *case_index;
      });
      if (it == cases.end())
        throw std::runtime_error("No testcase with requested index");
      results.push_back(run_case(*it, isa, static_cast<int>(std::distance(cases.begin(), it))));
    }

    std::unique_ptr<std::ofstream> file_out;
    std::ostream* out = &std::cout;
    if (!json_out_path.empty()) {
      file_out = std::make_unique<std::ofstream>(json_out_path);
      if (!*file_out)
        throw std::runtime_error("Could not open output file " + json_out_path);
      out = file_out.get();
    }

    if (!all) {
      write_result(*out, results.front());
      *out << "\n";
    } else {
      write_results(*out, results);
    }

    return std::any_of(results.begin(), results.end(), [](const case_result& r) {
      return r.error.has_value();
    }) ? 2 : 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
