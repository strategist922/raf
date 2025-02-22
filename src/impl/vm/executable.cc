/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/*!
 * \file src/impl/vm/vm.cc
 * \brief The RAF virtual machine executable.
 */

#include <dmlc/memory_io.h>
#include <tvm/runtime/memory.h>
#include <tvm/runtime/object.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <vector>

#include "raf/serialization.h"
#include "raf/vm/vm.h"
#include "./serialize_util.h"

namespace raf {
namespace executor {
namespace vm {

using namespace raf::ir;
using namespace raf::registry;

#define STREAM_CHECK(val, section)                                         \
  CHECK(val) << "Invalid VM file format in the " << section << " section." \
             << "\n";

// Helper to serialize a vm instruction.
VMInstructionSerializer SerializeInstruction(const Instruction& instr);
// Helper to deserialize a serialized vm instruction.
Instruction DeserializeInstruction(const VMInstructionSerializer& instr);

PackedFunc Executable::GetFunction(const std::string& name, const ObjectPtr<Object>& sptr_to_self) {
  if (name == "get_lib") {
    return PackedFunc(
        [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->GetLib(); });
  } else if (name == "get_bytecode") {
    return PackedFunc(
        [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->GetBytecode(); });
  } else if (name == "get_stats") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->Stats(); });
  } else if (name == "save") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->Save(); });
  } else if (name == "get_function_arity") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      std::string func_name = args[0];
      *rv = this->GetFunctionArity(func_name);
    });
  } else if (name == "get_function_param_name") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      std::string func_name = args[0];
      int index = args[1];
      *rv = this->GetFunctionParameterName(func_name, index);
    });
  } else {
    LOG(FATAL) << "Unknown packed function: " << name;
    return PackedFunc(nullptr);
  }
}

int Executable::GetFunctionArity(std::string func_name) const {
  auto it = global_map.find(func_name);
  if (it == global_map.end()) {
    LOG(ERROR) << "Cannot find function " << func_name << " in executable";
    return -1;
  }
  const auto& func = functions[it->second];
  return func.params.size();
}

std::string Executable::GetFunctionParameterName(std::string func_name, uint32_t index) const {
  auto it = global_map.find(func_name);
  if (it == global_map.end()) {
    LOG(ERROR) << "Cannot find function " << func_name << " in executable";
    return "";
  }
  const auto& func = functions[it->second];
  if (index > func.params.size()) {
    LOG(ERROR) << "Invalid parameter index";
    return "";
  }
  return func.params[index];
}

std::string Executable::GetBytecode() const {
  std::ostringstream oss;

  for (size_t i = 0; i < functions.size(); ++i) {
    const auto& func = functions[i];
    // Print the header of the function format.
    oss << "VM Function[" << i << "]: " << func.name << "(";
    for (const auto& param : func.params) {
      oss << param << ", ";
    }
    oss.seekp(-2, std::ios_base::end);
    oss << ")" << std::endl;
    oss << "# reg file size = " << func.register_file_size << std::endl;
    oss << "# instruction count = " << func.instructions.size() << std::endl;

    // Print the instructions of a `VMFunction`.
    // The part after ";" is the instruction in text format.
    oss << "opcode, fields # inst(text):" << std::endl;
    for (size_t idx = 0; idx < func.instructions.size(); ++idx) {
      const auto& instr = func.instructions[idx];
      const auto& serialized_instr = SerializeInstruction(instr);
      oss << std::setw(2) << idx << ": " << serialized_instr.opcode << " ";
      for (auto it : serialized_instr.fields) {
        oss << it << " ";
      }
      oss << "  # " << instr;
      if (oss.str().back() != '\n') oss << std::endl;
    }
    oss << std::endl;
  }

  return oss.str();
}

std::string Executable::Stats() const {
  std::ostringstream oss;
  oss << "RAF VM executable statistics:" << std::endl;

  // Get the number of constants and the shape of each of them.
  oss << "  Constant shapes (# " << constants.size() << "): [";
  for (const auto& it : constants) {
    // TODO(vinx13): print RAF constant info
  }
  if (!constants.empty()) oss.seekp(-2, oss.cur);
  oss << "]" << std::endl;

  // Get the number of globals and the name of each of them.
  oss << "  Globals (#" << global_map.size() << "): [";
  for (const auto& it : global_map) {
    oss << "(\"" << it.first << "\", " << it.second << ")"
        << ", ";
  }
  if (!global_map.empty()) oss.seekp(-2, oss.cur);
  oss << "]" << std::endl;

  // Get the number of primitive ops and the name of each of them.
  oss << "  Primitive ops (#" << primitive_map.size() << "): [";
  std::vector<std::string> prim_ops;
  for (const auto& it : primitive_map) {
    auto packed_index = static_cast<size_t>(it.second);
    if (prim_ops.size() <= packed_index) {
      prim_ops.resize(packed_index + 1);
    }
    prim_ops[packed_index] = it.first;
  }
  for (const auto& it : prim_ops) {
    oss << it << ", ";
  }
  if (!prim_ops.empty()) oss.seekp(-2, oss.cur);
  oss << "]" << std::endl;

  return oss.str();
}

void SaveHeader(dmlc::Stream* strm) {
  uint64_t header = kMetaVMBytecodeMagic;
  strm->Write(header);
  std::string version = TVM_VERSION;
  strm->Write(version);
}

TVMByteArray Executable::Save() {
  // Initialize the stream object.
  code_.clear();
  dmlc::MemoryStringStream strm(&code_);

  // Save header
  SaveHeader(&strm);

  // Global section.
  SaveGlobalSection(&strm);

  // Constant section.
  SaveConstantSection(&strm);

  // Primitive names.
  SavePrimitiveOpNames(&strm);

  // Code section.
  SaveCodeSection(&strm);

  TVMByteArray arr;
  arr.data = code_.c_str();
  arr.size = code_.length();
  return arr;
}

void Executable::SaveGlobalSection(dmlc::Stream* strm) {
  std::vector<std::pair<std::string, Index> > globals(this->global_map.begin(),
                                                      this->global_map.end());
  auto comp = [](const std::pair<std::string, Index>& a, const std::pair<std::string, Index>& b) {
    return a.second < b.second;
  };
  std::sort(globals.begin(), globals.end(), comp);

  std::vector<std::string> glbs;
  for (const auto& it : globals) {
    glbs.push_back(it.first);
  }
  strm->Write(glbs);
}

void Executable::SaveConstantSection(dmlc::Stream* strm) {
  strm->Write(static_cast<uint64_t>(constants.size()));
  for (const auto& value : this->constants) {
    serialization::SerializeValue(strm, value);
  }
}

void Executable::SavePrimitiveOpNames(dmlc::Stream* strm) {
  std::vector<std::string> primitive_names;
  for (const auto& it : this->primitive_map) {
    auto packed_index = static_cast<size_t>(it.second);
    if (primitive_names.size() <= packed_index) {
      primitive_names.resize(packed_index + 1);
    }
    primitive_names[packed_index] = it.first;
  }
  strm->Write(primitive_names);
}

// Serialize a virtual machine instruction. It creates a list that contains the
// hash, opcode, and all fields of an instruction.
//
// For example, the function signature used to create an `AllocTensor`
// instruction is:
//   Instruction AllocTensor(std::vector<Index> shape, DLDataType dtype, RegName dst)
//
// The serialized form will be:
//   `hash 5 dtype.code dtype.bits dtype.lanes ndim dst_register val1 val2 ... valn`
//
// where hash is the hash of serialized instruction that is computed internally
// by the `VMInstructionExecutable`. It is used for sanity check before decoding.
// 5 shows opcode of `AllocTensor`, `(dtype.code dtype.bits dtype.lanes)`
// represents a `DLDataType`, `ndim` is the number of dimensions, `dst_register`
// is the destination register, and the rest of it together indicates the shape
// of the tensor to be allocated.
VMInstructionSerializer SerializeInstruction(const Instruction& instr) {
  std::vector<Index> fields;
  // Save the opcode.
  DLOG(INFO) << "Serializing: " << instr << std::endl;
  switch (instr.op) {
    case Opcode::Move: {
      // Number of fields = 2
      fields.assign({instr.from, instr.dst});
      break;
    }
    case Opcode::Ret: {
      // Number of fields = 1
      fields.push_back(instr.result);
      break;
    }
    case Opcode::Fatal: {
      // Number of fields = 0
      break;
    }
    case Opcode::InvokePacked: {
      // Number of fields = 3 + instr.arity
      // Note that arity includes both input arguments and outputs. We will
      // put all the `arity` number of fields in the end for serialization.
      fields.assign({instr.invoke_packed.packed_index, instr.invoke_packed.arity,
                     instr.invoke_packed.output_size});
      // Save the args.
      fields.insert(fields.end(), instr.invoke_packed.args,
                    instr.invoke_packed.args + instr.invoke_packed.arity);
      break;
    }
    case Opcode::AllocTensor: {
      // Number of fields = 8 + instr.alloc_tensor.ndim
      fields.push_back(instr.alloc_tensor.storage);
      fields.push_back(instr.alloc_tensor.offset);
      // Save `DLDataType` and the dst register.
      const auto& dtype = instr.alloc_tensor.dtype;
      fields.push_back(dtype.code);
      fields.push_back(dtype.bits);
      fields.push_back(dtype.lanes);

      fields.push_back(instr.alloc_tensor.own);

      // The number of dimensions is not needed for constructing an
      // `AllocTensor` instruction as it equals to the length of the `shape`
      // vector. However, we save it to conveniently deserialize the instruction
      // because we will know how many fields are needed by the `shape` argument.
      fields.push_back(instr.alloc_tensor.ndim);
      fields.push_back(instr.dst);

      // Save the shape of the tensor.
      // Note that this field is rotated to the end of the list.
      fields.insert(fields.end(), instr.alloc_tensor.shape,
                    instr.alloc_tensor.shape + instr.alloc_tensor.ndim);
      break;
    }
    case Opcode::AllocTensorReg: {
      // Number of fields = 8
      fields.push_back(instr.alloc_tensor_reg.storage);
      fields.push_back(instr.alloc_tensor_reg.offset);
      fields.push_back(instr.alloc_tensor_reg.shape_register);
      // Save `DLDataType` and the dst register.
      const auto& dtype = instr.alloc_tensor_reg.dtype;
      fields.push_back(dtype.code);
      fields.push_back(dtype.bits);
      fields.push_back(dtype.lanes);
      fields.push_back(instr.dst);
      fields.push_back(instr.alloc_tensor_reg.own);
      break;
    }
    case Opcode::AllocStorage: {
      fields.push_back(instr.alloc_storage.allocation_size);
      fields.push_back(instr.alloc_storage.alignment);
      // Save `DLDataType` and the dst register.
      const auto& dtype = instr.alloc_storage.dtype_hint;
      fields.push_back(dtype.code);
      fields.push_back(dtype.bits);
      fields.push_back(dtype.lanes);
      fields.push_back(instr.alloc_storage.device_type);
      fields.push_back(instr.alloc_storage.device_id);
      fields.push_back(instr.dst);
      break;
    }
    case Opcode::Free: {
      fields.push_back(instr.free.memory);
      break;
    }
    case Opcode::AllocTuple: {
      // Number of fields = 2 + instr.num_fields
      fields.assign({instr.alloc_tuple.num_fields, instr.dst});

      // Save the fields.
      fields.insert(fields.end(), instr.alloc_tuple.fields,
                    instr.alloc_tuple.fields + instr.alloc_tuple.num_fields);
      break;
    }
    case Opcode::AllocClosure: {
      // Number of fields = 3 + instr.num_freevar
      fields.assign({instr.alloc_closure.func_index, instr.alloc_closure.num_free_vars, instr.dst});

      // Save the free vars.
      fields.insert(fields.end(), instr.alloc_closure.free_vars,
                    instr.alloc_closure.free_vars + instr.alloc_closure.num_free_vars);
      break;
    }
    case Opcode::SetShape: {
      // Number of fields = 3
      fields.push_back(instr.set_shape.data);
      fields.push_back(instr.set_shape.shape);
      fields.push_back(instr.dst);
      break;
    }
    case Opcode::If: {
      // Number of fields = 4
      fields.assign({instr.if_op.test, instr.if_op.target, instr.if_op.true_offset,
                     instr.if_op.false_offset});
      break;
    }
    case Opcode::InvokeFunc: {
      // Number of fields = 3 + instr.num_args
      fields.assign({instr.invoke_func.func_index, instr.invoke_func.num_args, instr.dst});

      // Save the args.
      fields.insert(fields.end(), instr.invoke_func.args,
                    instr.invoke_func.args + instr.invoke_func.num_args);
      break;
    }
    case Opcode::InvokeClosure: {
      // Number of fields = 3 + instr.num_closure_args
      fields.assign({instr.invoke_closure.closure, instr.invoke_closure.num_args, instr.dst});

      // Save the args.
      fields.insert(fields.end(), instr.invoke_closure.args,
                    instr.invoke_closure.args + instr.invoke_closure.num_args);
      break;
    }
    case Opcode::LoadConst: {
      // Number of fields = 2
      fields.assign({instr.const_index, instr.dst});
      break;
    }
    case Opcode::LoadConsti: {
      // Number of fields = 2
      fields.assign({instr.load_consti.val, instr.dst});
      break;
    }
    case Opcode::GetField: {
      // Number of fields = 3
      fields.assign({instr.get_field.object, instr.get_field.field_index, instr.dst});
      break;
    }
    case Opcode::Goto: {
      // Number of fields = 1
      fields.push_back(instr.pc_offset);
      break;
    }
    case Opcode::InvokeJit: {
      // Number of fields = 3 + instr.arity
      // Note that arity includes both input arguments and outputs. We will
      // put all the `arity` number of fields in the end for serialization.
      fields.assign(
          {instr.invoke_jit.op_reg, instr.invoke_jit.arity, instr.invoke_jit.output_size});
      // Save the args.
      fields.insert(fields.end(), instr.invoke_jit.args,
                    instr.invoke_jit.args + instr.invoke_jit.arity);
      break;
    }
    case Opcode::InferType: {
      // Number of fields = 3 + instr.num_args
      fields.assign({instr.infer_type.op_reg, instr.infer_type.num_args, instr.dst});

      // Save the args.
      fields.insert(fields.end(), instr.infer_type.args,
                    instr.infer_type.args + instr.infer_type.num_args);
      break;
    }
    case Opcode::CudaSetStream: {
      // Number of fields = 2
      fields.push_back(instr.cuda_set_stream.device_id);
      fields.push_back(instr.cuda_set_stream.stream_id);
      break;
    }
    case Opcode::CudaAddEvent:
    case Opcode::CudaWaitEvent: {
      // Number of fields = 2
      fields.push_back(instr.cuda_event.event_id);
      fields.push_back(instr.cuda_event.stream_id);
      break;
    }
    case Opcode::CudaStreamBarrier: {
      // Number of fields = 0
      break;
    }
    default:
      LOG(FATAL) << "Invalid opcode" << static_cast<int>(instr.op);
      break;
  }

  return VMInstructionSerializer(static_cast<Index>(instr.op), fields);
}

void Executable::SaveCodeSection(dmlc::Stream* strm) {
  // Save the number of functions.
  strm->Write(static_cast<uint64_t>(this->functions.size()));
  for (const auto& func : this->functions) {
    // Save the function info.
    VMFunctionSerializer func_format(func.name, func.register_file_size, func.instructions.size(),
                                     func.params);
    func_format.Save(strm);

    // Serialize each instruction.
    for (const auto& instr : func.instructions) {
      const auto& serialized_instr = SerializeInstruction(instr);
      serialized_instr.Save(strm);
    }
  }
}

void LoadHeader(dmlc::Stream* strm) {
  // Check header.
  uint64_t header;
  STREAM_CHECK(strm->Read(&header), "header");
  STREAM_CHECK(header == kMetaVMBytecodeMagic, "header");

  // Check version.
  std::string version;
  STREAM_CHECK(strm->Read(&version), "version");
  STREAM_CHECK(version == TVM_VERSION, "version");
}

tvm::runtime::Module Executable::Load(const std::string& code, const tvm::runtime::Module lib) {
  auto exec = make_object<Executable>();
  exec->lib = lib;
  exec->code_ = code;
  dmlc::MemoryStringStream strm(&exec->code_);

  // Load header.
  LoadHeader(&strm);

  // Global section.
  exec->LoadGlobalSection(&strm);

  // Constant section.
  exec->LoadConstantSection(&strm);

  // Primitive names that will be invoked by `InvokePacked` instructions.
  exec->LoadPrimitiveOpNames(&strm);

  // Code section.
  exec->LoadCodeSection(&strm);

  return tvm::runtime::Module(exec);
}

void Executable::LoadGlobalSection(dmlc::Stream* strm) {
  std::vector<std::string> globals;
  STREAM_CHECK(strm->Read(&globals), "global");
  for (size_t i = 0; i < globals.size(); i++) {
    this->global_map.insert({globals[i], i});
  }
}

void Executable::LoadConstantSection(dmlc::Stream* strm) {
  uint64_t sz;
  // Load the number of constants.
  STREAM_CHECK(strm->Read(&sz, sizeof(sz)), "constant");
  size_t size = static_cast<size_t>(sz);
  // Load each of the constants.
  for (size_t i = 0; i < size; i++) {
    Value value = serialization::DeserializeValue(strm);
    constants.push_back(value);
  }
}

void Executable::LoadPrimitiveOpNames(dmlc::Stream* strm) {
  std::vector<std::string> primitive_names;
  STREAM_CHECK(strm->Read(&primitive_names), "primitive name");
  for (size_t i = 0; i < primitive_names.size(); i++) {
    this->primitive_map.insert({primitive_names[i], i});
  }
}

// Extract the `cnt` number of fields started at `start` from the list
// `instr_fields`.
inline std::vector<Index> ExtractFields(const std::vector<Index>& instr_fields, Index start,
                                        Index cnt) {
  CHECK_LE(static_cast<size_t>(start + cnt), instr_fields.size());
  std::vector<Index> ret;
  for (auto i = start; i < start + cnt; i++) {
    ret.push_back(instr_fields[i]);
  }
  return ret;
}

Instruction DeserializeInstruction(const VMInstructionSerializer& instr) {
  Opcode opcode = static_cast<Opcode>(instr.opcode);
  switch (opcode) {
    case Opcode::Move: {
      // Number of fields = 2
      DCHECK_EQ(instr.fields.size(), 2U);
      return Instruction::Move(instr.fields[0], instr.fields[1]);
    }
    case Opcode::Ret: {
      // Number of fields = 1
      DCHECK_EQ(instr.fields.size(), 1U);
      return Instruction::Ret(instr.fields[0]);
    }
    case Opcode::Fatal: {
      // Number of fields = 0
      DCHECK(instr.fields.empty());
      return Instruction::Fatal();
    }
    case Opcode::InvokePacked: {
      // Number of fields = 3 + instr.arity
      DCHECK_GE(instr.fields.size(), 3U);
      DCHECK_EQ(instr.fields.size(), 3U + static_cast<size_t>(instr.fields[1]));

      Index packed_index = instr.fields[0];
      Index arity = instr.fields[1];
      Index output_size = instr.fields[2];
      std::vector<RegName> args = ExtractFields(instr.fields, 3, arity);
      return Instruction::InvokePacked(packed_index, arity, output_size, args);
    }
    case Opcode::AllocTensor: {
      // Number of fields = 8 + instr.alloc_tensor.ndim
      DCHECK_GE(instr.fields.size(), 8U);
      DCHECK_EQ(instr.fields.size(), 8U + static_cast<size_t>(instr.fields[6]));

      RegName storage_reg = instr.fields[0];
      RegName offset = instr.fields[1];

      DLDataType dtype;
      dtype.code = instr.fields[2];
      dtype.bits = instr.fields[3];
      dtype.lanes = instr.fields[4];

      bool own = instr.fields[5];

      Index ndim = instr.fields[6];
      RegName dst = instr.fields[7];

      std::vector<Index> shape = ExtractFields(instr.fields, 8, ndim);

      return Instruction::AllocTensor(storage_reg, offset, shape, dtype, dst, own);
    }
    case Opcode::AllocTensorReg: {
      // Number of fields = 8
      DCHECK_EQ(instr.fields.size(), 7U);

      RegName storage_reg = instr.fields[0];
      RegName offset = instr.fields[1];
      Index shape_register = instr.fields[2];

      DLDataType dtype;
      dtype.code = instr.fields[3];
      dtype.bits = instr.fields[4];
      dtype.lanes = instr.fields[5];

      RegName dst = instr.fields[6];

      bool own = instr.fields[7];

      return Instruction::AllocTensorReg(storage_reg, offset, shape_register, dtype, dst, own);
    }
    case Opcode::AllocTuple: {
      // Number of fields = 2 + instr.num_fields
      DCHECK_GE(instr.fields.size(), 2U);
      DCHECK_EQ(instr.fields.size(), 2U + static_cast<size_t>(instr.fields[0]));

      Index num_fields = instr.fields[0];
      RegName dst = instr.fields[1];
      std::vector<RegName> fields = ExtractFields(instr.fields, 2, num_fields);

      return Instruction::AllocTuple(fields, dst);
    }
    case Opcode::AllocClosure: {
      // Number of fields = 3 + instr.num_freevar
      DCHECK_GE(instr.fields.size(), 3U);
      DCHECK_EQ(instr.fields.size(), 3U + static_cast<size_t>(instr.fields[1]));

      Index func_index = instr.fields[0];
      Index num_free_vars = instr.fields[1];
      RegName dst = instr.fields[2];
      std::vector<RegName> free_vars = ExtractFields(instr.fields, 3, num_free_vars);

      return Instruction::AllocClosure(func_index, free_vars, dst);
    }
    case Opcode::AllocStorage: {
      DCHECK_GE(instr.fields.size(), 6U);
      Index allocation_size = instr.fields[0];
      Index alignment = instr.fields[1];

      DLDataType dtype;
      dtype.code = instr.fields[2];
      dtype.bits = instr.fields[3];
      dtype.lanes = instr.fields[4];

      DevType device_type = instr.fields[5];
      Index device_id = instr.fields[6];
      RegName dst = instr.fields[7];

      return Instruction::AllocStorage(allocation_size, alignment, dtype, device_type, device_id,
                                       dst);
    }
    case Opcode::Free: {
      DCHECK_EQ(instr.fields.size(), 1U);
      RegName memory_reg = instr.fields[0];
      return Instruction::Free(memory_reg);
    }
    case Opcode::SetShape: {
      DCHECK_GE(instr.fields.size(), 3U);
      RegName data = instr.fields[0];
      RegName shape = instr.fields[1];
      RegName dst = instr.fields[2];

      return Instruction::SetShape(data, shape, dst);
    }
    case Opcode::If: {
      // Number of fields = 4
      DCHECK_EQ(instr.fields.size(), 4U);
      Index test = instr.fields[0];
      Index target = instr.fields[1];
      Index true_offset = instr.fields[2];
      Index false_offset = instr.fields[3];

      return Instruction::If(test, target, true_offset, false_offset);
    }
    case Opcode::InvokeFunc: {
      // Number of fields = 3 + instr.num_args
      DCHECK_GE(instr.fields.size(), 3U);
      DCHECK_EQ(instr.fields.size(), 3U + static_cast<size_t>(instr.fields[1]));

      Index func_index = instr.fields[0];
      Index num_args = instr.fields[1];
      RegName dst = instr.fields[2];
      std::vector<RegName> args = ExtractFields(instr.fields, 3, num_args);

      return Instruction::InvokeFunc(func_index, args, dst);
    }
    case Opcode::InvokeClosure: {
      // Number of fields = 3 + instr.num_closure_args
      DCHECK_GE(instr.fields.size(), 3U);
      DCHECK_EQ(instr.fields.size(), 3U + static_cast<size_t>(instr.fields[1]));

      RegName closure = instr.fields[0];
      Index num_closure_args = instr.fields[1];
      RegName dst = instr.fields[2];
      std::vector<RegName> args = ExtractFields(instr.fields, 3, num_closure_args);

      return Instruction::InvokeClosure(closure, args, dst);
    }
    case Opcode::LoadConst: {
      // Number of fields = 2
      DCHECK_EQ(instr.fields.size(), 2U);
      return Instruction::LoadConst(instr.fields[0], instr.fields[1]);
    }
    case Opcode::LoadConsti: {
      // Number of fields = 2
      DCHECK_EQ(instr.fields.size(), 2U);
      return Instruction::LoadConsti(instr.fields[0], instr.fields[1]);
    }
    case Opcode::GetField: {
      // Number of fields = 3
      DCHECK_EQ(instr.fields.size(), 3U);
      return Instruction::GetField(instr.fields[0], instr.fields[1], instr.fields[2]);
    }
    case Opcode::Goto: {
      // Number of fields = 1
      DCHECK_EQ(instr.fields.size(), 1U);
      return Instruction::Goto(instr.fields[0]);
    }
    case Opcode::InvokeJit: {
      // Number of fields = 3 + instr.arity
      DCHECK_GE(instr.fields.size(), 3U);
      DCHECK_EQ(instr.fields.size(), 3U + static_cast<size_t>(instr.fields[1]));

      RegName op_reg = instr.fields[0];
      Index arity = instr.fields[1];
      Index output_size = instr.fields[2];
      std::vector<RegName> args = ExtractFields(instr.fields, 3, arity);
      return Instruction::InvokeJit(op_reg, arity, output_size, args);
    }
    case Opcode::InferType: {
      // Number of fields = 3 + instr.num_args
      DCHECK_GE(instr.fields.size(), 3U);
      DCHECK_EQ(instr.fields.size(), 3U + static_cast<size_t>(instr.fields[1]));

      RegName op_reg = instr.fields[0];
      Index num_args = instr.fields[1];
      RegName dst = instr.fields[2];
      std::vector<RegName> args = ExtractFields(instr.fields, 3, num_args);
      return Instruction::InferType(op_reg, args, dst);
    }
    case Opcode::CudaSetStream: {
      // Number of fields = 2
      DCHECK_EQ(instr.fields.size(), 2U);
      return Instruction::CudaSetStream(instr.fields[0], instr.fields[1]);
    }
    case Opcode::CudaAddEvent: {
      // Number of fields = 2
      DCHECK_EQ(instr.fields.size(), 2U);
      return Instruction::CudaAddEvent(instr.fields[0], instr.fields[1]);
    }
    case Opcode::CudaWaitEvent: {
      // Number of fields = 2
      DCHECK_EQ(instr.fields.size(), 2U);
      return Instruction::CudaWaitEvent(instr.fields[0], instr.fields[1]);
    }
    case Opcode::CudaStreamBarrier: {
      // Number of fields = 0
      DCHECK(instr.fields.empty());
      return Instruction::Fatal();
    }
    default:
      LOG(FATAL) << "Invalid opcode" << instr.opcode;
      return Instruction();
  }
}

void Executable::LoadCodeSection(dmlc::Stream* strm) {
  // Load the number of functions.
  uint64_t sz;
  STREAM_CHECK(strm->Read(&sz, sizeof(sz)), "code");

  size_t num_funcs = static_cast<size_t>(sz);
  this->functions.resize(num_funcs);
  for (size_t i = 0; i < num_funcs; i++) {
    // Load the function info.
    VMFunctionSerializer loaded_func;
    STREAM_CHECK(loaded_func.Load(strm), "code/function");

    // Load the instructions.
    std::vector<Instruction> instructions;
    for (size_t j = 0; j < loaded_func.num_instructions; j++) {
      VMInstructionSerializer instr;
      std::vector<Index> instr_fields;
      STREAM_CHECK(instr.Load(strm), "code/instruction");
      instructions.push_back(DeserializeInstruction(instr));
    }

    // Create the VM function.
    VMFunction vm_func = VMFunction(loaded_func.name, loaded_func.params, instructions,
                                    loaded_func.register_file_size);
    auto it = this->global_map.find(loaded_func.name);
    CHECK(it != this->global_map.end());
    CHECK_LE(it->second, this->global_map.size());
    this->functions[it->second] = vm_func;
  }
}

RAF_REGISTER_GLOBAL("raf.vm.GetNumOfGlobals").set_body([](TVMArgs args, TVMRetValue* rv) {
  tvm::runtime::Module mod = args[0];
  const auto* exec = dynamic_cast<Executable*>(mod.operator->());
  CHECK(exec);
  *rv = static_cast<int>(exec->global_map.size());
});

RAF_REGISTER_GLOBAL("raf.vm.GetGlobalFields").set_body([](TVMArgs args, TVMRetValue* rv) {
  tvm::runtime::Module mod = args[0];
  const auto* exec = dynamic_cast<Executable*>(mod.operator->());
  CHECK(exec);
  int idx = args[1];
  std::vector<std::pair<std::string, Index> > globals(exec->global_map.begin(),
                                                      exec->global_map.end());
  auto comp = [](const std::pair<std::string, Index>& a, const std::pair<std::string, Index>& b) {
    return a.second < b.second;
  };
  std::sort(globals.begin(), globals.end(), comp);
  CHECK_LT(idx, globals.size());
  *rv = globals[idx].first;
});

RAF_REGISTER_GLOBAL("raf.vm.GetNumOfPrimitives").set_body([](TVMArgs args, TVMRetValue* rv) {
  tvm::runtime::Module mod = args[0];
  const auto* exec = dynamic_cast<Executable*>(mod.operator->());
  CHECK(exec);
  *rv = static_cast<int>(exec->primitive_map.size());
});

RAF_REGISTER_GLOBAL("raf.vm.GetPrimitiveFields").set_body([](TVMArgs args, TVMRetValue* rv) {
  tvm::runtime::Module mod = args[0];
  const auto* exec = dynamic_cast<Executable*>(mod.operator->());
  CHECK(exec);
  int idx = args[1];
  CHECK_GE(idx, 0);
  CHECK_LT(idx, exec->primitive_map.size());

  for (const auto& it : exec->primitive_map) {
    if (idx == static_cast<int>(it.second)) {
      *rv = it.first;
      break;
    }
  }
});

RAF_REGISTER_GLOBAL("raf.vm.Load_Executable")
    .set_body_typed([](std::string code, tvm::runtime::Module lib) {
      return Executable::Load(code, lib);
    });

}  // namespace vm
}  // namespace executor
}  // namespace raf
