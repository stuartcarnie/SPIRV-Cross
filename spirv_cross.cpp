/*
 * Copyright 2015-2019 Arm Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "spirv_cross.hpp"
#include "GLSL.std.450.h"
#include "spirv_cfg.hpp"
#include "spirv_parser.hpp"
#include <algorithm>
#include <cstring>
#include <utility>

using namespace std;
using namespace spv;
using namespace SPIRV_CROSS_NAMESPACE;

Compiler::Compiler(vector<uint32_t> ir_)
{
	Parser parser(move(ir_));
	parser.parse();
	set_ir(move(parser.get_parsed_ir()));
}

Compiler::Compiler(const uint32_t *ir_, size_t word_count)
{
	Parser parser(ir_, word_count);
	parser.parse();
	set_ir(move(parser.get_parsed_ir()));
}

Compiler::Compiler(const ParsedIR &ir_)
{
	set_ir(ir_);
}

Compiler::Compiler(ParsedIR &&ir_)
{
	set_ir(move(ir_));
}

void Compiler::set_ir(ParsedIR &&ir_)
{
	ir = move(ir_);
	parse_fixup();
}

void Compiler::set_ir(const ParsedIR &ir_)
{
	ir = ir_;
	parse_fixup();
}

string Compiler::compile()
{
	return "";
}

bool Compiler::variable_storage_is_aliased(const SPIRVariable &v)
{
	auto &type = get<SPIRType>(v.basetype);
	bool ssbo = v.storage == StorageClassStorageBuffer ||
	            ir.meta[type.self].decoration.decoration_flags.get(DecorationBufferBlock);
	bool image = type.basetype == SPIRType::Image;
	bool counter = type.basetype == SPIRType::AtomicCounter;

	bool is_restrict;
	if (ssbo)
		is_restrict = ir.get_buffer_block_flags(v).get(DecorationRestrict);
	else
		is_restrict = has_decoration(v.self, DecorationRestrict);

	return !is_restrict && (ssbo || image || counter);
}

bool Compiler::block_is_pure(const SPIRBlock &block)
{
	for (auto &i : block.ops)
	{
		auto ops = stream(i);
		auto op = static_cast<Op>(i.op);

		switch (op)
		{
		case OpFunctionCall:
		{
			uint32_t func = ops[2];
			if (!function_is_pure(get<SPIRFunction>(func)))
				return false;
			break;
		}

		case OpCopyMemory:
		case OpStore:
		{
			auto &type = expression_type(ops[0]);
			if (type.storage != StorageClassFunction)
				return false;
			break;
		}

		case OpImageWrite:
			return false;

		// Atomics are impure.
		case OpAtomicLoad:
		case OpAtomicStore:
		case OpAtomicExchange:
		case OpAtomicCompareExchange:
		case OpAtomicCompareExchangeWeak:
		case OpAtomicIIncrement:
		case OpAtomicIDecrement:
		case OpAtomicIAdd:
		case OpAtomicISub:
		case OpAtomicSMin:
		case OpAtomicUMin:
		case OpAtomicSMax:
		case OpAtomicUMax:
		case OpAtomicAnd:
		case OpAtomicOr:
		case OpAtomicXor:
			return false;

		// Geometry shader builtins modify global state.
		case OpEndPrimitive:
		case OpEmitStreamVertex:
		case OpEndStreamPrimitive:
		case OpEmitVertex:
			return false;

		// Barriers disallow any reordering, so we should treat blocks with barrier as writing.
		case OpControlBarrier:
		case OpMemoryBarrier:
			return false;

		// Ray tracing builtins are impure.
		case OpReportIntersectionNV:
		case OpIgnoreIntersectionNV:
		case OpTerminateRayNV:
		case OpTraceNV:
		case OpExecuteCallableNV:
			return false;

			// OpExtInst is potentially impure depending on extension, but GLSL builtins are at least pure.

		default:
			break;
		}
	}

	return true;
}

string Compiler::to_name(uint32_t id, bool allow_alias) const
{
	if (allow_alias && ir.ids[id].get_type() == TypeType)
	{
		// If this type is a simple alias, emit the
		// name of the original type instead.
		// We don't want to override the meta alias
		// as that can be overridden by the reflection APIs after parse.
		auto &type = get<SPIRType>(id);
		if (type.type_alias)
		{
			// If the alias master has been specially packed, we will have emitted a clean variant as well,
			// so skip the name aliasing here.
			if (!has_extended_decoration(type.type_alias, SPIRVCrossDecorationPacked))
				return to_name(type.type_alias);
		}
	}

	auto &alias = ir.get_name(id);
	if (alias.empty())
		return join("_", id);
	else
		return alias;
}

bool Compiler::function_is_pure(const SPIRFunction &func)
{
	for (auto block : func.blocks)
	{
		if (!block_is_pure(get<SPIRBlock>(block)))
		{
			//fprintf(stderr, "Function %s is impure!\n", to_name(func.self).c_str());
			return false;
		}
	}

	//fprintf(stderr, "Function %s is pure!\n", to_name(func.self).c_str());
	return true;
}

void Compiler::register_global_read_dependencies(const SPIRBlock &block, uint32_t id)
{
	for (auto &i : block.ops)
	{
		auto ops = stream(i);
		auto op = static_cast<Op>(i.op);

		switch (op)
		{
		case OpFunctionCall:
		{
			uint32_t func = ops[2];
			register_global_read_dependencies(get<SPIRFunction>(func), id);
			break;
		}

		case OpLoad:
		case OpImageRead:
		{
			// If we're in a storage class which does not get invalidated, adding dependencies here is no big deal.
			auto *var = maybe_get_backing_variable(ops[2]);
			if (var && var->storage != StorageClassFunction)
			{
				auto &type = get<SPIRType>(var->basetype);

				// InputTargets are immutable.
				if (type.basetype != SPIRType::Image && type.image.dim != DimSubpassData)
					var->dependees.push_back(id);
			}
			break;
		}

		default:
			break;
		}
	}
}

void Compiler::register_global_read_dependencies(const SPIRFunction &func, uint32_t id)
{
	for (auto block : func.blocks)
		register_global_read_dependencies(get<SPIRBlock>(block), id);
}

SPIRVariable *Compiler::maybe_get_backing_variable(uint32_t chain)
{
	auto *var = maybe_get<SPIRVariable>(chain);
	if (!var)
	{
		auto *cexpr = maybe_get<SPIRExpression>(chain);
		if (cexpr)
			var = maybe_get<SPIRVariable>(cexpr->loaded_from);

		auto *access_chain = maybe_get<SPIRAccessChain>(chain);
		if (access_chain)
			var = maybe_get<SPIRVariable>(access_chain->loaded_from);
	}

	return var;
}

void Compiler::register_read(uint32_t expr, uint32_t chain, bool forwarded)
{
	auto &e = get<SPIRExpression>(expr);
	auto *var = maybe_get_backing_variable(chain);

	if (var)
	{
		e.loaded_from = var->self;

		// If the backing variable is immutable, we do not need to depend on the variable.
		if (forwarded && !is_immutable(var->self))
			var->dependees.push_back(e.self);

		// If we load from a parameter, make sure we create "inout" if we also write to the parameter.
		// The default is "in" however, so we never invalidate our compilation by reading.
		if (var && var->parameter)
			var->parameter->read_count++;
	}
}

void Compiler::register_write(uint32_t chain)
{
	auto *var = maybe_get<SPIRVariable>(chain);
	if (!var)
	{
		// If we're storing through an access chain, invalidate the backing variable instead.
		auto *expr = maybe_get<SPIRExpression>(chain);
		if (expr && expr->loaded_from)
			var = maybe_get<SPIRVariable>(expr->loaded_from);

		auto *access_chain = maybe_get<SPIRAccessChain>(chain);
		if (access_chain && access_chain->loaded_from)
			var = maybe_get<SPIRVariable>(access_chain->loaded_from);
	}

	if (var)
	{
		// If our variable is in a storage class which can alias with other buffers,
		// invalidate all variables which depend on aliased variables. And if this is a
		// variable pointer, then invalidate all variables regardless.
		if (get_variable_data_type(*var).pointer)
			flush_all_active_variables();
		if (variable_storage_is_aliased(*var))
			flush_all_aliased_variables();
		else if (var)
			flush_dependees(*var);

		// We tried to write to a parameter which is not marked with out qualifier, force a recompile.
		if (var->parameter && var->parameter->write_count == 0)
		{
			var->parameter->write_count++;
			force_recompile();
		}
	}
	else
	{
		// If we stored through a variable pointer, then we don't know which
		// variable we stored to. So *all* expressions after this point need to
		// be invalidated.
		// FIXME: If we can prove that the variable pointer will point to
		// only certain variables, we can invalidate only those.
		flush_all_active_variables();
	}
}

void Compiler::flush_dependees(SPIRVariable &var)
{
	for (auto expr : var.dependees)
		invalid_expressions.insert(expr);
	var.dependees.clear();
}

void Compiler::flush_all_aliased_variables()
{
	for (auto aliased : aliased_variables)
		flush_dependees(get<SPIRVariable>(aliased));
}

void Compiler::flush_all_atomic_capable_variables()
{
	for (auto global : global_variables)
		flush_dependees(get<SPIRVariable>(global));
	flush_all_aliased_variables();
}

void Compiler::flush_control_dependent_expressions(uint32_t block_id)
{
	auto &block = get<SPIRBlock>(block_id);
	for (auto &expr : block.invalidate_expressions)
		invalid_expressions.insert(expr);
	block.invalidate_expressions.clear();
}

void Compiler::flush_all_active_variables()
{
	// Invalidate all temporaries we read from variables in this block since they were forwarded.
	// Invalidate all temporaries we read from globals.
	for (auto &v : current_function->local_variables)
		flush_dependees(get<SPIRVariable>(v));
	for (auto &arg : current_function->arguments)
		flush_dependees(get<SPIRVariable>(arg.id));
	for (auto global : global_variables)
		flush_dependees(get<SPIRVariable>(global));

	flush_all_aliased_variables();
}

uint32_t Compiler::expression_type_id(uint32_t id) const
{
	switch (ir.ids[id].get_type())
	{
	case TypeVariable:
		return get<SPIRVariable>(id).basetype;

	case TypeExpression:
		return get<SPIRExpression>(id).expression_type;

	case TypeConstant:
		return get<SPIRConstant>(id).constant_type;

	case TypeConstantOp:
		return get<SPIRConstantOp>(id).basetype;

	case TypeUndef:
		return get<SPIRUndef>(id).basetype;

	case TypeCombinedImageSampler:
		return get<SPIRCombinedImageSampler>(id).combined_type;

	case TypeAccessChain:
		return get<SPIRAccessChain>(id).basetype;

	default:
		SPIRV_CROSS_THROW("Cannot resolve expression type.");
	}
}

const SPIRType &Compiler::expression_type(uint32_t id) const
{
	return get<SPIRType>(expression_type_id(id));
}

bool Compiler::expression_is_lvalue(uint32_t id) const
{
	auto &type = expression_type(id);
	switch (type.basetype)
	{
	case SPIRType::SampledImage:
	case SPIRType::Image:
	case SPIRType::Sampler:
		return false;

	default:
		return true;
	}
}

bool Compiler::is_immutable(uint32_t id) const
{
	if (ir.ids[id].get_type() == TypeVariable)
	{
		auto &var = get<SPIRVariable>(id);

		// Anything we load from the UniformConstant address space is guaranteed to be immutable.
		bool pointer_to_const = var.storage == StorageClassUniformConstant;
		return pointer_to_const || var.phi_variable || !expression_is_lvalue(id);
	}
	else if (ir.ids[id].get_type() == TypeAccessChain)
		return get<SPIRAccessChain>(id).immutable;
	else if (ir.ids[id].get_type() == TypeExpression)
		return get<SPIRExpression>(id).immutable;
	else if (ir.ids[id].get_type() == TypeConstant || ir.ids[id].get_type() == TypeConstantOp ||
	         ir.ids[id].get_type() == TypeUndef)
		return true;
	else
		return false;
}

static inline bool storage_class_is_interface(spv::StorageClass storage)
{
	switch (storage)
	{
	case StorageClassInput:
	case StorageClassOutput:
	case StorageClassUniform:
	case StorageClassUniformConstant:
	case StorageClassAtomicCounter:
	case StorageClassPushConstant:
	case StorageClassStorageBuffer:
		return true;

	default:
		return false;
	}
}

bool Compiler::is_hidden_variable(const SPIRVariable &var, bool include_builtins) const
{
	if ((is_builtin_variable(var) && !include_builtins) || var.remapped_variable)
		return true;

	// Combined image samplers are always considered active as they are "magic" variables.
	if (find_if(begin(combined_image_samplers), end(combined_image_samplers), [&var](const CombinedImageSampler &samp) {
		    return samp.combined_id == var.self;
	    }) != end(combined_image_samplers))
	{
		return false;
	}

	bool hidden = false;
	if (check_active_interface_variables && storage_class_is_interface(var.storage))
		hidden = active_interface_variables.find(var.self) == end(active_interface_variables);
	return hidden;
}

bool Compiler::is_builtin_type(const SPIRType &type) const
{
	auto *type_meta = ir.find_meta(type.self);

	// We can have builtin structs as well. If one member of a struct is builtin, the struct must also be builtin.
	if (type_meta)
		for (auto &m : type_meta->members)
			if (m.builtin)
				return true;

	return false;
}

bool Compiler::is_builtin_variable(const SPIRVariable &var) const
{
	auto *m = ir.find_meta(var.self);

	if (var.compat_builtin || (m && m->decoration.builtin))
		return true;
	else
		return is_builtin_type(get<SPIRType>(var.basetype));
}

bool Compiler::is_member_builtin(const SPIRType &type, uint32_t index, BuiltIn *builtin) const
{
	auto *type_meta = ir.find_meta(type.self);

	if (type_meta)
	{
		auto &memb = type_meta->members;
		if (index < memb.size() && memb[index].builtin)
		{
			if (builtin)
				*builtin = memb[index].builtin_type;
			return true;
		}
	}

	return false;
}

bool Compiler::is_scalar(const SPIRType &type) const
{
	return type.basetype != SPIRType::Struct && type.vecsize == 1 && type.columns == 1;
}

bool Compiler::is_vector(const SPIRType &type) const
{
	return type.vecsize > 1 && type.columns == 1;
}

bool Compiler::is_matrix(const SPIRType &type) const
{
	return type.vecsize > 1 && type.columns > 1;
}

bool Compiler::is_array(const SPIRType &type) const
{
	return !type.array.empty();
}

ShaderResources Compiler::get_shader_resources() const
{
	return get_shader_resources(nullptr);
}

ShaderResources Compiler::get_shader_resources(const unordered_set<uint32_t> &active_variables) const
{
	return get_shader_resources(&active_variables);
}

bool Compiler::InterfaceVariableAccessHandler::handle(Op opcode, const uint32_t *args, uint32_t length)
{
	uint32_t variable = 0;
	switch (opcode)
	{
	// Need this first, otherwise, GCC complains about unhandled switch statements.
	default:
		break;

	case OpFunctionCall:
	{
		// Invalid SPIR-V.
		if (length < 3)
			return false;

		uint32_t count = length - 3;
		args += 3;
		for (uint32_t i = 0; i < count; i++)
		{
			auto *var = compiler.maybe_get<SPIRVariable>(args[i]);
			if (var && storage_class_is_interface(var->storage))
				variables.insert(args[i]);
		}
		break;
	}

	case OpSelect:
	{
		// Invalid SPIR-V.
		if (length < 5)
			return false;

		uint32_t count = length - 3;
		args += 3;
		for (uint32_t i = 0; i < count; i++)
		{
			auto *var = compiler.maybe_get<SPIRVariable>(args[i]);
			if (var && storage_class_is_interface(var->storage))
				variables.insert(args[i]);
		}
		break;
	}

	case OpPhi:
	{
		// Invalid SPIR-V.
		if (length < 2)
			return false;

		uint32_t count = length - 2;
		args += 2;
		for (uint32_t i = 0; i < count; i += 2)
		{
			auto *var = compiler.maybe_get<SPIRVariable>(args[i]);
			if (var && storage_class_is_interface(var->storage))
				variables.insert(args[i]);
		}
		break;
	}

	case OpAtomicStore:
	case OpStore:
		// Invalid SPIR-V.
		if (length < 1)
			return false;
		variable = args[0];
		break;

	case OpCopyMemory:
	{
		if (length < 2)
			return false;

		auto *var = compiler.maybe_get<SPIRVariable>(args[0]);
		if (var && storage_class_is_interface(var->storage))
			variables.insert(variable);

		var = compiler.maybe_get<SPIRVariable>(args[1]);
		if (var && storage_class_is_interface(var->storage))
			variables.insert(variable);
		break;
	}

	case OpExtInst:
	{
		if (length < 5)
			return false;
		uint32_t extension_set = args[2];
		if (compiler.get<SPIRExtension>(extension_set).ext == SPIRExtension::SPV_AMD_shader_explicit_vertex_parameter)
		{
			enum AMDShaderExplicitVertexParameter
			{
				InterpolateAtVertexAMD = 1
			};

			auto op = static_cast<AMDShaderExplicitVertexParameter>(args[3]);

			switch (op)
			{
			case InterpolateAtVertexAMD:
			{
				auto *var = compiler.maybe_get<SPIRVariable>(args[4]);
				if (var && storage_class_is_interface(var->storage))
					variables.insert(args[4]);
				break;
			}

			default:
				break;
			}
		}
		break;
	}

	case OpAccessChain:
	case OpInBoundsAccessChain:
	case OpPtrAccessChain:
	case OpLoad:
	case OpCopyObject:
	case OpImageTexelPointer:
	case OpAtomicLoad:
	case OpAtomicExchange:
	case OpAtomicCompareExchange:
	case OpAtomicCompareExchangeWeak:
	case OpAtomicIIncrement:
	case OpAtomicIDecrement:
	case OpAtomicIAdd:
	case OpAtomicISub:
	case OpAtomicSMin:
	case OpAtomicUMin:
	case OpAtomicSMax:
	case OpAtomicUMax:
	case OpAtomicAnd:
	case OpAtomicOr:
	case OpAtomicXor:
		// Invalid SPIR-V.
		if (length < 3)
			return false;
		variable = args[2];
		break;
	}

	if (variable)
	{
		auto *var = compiler.maybe_get<SPIRVariable>(variable);
		if (var && storage_class_is_interface(var->storage))
			variables.insert(variable);
	}
	return true;
}

unordered_set<uint32_t> Compiler::get_active_interface_variables() const
{
	// Traverse the call graph and find all interface variables which are in use.
	unordered_set<uint32_t> variables;
	InterfaceVariableAccessHandler handler(*this, variables);
	traverse_all_reachable_opcodes(get<SPIRFunction>(ir.default_entry_point), handler);

	// Make sure we preserve output variables which are only initialized, but never accessed by any code.
	ir.for_each_typed_id<SPIRVariable>([&](uint32_t, const SPIRVariable &var) {
		if (var.storage == StorageClassOutput && var.initializer != 0)
			variables.insert(var.self);
	});

	// If we needed to create one, we'll need it.
	if (dummy_sampler_id)
		variables.insert(dummy_sampler_id);

	return variables;
}

void Compiler::set_enabled_interface_variables(std::unordered_set<uint32_t> active_variables)
{
	active_interface_variables = move(active_variables);
	check_active_interface_variables = true;
}

ShaderResources Compiler::get_shader_resources(const unordered_set<uint32_t> *active_variables) const
{
	ShaderResources res;

	ir.for_each_typed_id<SPIRVariable>([&](uint32_t, const SPIRVariable &var) {
		auto &type = this->get<SPIRType>(var.basetype);

		// It is possible for uniform storage classes to be passed as function parameters, so detect
		// that. To detect function parameters, check of StorageClass of variable is function scope.
		if (var.storage == StorageClassFunction || !type.pointer || is_builtin_variable(var))
			return;

		if (active_variables && active_variables->find(var.self) == end(*active_variables))
			return;

		// Input
		if (var.storage == StorageClassInput && interface_variable_exists_in_entry_point(var.self))
		{
			if (has_decoration(type.self, DecorationBlock))
			{
				res.stage_inputs.push_back(
				    { var.self, var.basetype, type.self, get_remapped_declared_block_name(var.self) });
			}
			else
				res.stage_inputs.push_back({ var.self, var.basetype, type.self, get_name(var.self) });
		}
		// Subpass inputs
		else if (var.storage == StorageClassUniformConstant && type.image.dim == DimSubpassData)
		{
			res.subpass_inputs.push_back({ var.self, var.basetype, type.self, get_name(var.self) });
		}
		// Outputs
		else if (var.storage == StorageClassOutput && interface_variable_exists_in_entry_point(var.self))
		{
			if (has_decoration(type.self, DecorationBlock))
			{
				res.stage_outputs.push_back(
				    { var.self, var.basetype, type.self, get_remapped_declared_block_name(var.self) });
			}
			else
				res.stage_outputs.push_back({ var.self, var.basetype, type.self, get_name(var.self) });
		}
		// UBOs
		else if (type.storage == StorageClassUniform && has_decoration(type.self, DecorationBlock))
		{
			res.uniform_buffers.push_back(
			    { var.self, var.basetype, type.self, get_remapped_declared_block_name(var.self) });
		}
		// Old way to declare SSBOs.
		else if (type.storage == StorageClassUniform && has_decoration(type.self, DecorationBufferBlock))
		{
			res.storage_buffers.push_back(
			    { var.self, var.basetype, type.self, get_remapped_declared_block_name(var.self) });
		}
		// Modern way to declare SSBOs.
		else if (type.storage == StorageClassStorageBuffer)
		{
			res.storage_buffers.push_back(
			    { var.self, var.basetype, type.self, get_remapped_declared_block_name(var.self) });
		}
		// Push constant blocks
		else if (type.storage == StorageClassPushConstant)
		{
			// There can only be one push constant block, but keep the vector in case this restriction is lifted
			// in the future.
			res.push_constant_buffers.push_back({ var.self, var.basetype, type.self, get_name(var.self) });
		}
		// Images
		else if (type.storage == StorageClassUniformConstant && type.basetype == SPIRType::Image &&
		         type.image.sampled == 2)
		{
			res.storage_images.push_back({ var.self, var.basetype, type.self, get_name(var.self) });
		}
		// Separate images
		else if (type.storage == StorageClassUniformConstant && type.basetype == SPIRType::Image &&
		         type.image.sampled == 1)
		{
			res.separate_images.push_back({ var.self, var.basetype, type.self, get_name(var.self) });
		}
		// Separate samplers
		else if (type.storage == StorageClassUniformConstant && type.basetype == SPIRType::Sampler)
		{
			res.separate_samplers.push_back({ var.self, var.basetype, type.self, get_name(var.self) });
		}
		// Textures
		else if (type.storage == StorageClassUniformConstant && type.basetype == SPIRType::SampledImage)
		{
			res.sampled_images.push_back({ var.self, var.basetype, type.self, get_name(var.self) });
		}
		// Atomic counters
		else if (type.storage == StorageClassAtomicCounter)
		{
			res.atomic_counters.push_back({ var.self, var.basetype, type.self, get_name(var.self) });
		}
		// Acceleration structures
		else if (type.storage == StorageClassUniformConstant && type.basetype == SPIRType::AccelerationStructureNV)
		{
			res.acceleration_structures.push_back({ var.self, var.basetype, type.self, get_name(var.self) });
		}
	});

	return res;
}

bool Compiler::type_is_block_like(const SPIRType &type) const
{
	if (type.basetype != SPIRType::Struct)
		return false;

	if (has_decoration(type.self, DecorationBlock) || has_decoration(type.self, DecorationBufferBlock))
	{
		return true;
	}

	// Block-like types may have Offset decorations.
	for (uint32_t i = 0; i < uint32_t(type.member_types.size()); i++)
		if (has_member_decoration(type.self, i, DecorationOffset))
			return true;

	return false;
}

void Compiler::fixup_type_alias()
{
	// Due to how some backends work, the "master" type of type_alias must be a block-like type if it exists.
	// FIXME: Multiple alias types which are both block-like will be awkward, for now, it's best to just drop the type
	// alias if the slave type is a block type.
	ir.for_each_typed_id<SPIRType>([&](uint32_t self, SPIRType &type) {
		if (type.type_alias && type_is_block_like(type))
		{
			// Become the master.
			ir.for_each_typed_id<SPIRType>([&](uint32_t other_id, SPIRType &other_type) {
				if (other_id == type.self)
					return;

				if (other_type.type_alias == type.type_alias)
					other_type.type_alias = type.self;
			});

			this->get<SPIRType>(type.type_alias).type_alias = self;
			type.type_alias = 0;
		}
	});

	ir.for_each_typed_id<SPIRType>([&](uint32_t, SPIRType &type) {
		if (type.type_alias && type_is_block_like(type))
		{
			// This is not allowed, drop the type_alias.
			type.type_alias = 0;
		}
	});

	// Reorder declaration of types so that the master of the type alias is always emitted first.
	// We need this in case a type B depends on type A (A must come before in the vector), but A is an alias of a type Abuffer, which
	// means declaration of A doesn't happen (yet), and order would be B, ABuffer and not ABuffer, B. Fix this up here.
	auto &type_ids = ir.ids_for_type[TypeType];
	for (auto alias_itr = begin(type_ids); alias_itr != end(type_ids); ++alias_itr)
	{
		auto &type = get<SPIRType>(*alias_itr);
		if (type.type_alias != 0 && !has_extended_decoration(type.type_alias, SPIRVCrossDecorationPacked))
		{
			// We will skip declaring this type, so make sure the type_alias type comes before.
			auto master_itr = find(begin(type_ids), end(type_ids), type.type_alias);
			assert(master_itr != end(type_ids));

			if (alias_itr < master_itr)
			{
				// Must also swap the type order for the constant-type joined array.
				auto &joined_types = ir.ids_for_constant_or_type;
				auto alt_alias_itr = find(begin(joined_types), end(joined_types), *alias_itr);
				auto alt_master_itr = find(begin(joined_types), end(joined_types), *master_itr);
				assert(alt_alias_itr != end(joined_types));
				assert(alt_master_itr != end(joined_types));

				swap(*alias_itr, *master_itr);
				swap(*alt_alias_itr, *alt_master_itr);
			}
		}
	}
}

void Compiler::parse_fixup()
{
	// Figure out specialization constants for work group sizes.
	for (auto id_ : ir.ids_for_constant_or_variable)
	{
		auto &id = ir.ids[id_];

		if (id.get_type() == TypeConstant)
		{
			auto &c = id.get<SPIRConstant>();
			if (ir.meta[c.self].decoration.builtin && ir.meta[c.self].decoration.builtin_type == BuiltInWorkgroupSize)
			{
				// In current SPIR-V, there can be just one constant like this.
				// All entry points will receive the constant value.
				for (auto &entry : ir.entry_points)
				{
					entry.second.workgroup_size.constant = c.self;
					entry.second.workgroup_size.x = c.scalar(0, 0);
					entry.second.workgroup_size.y = c.scalar(0, 1);
					entry.second.workgroup_size.z = c.scalar(0, 2);
				}
			}
		}
		else if (id.get_type() == TypeVariable)
		{
			auto &var = id.get<SPIRVariable>();
			if (var.storage == StorageClassPrivate || var.storage == StorageClassWorkgroup ||
			    var.storage == StorageClassOutput)
				global_variables.push_back(var.self);
			if (variable_storage_is_aliased(var))
				aliased_variables.push_back(var.self);
		}
	}

	fixup_type_alias();
}

void Compiler::update_name_cache(unordered_set<string> &cache_primary, const unordered_set<string> &cache_secondary,
                                 string &name)
{
	if (name.empty())
		return;

	const auto find_name = [&](const string &n) -> bool {
		if (cache_primary.find(n) != end(cache_primary))
			return true;

		if (&cache_primary != &cache_secondary)
			if (cache_secondary.find(n) != end(cache_secondary))
				return true;

		return false;
	};

	const auto insert_name = [&](const string &n) { cache_primary.insert(n); };

	if (!find_name(name))
	{
		insert_name(name);
		return;
	}

	uint32_t counter = 0;
	auto tmpname = name;

	bool use_linked_underscore = true;

	if (tmpname == "_")
	{
		// We cannot just append numbers, as we will end up creating internally reserved names.
		// Make it like _0_<counter> instead.
		tmpname += "0";
	}
	else if (tmpname.back() == '_')
	{
		// The last_character is an underscore, so we don't need to link in underscore.
		// This would violate double underscore rules.
		use_linked_underscore = false;
	}

	// If there is a collision (very rare),
	// keep tacking on extra identifier until it's unique.
	do
	{
		counter++;
		name = tmpname + (use_linked_underscore ? "_" : "") + convert_to_string(counter);
	} while (find_name(name));
	insert_name(name);
}

void Compiler::update_name_cache(unordered_set<string> &cache, string &name)
{
	update_name_cache(cache, cache, name);
}

void Compiler::set_name(uint32_t id, const std::string &name)
{
	ir.set_name(id, name);
}

const SPIRType &Compiler::get_type(uint32_t id) const
{
	return get<SPIRType>(id);
}

const SPIRType &Compiler::get_type_from_variable(uint32_t id) const
{
	return get<SPIRType>(get<SPIRVariable>(id).basetype);
}

uint32_t Compiler::get_pointee_type_id(uint32_t type_id) const
{
	auto *p_type = &get<SPIRType>(type_id);
	if (p_type->pointer)
	{
		assert(p_type->parent_type);
		type_id = p_type->parent_type;
	}
	return type_id;
}

const SPIRType &Compiler::get_pointee_type(const SPIRType &type) const
{
	auto *p_type = &type;
	if (p_type->pointer)
	{
		assert(p_type->parent_type);
		p_type = &get<SPIRType>(p_type->parent_type);
	}
	return *p_type;
}

const SPIRType &Compiler::get_pointee_type(uint32_t type_id) const
{
	return get_pointee_type(get<SPIRType>(type_id));
}

uint32_t Compiler::get_variable_data_type_id(const SPIRVariable &var) const
{
	if (var.phi_variable)
		return var.basetype;
	return get_pointee_type_id(var.basetype);
}

SPIRType &Compiler::get_variable_data_type(const SPIRVariable &var)
{
	return get<SPIRType>(get_variable_data_type_id(var));
}

const SPIRType &Compiler::get_variable_data_type(const SPIRVariable &var) const
{
	return get<SPIRType>(get_variable_data_type_id(var));
}

SPIRType &Compiler::get_variable_element_type(const SPIRVariable &var)
{
	SPIRType *type = &get_variable_data_type(var);
	if (is_array(*type))
		type = &get<SPIRType>(type->parent_type);
	return *type;
}

const SPIRType &Compiler::get_variable_element_type(const SPIRVariable &var) const
{
	const SPIRType *type = &get_variable_data_type(var);
	if (is_array(*type))
		type = &get<SPIRType>(type->parent_type);
	return *type;
}

bool Compiler::is_sampled_image_type(const SPIRType &type)
{
	return (type.basetype == SPIRType::Image || type.basetype == SPIRType::SampledImage) && type.image.sampled == 1 &&
	       type.image.dim != DimBuffer;
}

void Compiler::set_member_decoration_string(uint32_t id, uint32_t index, spv::Decoration decoration,
                                            const std::string &argument)
{
	ir.set_member_decoration_string(id, index, decoration, argument);
}

void Compiler::set_member_decoration(uint32_t id, uint32_t index, Decoration decoration, uint32_t argument)
{
	ir.set_member_decoration(id, index, decoration, argument);
}

void Compiler::set_member_name(uint32_t id, uint32_t index, const std::string &name)
{
	ir.set_member_name(id, index, name);
}

const std::string &Compiler::get_member_name(uint32_t id, uint32_t index) const
{
	return ir.get_member_name(id, index);
}

void Compiler::set_qualified_name(uint32_t id, const string &name)
{
	ir.meta[id].decoration.qualified_alias = name;
}

void Compiler::set_member_qualified_name(uint32_t type_id, uint32_t index, const std::string &name)
{
	ir.meta[type_id].members.resize(max(ir.meta[type_id].members.size(), size_t(index) + 1));
	ir.meta[type_id].members[index].qualified_alias = name;
}

const string &Compiler::get_member_qualified_name(uint32_t type_id, uint32_t index) const
{
	auto *m = ir.find_meta(type_id);
	if (m && index < m->members.size())
		return m->members[index].qualified_alias;
	else
		return ir.get_empty_string();
}

uint32_t Compiler::get_member_decoration(uint32_t id, uint32_t index, Decoration decoration) const
{
	return ir.get_member_decoration(id, index, decoration);
}

const Bitset &Compiler::get_member_decoration_bitset(uint32_t id, uint32_t index) const
{
	return ir.get_member_decoration_bitset(id, index);
}

bool Compiler::has_member_decoration(uint32_t id, uint32_t index, Decoration decoration) const
{
	return ir.has_member_decoration(id, index, decoration);
}

void Compiler::unset_member_decoration(uint32_t id, uint32_t index, Decoration decoration)
{
	ir.unset_member_decoration(id, index, decoration);
}

void Compiler::set_decoration_string(uint32_t id, spv::Decoration decoration, const std::string &argument)
{
	ir.set_decoration_string(id, decoration, argument);
}

void Compiler::set_decoration(uint32_t id, Decoration decoration, uint32_t argument)
{
	ir.set_decoration(id, decoration, argument);
}

void Compiler::set_extended_decoration(uint32_t id, ExtendedDecorations decoration, uint32_t value)
{
	auto &dec = ir.meta[id].decoration;
	switch (decoration)
	{
	case SPIRVCrossDecorationPacked:
		dec.extended.packed = true;
		break;

	case SPIRVCrossDecorationPackedType:
		dec.extended.packed_type = value;
		break;

	case SPIRVCrossDecorationInterfaceMemberIndex:
		dec.extended.ib_member_index = value;
		break;

	case SPIRVCrossDecorationInterfaceOrigID:
		dec.extended.ib_orig_id = value;
		break;

	case SPIRVCrossDecorationArgumentBufferID:
		dec.extended.argument_buffer_id = value;
		break;
	}
}

void Compiler::set_extended_member_decoration(uint32_t type, uint32_t index, ExtendedDecorations decoration,
                                              uint32_t value)
{
	ir.meta[type].members.resize(max(ir.meta[type].members.size(), size_t(index) + 1));
	auto &dec = ir.meta[type].members[index];

	switch (decoration)
	{
	case SPIRVCrossDecorationPacked:
		dec.extended.packed = true;
		break;

	case SPIRVCrossDecorationPackedType:
		dec.extended.packed_type = value;
		break;

	case SPIRVCrossDecorationInterfaceMemberIndex:
		dec.extended.ib_member_index = value;
		break;

	case SPIRVCrossDecorationInterfaceOrigID:
		dec.extended.ib_orig_id = value;
		break;

	case SPIRVCrossDecorationArgumentBufferID:
		dec.extended.argument_buffer_id = value;
		break;
	}
}

uint32_t Compiler::get_extended_decoration(uint32_t id, ExtendedDecorations decoration) const
{
	auto *m = ir.find_meta(id);
	if (!m)
		return 0;

	auto &dec = m->decoration;
	switch (decoration)
	{
	case SPIRVCrossDecorationPacked:
		return uint32_t(dec.extended.packed);

	case SPIRVCrossDecorationPackedType:
		return dec.extended.packed_type;

	case SPIRVCrossDecorationInterfaceMemberIndex:
		return dec.extended.ib_member_index;

	case SPIRVCrossDecorationInterfaceOrigID:
		return dec.extended.ib_orig_id;

	case SPIRVCrossDecorationArgumentBufferID:
		return dec.extended.argument_buffer_id;
	}

	return 0;
}

uint32_t Compiler::get_extended_member_decoration(uint32_t type, uint32_t index, ExtendedDecorations decoration) const
{
	auto *m = ir.find_meta(type);
	if (!m)
		return 0;

	if (index >= m->members.size())
		return 0;

	auto &dec = m->members[index];
	switch (decoration)
	{
	case SPIRVCrossDecorationPacked:
		return uint32_t(dec.extended.packed);

	case SPIRVCrossDecorationPackedType:
		return dec.extended.packed_type;

	case SPIRVCrossDecorationInterfaceMemberIndex:
		return dec.extended.ib_member_index;

	case SPIRVCrossDecorationInterfaceOrigID:
		return dec.extended.ib_orig_id;

	case SPIRVCrossDecorationArgumentBufferID:
		return dec.extended.argument_buffer_id;
	}

	return 0;
}

bool Compiler::has_extended_decoration(uint32_t id, ExtendedDecorations decoration) const
{
	auto *m = ir.find_meta(id);
	if (!m)
		return false;

	auto &dec = m->decoration;
	switch (decoration)
	{
	case SPIRVCrossDecorationPacked:
		return dec.extended.packed;

	case SPIRVCrossDecorationPackedType:
		return dec.extended.packed_type != 0;

	case SPIRVCrossDecorationInterfaceMemberIndex:
		return dec.extended.ib_member_index != uint32_t(-1);

	case SPIRVCrossDecorationInterfaceOrigID:
		return dec.extended.ib_orig_id != 0;

	case SPIRVCrossDecorationArgumentBufferID:
		return dec.extended.argument_buffer_id != 0;
	}

	return false;
}

bool Compiler::has_extended_member_decoration(uint32_t type, uint32_t index, ExtendedDecorations decoration) const
{
	auto *m = ir.find_meta(type);
	if (!m)
		return false;

	if (index >= m->members.size())
		return false;

	auto &dec = m->members[index];
	switch (decoration)
	{
	case SPIRVCrossDecorationPacked:
		return dec.extended.packed;

	case SPIRVCrossDecorationPackedType:
		return dec.extended.packed_type != 0;

	case SPIRVCrossDecorationInterfaceMemberIndex:
		return dec.extended.ib_member_index != uint32_t(-1);

	case SPIRVCrossDecorationInterfaceOrigID:
		return dec.extended.ib_orig_id != 0;

	case SPIRVCrossDecorationArgumentBufferID:
		return dec.extended.argument_buffer_id != uint32_t(-1);
	}

	return false;
}

void Compiler::unset_extended_decoration(uint32_t id, ExtendedDecorations decoration)
{
	auto &dec = ir.meta[id].decoration;
	switch (decoration)
	{
	case SPIRVCrossDecorationPacked:
		dec.extended.packed = false;
		break;

	case SPIRVCrossDecorationPackedType:
		dec.extended.packed_type = 0;
		break;

	case SPIRVCrossDecorationInterfaceMemberIndex:
		dec.extended.ib_member_index = ~(0u);
		break;

	case SPIRVCrossDecorationInterfaceOrigID:
		dec.extended.ib_orig_id = 0;
		break;

	case SPIRVCrossDecorationArgumentBufferID:
		dec.extended.argument_buffer_id = 0;
		break;
	}
}

void Compiler::unset_extended_member_decoration(uint32_t type, uint32_t index, ExtendedDecorations decoration)
{
	ir.meta[type].members.resize(max(ir.meta[type].members.size(), size_t(index) + 1));
	auto &dec = ir.meta[type].members[index];

	switch (decoration)
	{
	case SPIRVCrossDecorationPacked:
		dec.extended.packed = false;
		break;

	case SPIRVCrossDecorationPackedType:
		dec.extended.packed_type = 0;
		break;

	case SPIRVCrossDecorationInterfaceMemberIndex:
		dec.extended.ib_member_index = ~(0u);
		break;

	case SPIRVCrossDecorationInterfaceOrigID:
		dec.extended.ib_orig_id = 0;
		break;

	case SPIRVCrossDecorationArgumentBufferID:
		dec.extended.argument_buffer_id = 0;
		break;
	}
}

StorageClass Compiler::get_storage_class(uint32_t id) const
{
	return get<SPIRVariable>(id).storage;
}

const std::string &Compiler::get_name(uint32_t id) const
{
	return ir.get_name(id);
}

const std::string Compiler::get_fallback_name(uint32_t id) const
{
	return join("_", id);
}

const std::string Compiler::get_block_fallback_name(uint32_t id) const
{
	auto &var = get<SPIRVariable>(id);
	if (get_name(id).empty())
		return join("_", get<SPIRType>(var.basetype).self, "_", id);
	else
		return get_name(id);
}

const Bitset &Compiler::get_decoration_bitset(uint32_t id) const
{
	return ir.get_decoration_bitset(id);
}

bool Compiler::has_decoration(uint32_t id, Decoration decoration) const
{
	return ir.has_decoration(id, decoration);
}

const string &Compiler::get_decoration_string(uint32_t id, Decoration decoration) const
{
	return ir.get_decoration_string(id, decoration);
}

const string &Compiler::get_member_decoration_string(uint32_t id, uint32_t index, Decoration decoration) const
{
	return ir.get_member_decoration_string(id, index, decoration);
}

uint32_t Compiler::get_decoration(uint32_t id, Decoration decoration) const
{
	return ir.get_decoration(id, decoration);
}

void Compiler::unset_decoration(uint32_t id, Decoration decoration)
{
	ir.unset_decoration(id, decoration);
}

bool Compiler::get_binary_offset_for_decoration(uint32_t id, spv::Decoration decoration, uint32_t &word_offset) const
{
	auto *m = ir.find_meta(id);
	if (!m)
		return false;

	auto &word_offsets = m->decoration_word_offset;
	auto itr = word_offsets.find(decoration);
	if (itr == end(word_offsets))
		return false;

	word_offset = itr->second;
	return true;
}

bool Compiler::block_is_loop_candidate(const SPIRBlock &block, SPIRBlock::Method method) const
{
	// Tried and failed.
	if (block.disable_block_optimization || block.complex_continue)
		return false;

	if (method == SPIRBlock::MergeToSelectForLoop || method == SPIRBlock::MergeToSelectContinueForLoop)
	{
		// Try to detect common for loop pattern
		// which the code backend can use to create cleaner code.
		// for(;;) { if (cond) { some_body; } else { break; } }
		// is the pattern we're looking for.
		const auto *false_block = maybe_get<SPIRBlock>(block.false_block);
		const auto *true_block = maybe_get<SPIRBlock>(block.true_block);
		const auto *merge_block = maybe_get<SPIRBlock>(block.merge_block);

		bool false_block_is_merge = block.false_block == block.merge_block ||
		                            (false_block && merge_block && execution_is_noop(*false_block, *merge_block));

		bool true_block_is_merge = block.true_block == block.merge_block ||
		                           (true_block && merge_block && execution_is_noop(*true_block, *merge_block));

		bool positive_candidate =
		    block.true_block != block.merge_block && block.true_block != block.self && false_block_is_merge;

		bool negative_candidate =
		    block.false_block != block.merge_block && block.false_block != block.self && true_block_is_merge;

		bool ret = block.terminator == SPIRBlock::Select && block.merge == SPIRBlock::MergeLoop &&
		           (positive_candidate || negative_candidate);

		if (ret && positive_candidate && method == SPIRBlock::MergeToSelectContinueForLoop)
			ret = block.true_block == block.continue_block;
		else if (ret && negative_candidate && method == SPIRBlock::MergeToSelectContinueForLoop)
			ret = block.false_block == block.continue_block;

		// If we have OpPhi which depends on branches which came from our own block,
		// we need to flush phi variables in else block instead of a trivial break,
		// so we cannot assume this is a for loop candidate.
		if (ret)
		{
			for (auto &phi : block.phi_variables)
				if (phi.parent == block.self)
					return false;

			auto *merge = maybe_get<SPIRBlock>(block.merge_block);
			if (merge)
				for (auto &phi : merge->phi_variables)
					if (phi.parent == block.self)
						return false;
		}
		return ret;
	}
	else if (method == SPIRBlock::MergeToDirectForLoop)
	{
		// Empty loop header that just sets up merge target
		// and branches to loop body.
		bool ret = block.terminator == SPIRBlock::Direct && block.merge == SPIRBlock::MergeLoop && block.ops.empty();

		if (!ret)
			return false;

		auto &child = get<SPIRBlock>(block.next_block);

		const auto *false_block = maybe_get<SPIRBlock>(child.false_block);
		const auto *true_block = maybe_get<SPIRBlock>(child.true_block);
		const auto *merge_block = maybe_get<SPIRBlock>(block.merge_block);

		bool false_block_is_merge = child.false_block == block.merge_block ||
		                            (false_block && merge_block && execution_is_noop(*false_block, *merge_block));

		bool true_block_is_merge = child.true_block == block.merge_block ||
		                           (true_block && merge_block && execution_is_noop(*true_block, *merge_block));

		bool positive_candidate =
		    child.true_block != block.merge_block && child.true_block != block.self && false_block_is_merge;

		bool negative_candidate =
		    child.false_block != block.merge_block && child.false_block != block.self && true_block_is_merge;

		ret = child.terminator == SPIRBlock::Select && child.merge == SPIRBlock::MergeNone &&
		      (positive_candidate || negative_candidate);

		// If we have OpPhi which depends on branches which came from our own block,
		// we need to flush phi variables in else block instead of a trivial break,
		// so we cannot assume this is a for loop candidate.
		if (ret)
		{
			for (auto &phi : block.phi_variables)
				if (phi.parent == block.self || phi.parent == child.self)
					return false;

			for (auto &phi : child.phi_variables)
				if (phi.parent == block.self)
					return false;

			auto *merge = maybe_get<SPIRBlock>(block.merge_block);
			if (merge)
				for (auto &phi : merge->phi_variables)
					if (phi.parent == block.self || phi.parent == child.false_block)
						return false;
		}

		return ret;
	}
	else
		return false;
}

bool Compiler::block_is_outside_flow_control_from_block(const SPIRBlock &from, const SPIRBlock &to)
{
	auto *start = &from;

	if (start->self == to.self)
		return true;

	// Break cycles.
	if (is_continue(start->self))
		return false;

	// If our select block doesn't merge, we must break or continue in these blocks,
	// so if continues occur branchless within these blocks, consider them branchless as well.
	// This is typically used for loop control.
	if (start->terminator == SPIRBlock::Select && start->merge == SPIRBlock::MergeNone &&
	    (block_is_outside_flow_control_from_block(get<SPIRBlock>(start->true_block), to) ||
	     block_is_outside_flow_control_from_block(get<SPIRBlock>(start->false_block), to)))
	{
		return true;
	}
	else if (start->merge_block && block_is_outside_flow_control_from_block(get<SPIRBlock>(start->merge_block), to))
	{
		return true;
	}
	else if (start->next_block && block_is_outside_flow_control_from_block(get<SPIRBlock>(start->next_block), to))
	{
		return true;
	}
	else
		return false;
}

bool Compiler::execution_is_noop(const SPIRBlock &from, const SPIRBlock &to) const
{
	if (!execution_is_branchless(from, to))
		return false;

	auto *start = &from;
	for (;;)
	{
		if (start->self == to.self)
			return true;

		if (!start->ops.empty())
			return false;

		auto &next = get<SPIRBlock>(start->next_block);
		// Flushing phi variables does not count as noop.
		for (auto &phi : next.phi_variables)
			if (phi.parent == start->self)
				return false;

		start = &next;
	}
}

bool Compiler::execution_is_branchless(const SPIRBlock &from, const SPIRBlock &to) const
{
	auto *start = &from;
	for (;;)
	{
		if (start->self == to.self)
			return true;

		if (start->terminator == SPIRBlock::Direct && start->merge == SPIRBlock::MergeNone)
			start = &get<SPIRBlock>(start->next_block);
		else
			return false;
	}
}

SPIRBlock::ContinueBlockType Compiler::continue_block_type(const SPIRBlock &block) const
{
	// The block was deemed too complex during code emit, pick conservative fallback paths.
	if (block.complex_continue)
		return SPIRBlock::ComplexLoop;

	// In older glslang output continue block can be equal to the loop header.
	// In this case, execution is clearly branchless, so just assume a while loop header here.
	if (block.merge == SPIRBlock::MergeLoop)
		return SPIRBlock::WhileLoop;

	auto &dominator = get<SPIRBlock>(block.loop_dominator);

	if (execution_is_noop(block, dominator))
		return SPIRBlock::WhileLoop;
	else if (execution_is_branchless(block, dominator))
		return SPIRBlock::ForLoop;
	else
	{
		const auto *false_block = maybe_get<SPIRBlock>(block.false_block);
		const auto *true_block = maybe_get<SPIRBlock>(block.true_block);
		const auto *merge_block = maybe_get<SPIRBlock>(dominator.merge_block);

		bool positive_do_while = block.true_block == dominator.self &&
		                         (block.false_block == dominator.merge_block ||
		                          (false_block && merge_block && execution_is_noop(*false_block, *merge_block)));

		bool negative_do_while = block.false_block == dominator.self &&
		                         (block.true_block == dominator.merge_block ||
		                          (true_block && merge_block && execution_is_noop(*true_block, *merge_block)));

		if (block.merge == SPIRBlock::MergeNone && block.terminator == SPIRBlock::Select &&
		    (positive_do_while || negative_do_while))
		{
			return SPIRBlock::DoWhileLoop;
		}
		else
			return SPIRBlock::ComplexLoop;
	}
}

bool Compiler::traverse_all_reachable_opcodes(const SPIRBlock &block, OpcodeHandler &handler) const
{
	handler.set_current_block(block);

	// Ideally, perhaps traverse the CFG instead of all blocks in order to eliminate dead blocks,
	// but this shouldn't be a problem in practice unless the SPIR-V is doing insane things like recursing
	// inside dead blocks ...
	for (auto &i : block.ops)
	{
		auto ops = stream(i);
		auto op = static_cast<Op>(i.op);

		if (!handler.handle(op, ops, i.length))
			return false;

		if (op == OpFunctionCall)
		{
			auto &func = get<SPIRFunction>(ops[2]);
			if (handler.follow_function_call(func))
			{
				if (!handler.begin_function_scope(ops, i.length))
					return false;
				if (!traverse_all_reachable_opcodes(get<SPIRFunction>(ops[2]), handler))
					return false;
				if (!handler.end_function_scope(ops, i.length))
					return false;
			}
		}
	}

	return true;
}

bool Compiler::traverse_all_reachable_opcodes(const SPIRFunction &func, OpcodeHandler &handler) const
{
	for (auto block : func.blocks)
		if (!traverse_all_reachable_opcodes(get<SPIRBlock>(block), handler))
			return false;

	return true;
}

uint32_t Compiler::type_struct_member_offset(const SPIRType &type, uint32_t index) const
{
	auto *type_meta = ir.find_meta(type.self);
	if (type_meta)
	{
		// Decoration must be set in valid SPIR-V, otherwise throw.
		auto &dec = type_meta->members[index];
		if (dec.decoration_flags.get(DecorationOffset))
			return dec.offset;
		else
			SPIRV_CROSS_THROW("Struct member does not have Offset set.");
	}
	else
		SPIRV_CROSS_THROW("Struct member does not have Offset set.");
}

uint32_t Compiler::type_struct_member_array_stride(const SPIRType &type, uint32_t index) const
{
	auto *type_meta = ir.find_meta(type.member_types[index]);
	if (type_meta)
	{
		// Decoration must be set in valid SPIR-V, otherwise throw.
		// ArrayStride is part of the array type not OpMemberDecorate.
		auto &dec = type_meta->decoration;
		if (dec.decoration_flags.get(DecorationArrayStride))
			return dec.array_stride;
		else
			SPIRV_CROSS_THROW("Struct member does not have ArrayStride set.");
	}
	else
		SPIRV_CROSS_THROW("Struct member does not have ArrayStride set.");
}

uint32_t Compiler::type_struct_member_matrix_stride(const SPIRType &type, uint32_t index) const
{
	auto *type_meta = ir.find_meta(type.self);
	if (type_meta)
	{
		// Decoration must be set in valid SPIR-V, otherwise throw.
		// MatrixStride is part of OpMemberDecorate.
		auto &dec = type_meta->members[index];
		if (dec.decoration_flags.get(DecorationMatrixStride))
			return dec.matrix_stride;
		else
			SPIRV_CROSS_THROW("Struct member does not have MatrixStride set.");
	}
	else
		SPIRV_CROSS_THROW("Struct member does not have MatrixStride set.");
}

size_t Compiler::get_declared_struct_size(const SPIRType &type) const
{
	if (type.member_types.empty())
		SPIRV_CROSS_THROW("Declared struct in block cannot be empty.");

	uint32_t last = uint32_t(type.member_types.size() - 1);
	size_t offset = type_struct_member_offset(type, last);
	size_t size = get_declared_struct_member_size(type, last);
	return offset + size;
}

size_t Compiler::get_declared_struct_size_runtime_array(const SPIRType &type, size_t array_size) const
{
	if (type.member_types.empty())
		SPIRV_CROSS_THROW("Declared struct in block cannot be empty.");

	size_t size = get_declared_struct_size(type);
	auto &last_type = get<SPIRType>(type.member_types.back());
	if (!last_type.array.empty() && last_type.array_size_literal[0] && last_type.array[0] == 0) // Runtime array
		size += array_size * type_struct_member_array_stride(type, uint32_t(type.member_types.size() - 1));

	return size;
}

size_t Compiler::get_declared_struct_member_size(const SPIRType &struct_type, uint32_t index) const
{
	if (struct_type.member_types.empty())
		SPIRV_CROSS_THROW("Declared struct in block cannot be empty.");

	auto &flags = get_member_decoration_bitset(struct_type.self, index);
	auto &type = get<SPIRType>(struct_type.member_types[index]);

	switch (type.basetype)
	{
	case SPIRType::Unknown:
	case SPIRType::Void:
	case SPIRType::Boolean: // Bools are purely logical, and cannot be used for externally visible types.
	case SPIRType::AtomicCounter:
	case SPIRType::Image:
	case SPIRType::SampledImage:
	case SPIRType::Sampler:
		SPIRV_CROSS_THROW("Querying size for object with opaque size.");

	default:
		break;
	}

	if (!type.array.empty())
	{
		// For arrays, we can use ArrayStride to get an easy check.
		bool array_size_literal = type.array_size_literal.back();
		uint32_t array_size = array_size_literal ? type.array.back() : get<SPIRConstant>(type.array.back()).scalar();
		return type_struct_member_array_stride(struct_type, index) * array_size;
	}
	else if (type.basetype == SPIRType::Struct)
	{
		return get_declared_struct_size(type);
	}
	else
	{
		unsigned vecsize = type.vecsize;
		unsigned columns = type.columns;

		// Vectors.
		if (columns == 1)
		{
			size_t component_size = type.width / 8;
			return vecsize * component_size;
		}
		else
		{
			uint32_t matrix_stride = type_struct_member_matrix_stride(struct_type, index);

			// Per SPIR-V spec, matrices must be tightly packed and aligned up for vec3 accesses.
			if (flags.get(DecorationRowMajor))
				return matrix_stride * vecsize;
			else if (flags.get(DecorationColMajor))
				return matrix_stride * columns;
			else
				SPIRV_CROSS_THROW("Either row-major or column-major must be declared for matrices.");
		}
	}
}

bool Compiler::BufferAccessHandler::handle(Op opcode, const uint32_t *args, uint32_t length)
{
	if (opcode != OpAccessChain && opcode != OpInBoundsAccessChain && opcode != OpPtrAccessChain)
		return true;

	bool ptr_chain = (opcode == OpPtrAccessChain);

	// Invalid SPIR-V.
	if (length < (ptr_chain ? 5u : 4u))
		return false;

	if (args[2] != id)
		return true;

	// Don't bother traversing the entire access chain tree yet.
	// If we access a struct member, assume we access the entire member.
	uint32_t index = compiler.get<SPIRConstant>(args[ptr_chain ? 4 : 3]).scalar();

	// Seen this index already.
	if (seen.find(index) != end(seen))
		return true;
	seen.insert(index);

	auto &type = compiler.expression_type(id);
	uint32_t offset = compiler.type_struct_member_offset(type, index);

	size_t range;
	// If we have another member in the struct, deduce the range by looking at the next member.
	// This is okay since structs in SPIR-V can have padding, but Offset decoration must be
	// monotonically increasing.
	// Of course, this doesn't take into account if the SPIR-V for some reason decided to add
	// very large amounts of padding, but that's not really a big deal.
	if (index + 1 < type.member_types.size())
	{
		range = compiler.type_struct_member_offset(type, index + 1) - offset;
	}
	else
	{
		// No padding, so just deduce it from the size of the member directly.
		range = compiler.get_declared_struct_member_size(type, index);
	}

	ranges.push_back({ index, offset, range });
	return true;
}

SmallVector<BufferRange> Compiler::get_active_buffer_ranges(uint32_t id) const
{
	SmallVector<BufferRange> ranges;
	BufferAccessHandler handler(*this, ranges, id);
	traverse_all_reachable_opcodes(get<SPIRFunction>(ir.default_entry_point), handler);
	return ranges;
}

bool Compiler::types_are_logically_equivalent(const SPIRType &a, const SPIRType &b) const
{
	if (a.basetype != b.basetype)
		return false;
	if (a.width != b.width)
		return false;
	if (a.vecsize != b.vecsize)
		return false;
	if (a.columns != b.columns)
		return false;
	if (a.array.size() != b.array.size())
		return false;

	size_t array_count = a.array.size();
	if (array_count && memcmp(a.array.data(), b.array.data(), array_count * sizeof(uint32_t)) != 0)
		return false;

	if (a.basetype == SPIRType::Image || a.basetype == SPIRType::SampledImage)
	{
		if (memcmp(&a.image, &b.image, sizeof(SPIRType::Image)) != 0)
			return false;
	}

	if (a.member_types.size() != b.member_types.size())
		return false;

	size_t member_types = a.member_types.size();
	for (size_t i = 0; i < member_types; i++)
	{
		if (!types_are_logically_equivalent(get<SPIRType>(a.member_types[i]), get<SPIRType>(b.member_types[i])))
			return false;
	}

	return true;
}

const Bitset &Compiler::get_execution_mode_bitset() const
{
	return get_entry_point().flags;
}

void Compiler::set_execution_mode(ExecutionMode mode, uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
	auto &execution = get_entry_point();

	execution.flags.set(mode);
	switch (mode)
	{
	case ExecutionModeLocalSize:
		execution.workgroup_size.x = arg0;
		execution.workgroup_size.y = arg1;
		execution.workgroup_size.z = arg2;
		break;

	case ExecutionModeInvocations:
		execution.invocations = arg0;
		break;

	case ExecutionModeOutputVertices:
		execution.output_vertices = arg0;
		break;

	default:
		break;
	}
}

void Compiler::unset_execution_mode(ExecutionMode mode)
{
	auto &execution = get_entry_point();
	execution.flags.clear(mode);
}

uint32_t Compiler::get_work_group_size_specialization_constants(SpecializationConstant &x, SpecializationConstant &y,
                                                                SpecializationConstant &z) const
{
	auto &execution = get_entry_point();
	x = { 0, 0 };
	y = { 0, 0 };
	z = { 0, 0 };

	if (execution.workgroup_size.constant != 0)
	{
		auto &c = get<SPIRConstant>(execution.workgroup_size.constant);

		if (c.m.c[0].id[0] != 0)
		{
			x.id = c.m.c[0].id[0];
			x.constant_id = get_decoration(c.m.c[0].id[0], DecorationSpecId);
		}

		if (c.m.c[0].id[1] != 0)
		{
			y.id = c.m.c[0].id[1];
			y.constant_id = get_decoration(c.m.c[0].id[1], DecorationSpecId);
		}

		if (c.m.c[0].id[2] != 0)
		{
			z.id = c.m.c[0].id[2];
			z.constant_id = get_decoration(c.m.c[0].id[2], DecorationSpecId);
		}
	}

	return execution.workgroup_size.constant;
}

uint32_t Compiler::get_execution_mode_argument(spv::ExecutionMode mode, uint32_t index) const
{
	auto &execution = get_entry_point();
	switch (mode)
	{
	case ExecutionModeLocalSize:
		switch (index)
		{
		case 0:
			return execution.workgroup_size.x;
		case 1:
			return execution.workgroup_size.y;
		case 2:
			return execution.workgroup_size.z;
		default:
			return 0;
		}

	case ExecutionModeInvocations:
		return execution.invocations;

	case ExecutionModeOutputVertices:
		return execution.output_vertices;

	default:
		return 0;
	}
}

ExecutionModel Compiler::get_execution_model() const
{
	auto &execution = get_entry_point();
	return execution.model;
}

bool Compiler::is_tessellation_shader(ExecutionModel model)
{
	return model == ExecutionModelTessellationControl || model == ExecutionModelTessellationEvaluation;
}

bool Compiler::is_tessellation_shader() const
{
	return is_tessellation_shader(get_execution_model());
}

void Compiler::set_remapped_variable_state(uint32_t id, bool remap_enable)
{
	get<SPIRVariable>(id).remapped_variable = remap_enable;
}

bool Compiler::get_remapped_variable_state(uint32_t id) const
{
	return get<SPIRVariable>(id).remapped_variable;
}

void Compiler::set_subpass_input_remapped_components(uint32_t id, uint32_t components)
{
	get<SPIRVariable>(id).remapped_components = components;
}

uint32_t Compiler::get_subpass_input_remapped_components(uint32_t id) const
{
	return get<SPIRVariable>(id).remapped_components;
}

void Compiler::add_implied_read_expression(SPIRExpression &e, uint32_t source)
{
	auto itr = find(begin(e.implied_read_expressions), end(e.implied_read_expressions), source);
	if (itr == end(e.implied_read_expressions))
		e.implied_read_expressions.push_back(source);
}

void Compiler::add_implied_read_expression(SPIRAccessChain &e, uint32_t source)
{
	auto itr = find(begin(e.implied_read_expressions), end(e.implied_read_expressions), source);
	if (itr == end(e.implied_read_expressions))
		e.implied_read_expressions.push_back(source);
}

void Compiler::inherit_expression_dependencies(uint32_t dst, uint32_t source_expression)
{
	// Don't inherit any expression dependencies if the expression in dst
	// is not a forwarded temporary.
	if (forwarded_temporaries.find(dst) == end(forwarded_temporaries) ||
	    forced_temporaries.find(dst) != end(forced_temporaries))
	{
		return;
	}

	auto &e = get<SPIRExpression>(dst);
	auto *phi = maybe_get<SPIRVariable>(source_expression);
	if (phi && phi->phi_variable)
	{
		// We have used a phi variable, which can change at the end of the block,
		// so make sure we take a dependency on this phi variable.
		phi->dependees.push_back(dst);
	}

	auto *s = maybe_get<SPIRExpression>(source_expression);
	if (!s)
		return;

	auto &e_deps = e.expression_dependencies;
	auto &s_deps = s->expression_dependencies;

	// If we depend on a expression, we also depend on all sub-dependencies from source.
	e_deps.push_back(source_expression);
	e_deps.insert(end(e_deps), begin(s_deps), end(s_deps));

	// Eliminate duplicated dependencies.
	sort(begin(e_deps), end(e_deps));
	e_deps.erase(unique(begin(e_deps), end(e_deps)), end(e_deps));
}

SmallVector<EntryPoint> Compiler::get_entry_points_and_stages() const
{
	SmallVector<EntryPoint> entries;
	for (auto &entry : ir.entry_points)
		entries.push_back({ entry.second.orig_name, entry.second.model });
	return entries;
}

void Compiler::rename_entry_point(const std::string &old_name, const std::string &new_name, spv::ExecutionModel model)
{
	auto &entry = get_entry_point(old_name, model);
	entry.orig_name = new_name;
	entry.name = new_name;
}

void Compiler::set_entry_point(const std::string &name, spv::ExecutionModel model)
{
	auto &entry = get_entry_point(name, model);
	ir.default_entry_point = entry.self;
}

SPIREntryPoint &Compiler::get_first_entry_point(const std::string &name)
{
	auto itr = find_if(
	    begin(ir.entry_points), end(ir.entry_points),
	    [&](const std::pair<uint32_t, SPIREntryPoint> &entry) -> bool { return entry.second.orig_name == name; });

	if (itr == end(ir.entry_points))
		SPIRV_CROSS_THROW("Entry point does not exist.");

	return itr->second;
}

const SPIREntryPoint &Compiler::get_first_entry_point(const std::string &name) const
{
	auto itr = find_if(
	    begin(ir.entry_points), end(ir.entry_points),
	    [&](const std::pair<uint32_t, SPIREntryPoint> &entry) -> bool { return entry.second.orig_name == name; });

	if (itr == end(ir.entry_points))
		SPIRV_CROSS_THROW("Entry point does not exist.");

	return itr->second;
}

SPIREntryPoint &Compiler::get_entry_point(const std::string &name, ExecutionModel model)
{
	auto itr = find_if(begin(ir.entry_points), end(ir.entry_points),
	                   [&](const std::pair<uint32_t, SPIREntryPoint> &entry) -> bool {
		                   return entry.second.orig_name == name && entry.second.model == model;
	                   });

	if (itr == end(ir.entry_points))
		SPIRV_CROSS_THROW("Entry point does not exist.");

	return itr->second;
}

const SPIREntryPoint &Compiler::get_entry_point(const std::string &name, ExecutionModel model) const
{
	auto itr = find_if(begin(ir.entry_points), end(ir.entry_points),
	                   [&](const std::pair<uint32_t, SPIREntryPoint> &entry) -> bool {
		                   return entry.second.orig_name == name && entry.second.model == model;
	                   });

	if (itr == end(ir.entry_points))
		SPIRV_CROSS_THROW("Entry point does not exist.");

	return itr->second;
}

const string &Compiler::get_cleansed_entry_point_name(const std::string &name, ExecutionModel model) const
{
	return get_entry_point(name, model).name;
}

const SPIREntryPoint &Compiler::get_entry_point() const
{
	return ir.entry_points.find(ir.default_entry_point)->second;
}

SPIREntryPoint &Compiler::get_entry_point()
{
	return ir.entry_points.find(ir.default_entry_point)->second;
}

bool Compiler::interface_variable_exists_in_entry_point(uint32_t id) const
{
	auto &var = get<SPIRVariable>(id);
	if (var.storage != StorageClassInput && var.storage != StorageClassOutput &&
	    var.storage != StorageClassUniformConstant)
		SPIRV_CROSS_THROW("Only Input, Output variables and Uniform constants are part of a shader linking interface.");

	// This is to avoid potential problems with very old glslang versions which did
	// not emit input/output interfaces properly.
	// We can assume they only had a single entry point, and single entry point
	// shaders could easily be assumed to use every interface variable anyways.
	if (ir.entry_points.size() <= 1)
		return true;

	auto &execution = get_entry_point();
	return find(begin(execution.interface_variables), end(execution.interface_variables), id) !=
	       end(execution.interface_variables);
}

void Compiler::CombinedImageSamplerHandler::push_remap_parameters(const SPIRFunction &func, const uint32_t *args,
                                                                  uint32_t length)
{
	// If possible, pipe through a remapping table so that parameters know
	// which variables they actually bind to in this scope.
	unordered_map<uint32_t, uint32_t> remapping;
	for (uint32_t i = 0; i < length; i++)
		remapping[func.arguments[i].id] = remap_parameter(args[i]);
	parameter_remapping.push(move(remapping));
}

void Compiler::CombinedImageSamplerHandler::pop_remap_parameters()
{
	parameter_remapping.pop();
}

uint32_t Compiler::CombinedImageSamplerHandler::remap_parameter(uint32_t id)
{
	auto *var = compiler.maybe_get_backing_variable(id);
	if (var)
		id = var->self;

	if (parameter_remapping.empty())
		return id;

	auto &remapping = parameter_remapping.top();
	auto itr = remapping.find(id);
	if (itr != end(remapping))
		return itr->second;
	else
		return id;
}

bool Compiler::CombinedImageSamplerHandler::begin_function_scope(const uint32_t *args, uint32_t length)
{
	if (length < 3)
		return false;

	auto &callee = compiler.get<SPIRFunction>(args[2]);
	args += 3;
	length -= 3;
	push_remap_parameters(callee, args, length);
	functions.push(&callee);
	return true;
}

bool Compiler::CombinedImageSamplerHandler::end_function_scope(const uint32_t *args, uint32_t length)
{
	if (length < 3)
		return false;

	auto &callee = compiler.get<SPIRFunction>(args[2]);
	args += 3;

	// There are two types of cases we have to handle,
	// a callee might call sampler2D(texture2D, sampler) directly where
	// one or more parameters originate from parameters.
	// Alternatively, we need to provide combined image samplers to our callees,
	// and in this case we need to add those as well.

	pop_remap_parameters();

	// Our callee has now been processed at least once.
	// No point in doing it again.
	callee.do_combined_parameters = false;

	auto &params = functions.top()->combined_parameters;
	functions.pop();
	if (functions.empty())
		return true;

	auto &caller = *functions.top();
	if (caller.do_combined_parameters)
	{
		for (auto &param : params)
		{
			uint32_t image_id = param.global_image ? param.image_id : args[param.image_id];
			uint32_t sampler_id = param.global_sampler ? param.sampler_id : args[param.sampler_id];

			auto *i = compiler.maybe_get_backing_variable(image_id);
			auto *s = compiler.maybe_get_backing_variable(sampler_id);
			if (i)
				image_id = i->self;
			if (s)
				sampler_id = s->self;

			register_combined_image_sampler(caller, image_id, sampler_id, param.depth);
		}
	}

	return true;
}

void Compiler::CombinedImageSamplerHandler::register_combined_image_sampler(SPIRFunction &caller, uint32_t image_id,
                                                                            uint32_t sampler_id, bool depth)
{
	// We now have a texture ID and a sampler ID which will either be found as a global
	// or a parameter in our own function. If both are global, they will not need a parameter,
	// otherwise, add it to our list.
	SPIRFunction::CombinedImageSamplerParameter param = {
		0u, image_id, sampler_id, true, true, depth,
	};

	auto texture_itr = find_if(begin(caller.arguments), end(caller.arguments),
	                           [image_id](const SPIRFunction::Parameter &p) { return p.id == image_id; });
	auto sampler_itr = find_if(begin(caller.arguments), end(caller.arguments),
	                           [sampler_id](const SPIRFunction::Parameter &p) { return p.id == sampler_id; });

	if (texture_itr != end(caller.arguments))
	{
		param.global_image = false;
		param.image_id = uint32_t(texture_itr - begin(caller.arguments));
	}

	if (sampler_itr != end(caller.arguments))
	{
		param.global_sampler = false;
		param.sampler_id = uint32_t(sampler_itr - begin(caller.arguments));
	}

	if (param.global_image && param.global_sampler)
		return;

	auto itr = find_if(begin(caller.combined_parameters), end(caller.combined_parameters),
	                   [&param](const SPIRFunction::CombinedImageSamplerParameter &p) {
		                   return param.image_id == p.image_id && param.sampler_id == p.sampler_id &&
		                          param.global_image == p.global_image && param.global_sampler == p.global_sampler;
	                   });

	if (itr == end(caller.combined_parameters))
	{
		uint32_t id = compiler.ir.increase_bound_by(3);
		auto type_id = id + 0;
		auto ptr_type_id = id + 1;
		auto combined_id = id + 2;
		auto &base = compiler.expression_type(image_id);
		auto &type = compiler.set<SPIRType>(type_id);
		auto &ptr_type = compiler.set<SPIRType>(ptr_type_id);

		type = base;
		type.self = type_id;
		type.basetype = SPIRType::SampledImage;
		type.pointer = false;
		type.storage = StorageClassGeneric;
		type.image.depth = depth;

		ptr_type = type;
		ptr_type.pointer = true;
		ptr_type.storage = StorageClassUniformConstant;
		ptr_type.parent_type = type_id;

		// Build new variable.
		compiler.set<SPIRVariable>(combined_id, ptr_type_id, StorageClassFunction, 0);

		// Inherit RelaxedPrecision (and potentially other useful flags if deemed relevant).
		auto &new_flags = compiler.ir.meta[combined_id].decoration.decoration_flags;
		auto &old_flags = compiler.ir.meta[sampler_id].decoration.decoration_flags;
		new_flags.reset();
		if (old_flags.get(DecorationRelaxedPrecision))
			new_flags.set(DecorationRelaxedPrecision);

		param.id = combined_id;

		compiler.set_name(combined_id,
		                  join("SPIRV_Cross_Combined", compiler.to_name(image_id), compiler.to_name(sampler_id)));

		caller.combined_parameters.push_back(param);
		caller.shadow_arguments.push_back({ ptr_type_id, combined_id, 0u, 0u, true });
	}
}

bool Compiler::DummySamplerForCombinedImageHandler::handle(Op opcode, const uint32_t *args, uint32_t length)
{
	if (need_dummy_sampler)
	{
		// No need to traverse further, we know the result.
		return false;
	}

	switch (opcode)
	{
	case OpLoad:
	{
		if (length < 3)
			return false;

		uint32_t result_type = args[0];

		auto &type = compiler.get<SPIRType>(result_type);
		bool separate_image =
		    type.basetype == SPIRType::Image && type.image.sampled == 1 && type.image.dim != DimBuffer;

		// If not separate image, don't bother.
		if (!separate_image)
			return true;

		uint32_t id = args[1];
		uint32_t ptr = args[2];
		compiler.set<SPIRExpression>(id, "", result_type, true);
		compiler.register_read(id, ptr, true);
		break;
	}

	case OpImageFetch:
	case OpImageQuerySizeLod:
	case OpImageQuerySize:
	case OpImageQueryLevels:
	case OpImageQuerySamples:
	{
		// If we are fetching or querying LOD from a plain OpTypeImage, we must pre-combine with our dummy sampler.
		auto *var = compiler.maybe_get_backing_variable(args[2]);
		if (var)
		{
			auto &type = compiler.get<SPIRType>(var->basetype);
			if (type.basetype == SPIRType::Image && type.image.sampled == 1 && type.image.dim != DimBuffer)
				need_dummy_sampler = true;
		}

		break;
	}

	case OpInBoundsAccessChain:
	case OpAccessChain:
	case OpPtrAccessChain:
	{
		if (length < 3)
			return false;

		uint32_t result_type = args[0];
		auto &type = compiler.get<SPIRType>(result_type);
		bool separate_image =
		    type.basetype == SPIRType::Image && type.image.sampled == 1 && type.image.dim != DimBuffer;
		if (!separate_image)
			return true;

		uint32_t id = args[1];
		uint32_t ptr = args[2];
		compiler.set<SPIRExpression>(id, "", result_type, true);
		compiler.register_read(id, ptr, true);

		// Other backends might use SPIRAccessChain for this later.
		compiler.ir.ids[id].set_allow_type_rewrite();
		break;
	}

	default:
		break;
	}

	return true;
}

bool Compiler::CombinedImageSamplerHandler::handle(Op opcode, const uint32_t *args, uint32_t length)
{
	// We need to figure out where samplers and images are loaded from, so do only the bare bones compilation we need.
	bool is_fetch = false;

	switch (opcode)
	{
	case OpLoad:
	{
		if (length < 3)
			return false;

		uint32_t result_type = args[0];

		auto &type = compiler.get<SPIRType>(result_type);
		bool separate_image = type.basetype == SPIRType::Image && type.image.sampled == 1;
		bool separate_sampler = type.basetype == SPIRType::Sampler;

		// If not separate image or sampler, don't bother.
		if (!separate_image && !separate_sampler)
			return true;

		uint32_t id = args[1];
		uint32_t ptr = args[2];
		compiler.set<SPIRExpression>(id, "", result_type, true);
		compiler.register_read(id, ptr, true);
		return true;
	}

	case OpInBoundsAccessChain:
	case OpAccessChain:
	case OpPtrAccessChain:
	{
		if (length < 3)
			return false;

		// Technically, it is possible to have arrays of textures and arrays of samplers and combine them, but this becomes essentially
		// impossible to implement, since we don't know which concrete sampler we are accessing.
		// One potential way is to create a combinatorial explosion where N textures and M samplers are combined into N * M sampler2Ds,
		// but this seems ridiculously complicated for a problem which is easy to work around.
		// Checking access chains like this assumes we don't have samplers or textures inside uniform structs, but this makes no sense.

		uint32_t result_type = args[0];

		auto &type = compiler.get<SPIRType>(result_type);
		bool separate_image = type.basetype == SPIRType::Image && type.image.sampled == 1;
		bool separate_sampler = type.basetype == SPIRType::Sampler;
		if (separate_sampler)
			SPIRV_CROSS_THROW(
			    "Attempting to use arrays or structs of separate samplers. This is not possible to statically "
			    "remap to plain GLSL.");

		if (separate_image)
		{
			uint32_t id = args[1];
			uint32_t ptr = args[2];
			compiler.set<SPIRExpression>(id, "", result_type, true);
			compiler.register_read(id, ptr, true);
		}
		return true;
	}

	case OpImageFetch:
	case OpImageQuerySizeLod:
	case OpImageQuerySize:
	case OpImageQueryLevels:
	case OpImageQuerySamples:
	{
		// If we are fetching from a plain OpTypeImage or querying LOD, we must pre-combine with our dummy sampler.
		auto *var = compiler.maybe_get_backing_variable(args[2]);
		if (!var)
			return true;

		auto &type = compiler.get<SPIRType>(var->basetype);
		if (type.basetype == SPIRType::Image && type.image.sampled == 1 && type.image.dim != DimBuffer)
		{
			if (compiler.dummy_sampler_id == 0)
				SPIRV_CROSS_THROW("texelFetch without sampler was found, but no dummy sampler has been created with "
				                  "build_dummy_sampler_for_combined_images().");

			// Do it outside.
			is_fetch = true;
			break;
		}

		return true;
	}

	case OpSampledImage:
		// Do it outside.
		break;

	default:
		return true;
	}

	// Registers sampler2D calls used in case they are parameters so
	// that their callees know which combined image samplers to propagate down the call stack.
	if (!functions.empty())
	{
		auto &callee = *functions.top();
		if (callee.do_combined_parameters)
		{
			uint32_t image_id = args[2];

			auto *image = compiler.maybe_get_backing_variable(image_id);
			if (image)
				image_id = image->self;

			uint32_t sampler_id = is_fetch ? compiler.dummy_sampler_id : args[3];
			auto *sampler = compiler.maybe_get_backing_variable(sampler_id);
			if (sampler)
				sampler_id = sampler->self;

			auto &combined_type = compiler.get<SPIRType>(args[0]);
			register_combined_image_sampler(callee, image_id, sampler_id, combined_type.image.depth);
		}
	}

	// For function calls, we need to remap IDs which are function parameters into global variables.
	// This information is statically known from the current place in the call stack.
	// Function parameters are not necessarily pointers, so if we don't have a backing variable, remapping will know
	// which backing variable the image/sample came from.
	uint32_t image_id = remap_parameter(args[2]);
	uint32_t sampler_id = is_fetch ? compiler.dummy_sampler_id : remap_parameter(args[3]);

	auto itr = find_if(begin(compiler.combined_image_samplers), end(compiler.combined_image_samplers),
	                   [image_id, sampler_id](const CombinedImageSampler &combined) {
		                   return combined.image_id == image_id && combined.sampler_id == sampler_id;
	                   });

	if (itr == end(compiler.combined_image_samplers))
	{
		uint32_t sampled_type;
		if (is_fetch)
		{
			// Have to invent the sampled image type.
			sampled_type = compiler.ir.increase_bound_by(1);
			auto &type = compiler.set<SPIRType>(sampled_type);
			type = compiler.expression_type(args[2]);
			type.self = sampled_type;
			type.basetype = SPIRType::SampledImage;
			type.image.depth = false;
		}
		else
		{
			sampled_type = args[0];
		}

		auto id = compiler.ir.increase_bound_by(2);
		auto type_id = id + 0;
		auto combined_id = id + 1;

		// Make a new type, pointer to OpTypeSampledImage, so we can make a variable of this type.
		// We will probably have this type lying around, but it doesn't hurt to make duplicates for internal purposes.
		auto &type = compiler.set<SPIRType>(type_id);
		auto &base = compiler.get<SPIRType>(sampled_type);
		type = base;
		type.pointer = true;
		type.storage = StorageClassUniformConstant;
		type.parent_type = type_id;

		// Build new variable.
		compiler.set<SPIRVariable>(combined_id, type_id, StorageClassUniformConstant, 0);

		// Inherit RelaxedPrecision (and potentially other useful flags if deemed relevant).
		auto &new_flags = compiler.ir.meta[combined_id].decoration.decoration_flags;
		// Fetch inherits precision from the image, not sampler (there is no sampler).
		auto &old_flags = compiler.ir.meta[is_fetch ? image_id : sampler_id].decoration.decoration_flags;
		new_flags.reset();
		if (old_flags.get(DecorationRelaxedPrecision))
			new_flags.set(DecorationRelaxedPrecision);

		// Propagate the array type for the original image as well.
		auto *var = compiler.maybe_get_backing_variable(image_id);
		if (var)
		{
			auto &parent_type = compiler.get<SPIRType>(var->basetype);
			type.array = parent_type.array;
			type.array_size_literal = parent_type.array_size_literal;
		}

		compiler.combined_image_samplers.push_back({ combined_id, image_id, sampler_id });
	}

	return true;
}

uint32_t Compiler::build_dummy_sampler_for_combined_images()
{
	DummySamplerForCombinedImageHandler handler(*this);
	traverse_all_reachable_opcodes(get<SPIRFunction>(ir.default_entry_point), handler);
	if (handler.need_dummy_sampler)
	{
		uint32_t offset = ir.increase_bound_by(3);
		auto type_id = offset + 0;
		auto ptr_type_id = offset + 1;
		auto var_id = offset + 2;

		SPIRType sampler_type;
		auto &sampler = set<SPIRType>(type_id);
		sampler.basetype = SPIRType::Sampler;

		auto &ptr_sampler = set<SPIRType>(ptr_type_id);
		ptr_sampler = sampler;
		ptr_sampler.self = type_id;
		ptr_sampler.storage = StorageClassUniformConstant;
		ptr_sampler.pointer = true;
		ptr_sampler.parent_type = type_id;

		set<SPIRVariable>(var_id, ptr_type_id, StorageClassUniformConstant, 0);
		set_name(var_id, "SPIRV_Cross_DummySampler");
		dummy_sampler_id = var_id;
		return var_id;
	}
	else
		return 0;
}

void Compiler::build_combined_image_samplers()
{
	ir.for_each_typed_id<SPIRFunction>([&](uint32_t, SPIRFunction &func) {
		func.combined_parameters.clear();
		func.shadow_arguments.clear();
		func.do_combined_parameters = true;
	});

	combined_image_samplers.clear();
	CombinedImageSamplerHandler handler(*this);
	traverse_all_reachable_opcodes(get<SPIRFunction>(ir.default_entry_point), handler);
}

SmallVector<SpecializationConstant> Compiler::get_specialization_constants() const
{
	SmallVector<SpecializationConstant> spec_consts;
	ir.for_each_typed_id<SPIRConstant>([&](uint32_t, const SPIRConstant &c) {
		if (c.specialization && has_decoration(c.self, DecorationSpecId))
			spec_consts.push_back({ c.self, get_decoration(c.self, DecorationSpecId) });
	});
	return spec_consts;
}

SPIRConstant &Compiler::get_constant(uint32_t id)
{
	return get<SPIRConstant>(id);
}

const SPIRConstant &Compiler::get_constant(uint32_t id) const
{
	return get<SPIRConstant>(id);
}

static bool exists_unaccessed_path_to_return(const CFG &cfg, uint32_t block, const unordered_set<uint32_t> &blocks)
{
	// This block accesses the variable.
	if (blocks.find(block) != end(blocks))
		return false;

	// We are at the end of the CFG.
	if (cfg.get_succeeding_edges(block).empty())
		return true;

	// If any of our successors have a path to the end, there exists a path from block.
	for (auto &succ : cfg.get_succeeding_edges(block))
		if (exists_unaccessed_path_to_return(cfg, succ, blocks))
			return true;

	return false;
}

void Compiler::analyze_parameter_preservation(
    SPIRFunction &entry, const CFG &cfg, const unordered_map<uint32_t, unordered_set<uint32_t>> &variable_to_blocks,
    const unordered_map<uint32_t, unordered_set<uint32_t>> &complete_write_blocks)
{
	for (auto &arg : entry.arguments)
	{
		// Non-pointers are always inputs.
		auto &type = get<SPIRType>(arg.type);
		if (!type.pointer)
			continue;

		// Opaque argument types are always in
		bool potential_preserve;
		switch (type.basetype)
		{
		case SPIRType::Sampler:
		case SPIRType::Image:
		case SPIRType::SampledImage:
		case SPIRType::AtomicCounter:
			potential_preserve = false;
			break;

		default:
			potential_preserve = true;
			break;
		}

		if (!potential_preserve)
			continue;

		auto itr = variable_to_blocks.find(arg.id);
		if (itr == end(variable_to_blocks))
		{
			// Variable is never accessed.
			continue;
		}

		// We have accessed a variable, but there was no complete writes to that variable.
		// We deduce that we must preserve the argument.
		itr = complete_write_blocks.find(arg.id);
		if (itr == end(complete_write_blocks))
		{
			arg.read_count++;
			continue;
		}

		// If there is a path through the CFG where no block completely writes to the variable, the variable will be in an undefined state
		// when the function returns. We therefore need to implicitly preserve the variable in case there are writers in the function.
		// Major case here is if a function is
		// void foo(int &var) { if (cond) var = 10; }
		// Using read/write counts, we will think it's just an out variable, but it really needs to be inout,
		// because if we don't write anything whatever we put into the function must return back to the caller.
		if (exists_unaccessed_path_to_return(cfg, entry.entry_block, itr->second))
			arg.read_count++;
	}
}

Compiler::AnalyzeVariableScopeAccessHandler::AnalyzeVariableScopeAccessHandler(Compiler &compiler_,
                                                                               SPIRFunction &entry_)
    : compiler(compiler_)
    , entry(entry_)
{
}

bool Compiler::AnalyzeVariableScopeAccessHandler::follow_function_call(const SPIRFunction &)
{
	// Only analyze within this function.
	return false;
}

void Compiler::AnalyzeVariableScopeAccessHandler::set_current_block(const SPIRBlock &block)
{
	current_block = &block;

	// If we're branching to a block which uses OpPhi, in GLSL
	// this will be a variable write when we branch,
	// so we need to track access to these variables as well to
	// have a complete picture.
	const auto test_phi = [this, &block](uint32_t to) {
		auto &next = compiler.get<SPIRBlock>(to);
		for (auto &phi : next.phi_variables)
		{
			if (phi.parent == block.self)
			{
				accessed_variables_to_block[phi.function_variable].insert(block.self);
				// Phi variables are also accessed in our target branch block.
				accessed_variables_to_block[phi.function_variable].insert(next.self);

				notify_variable_access(phi.local_variable, block.self);
			}
		}
	};

	switch (block.terminator)
	{
	case SPIRBlock::Direct:
		notify_variable_access(block.condition, block.self);
		test_phi(block.next_block);
		break;

	case SPIRBlock::Select:
		notify_variable_access(block.condition, block.self);
		test_phi(block.true_block);
		test_phi(block.false_block);
		break;

	case SPIRBlock::MultiSelect:
		notify_variable_access(block.condition, block.self);
		for (auto &target : block.cases)
			test_phi(target.block);
		if (block.default_block)
			test_phi(block.default_block);
		break;

	default:
		break;
	}
}

void Compiler::AnalyzeVariableScopeAccessHandler::notify_variable_access(uint32_t id, uint32_t block)
{
	if (id_is_phi_variable(id))
		accessed_variables_to_block[id].insert(block);
	else if (id_is_potential_temporary(id))
		accessed_temporaries_to_block[id].insert(block);
}

bool Compiler::AnalyzeVariableScopeAccessHandler::id_is_phi_variable(uint32_t id) const
{
	if (id >= compiler.get_current_id_bound())
		return false;
	auto *var = compiler.maybe_get<SPIRVariable>(id);
	return var && var->phi_variable;
}

bool Compiler::AnalyzeVariableScopeAccessHandler::id_is_potential_temporary(uint32_t id) const
{
	if (id >= compiler.get_current_id_bound())
		return false;

	// Temporaries are not created before we start emitting code.
	return compiler.ir.ids[id].empty() || (compiler.ir.ids[id].get_type() == TypeExpression);
}

bool Compiler::AnalyzeVariableScopeAccessHandler::handle(spv::Op op, const uint32_t *args, uint32_t length)
{
	// Keep track of the types of temporaries, so we can hoist them out as necessary.
	uint32_t result_type, result_id;
	if (compiler.instruction_to_result_type(result_type, result_id, op, args, length))
		result_id_to_type[result_id] = result_type;

	switch (op)
	{
	case OpStore:
	{
		if (length < 2)
			return false;

		uint32_t ptr = args[0];
		auto *var = compiler.maybe_get_backing_variable(ptr);

		// If we store through an access chain, we have a partial write.
		if (var)
		{
			accessed_variables_to_block[var->self].insert(current_block->self);
			if (var->self == ptr)
				complete_write_variables_to_block[var->self].insert(current_block->self);
			else
				partial_write_variables_to_block[var->self].insert(current_block->self);
		}

		// Might try to store a Phi variable here.
		notify_variable_access(args[1], current_block->self);
		break;
	}

	case OpAccessChain:
	case OpInBoundsAccessChain:
	case OpPtrAccessChain:
	{
		if (length < 3)
			return false;

		uint32_t ptr = args[2];
		auto *var = compiler.maybe_get<SPIRVariable>(ptr);
		if (var)
			accessed_variables_to_block[var->self].insert(current_block->self);

		for (uint32_t i = 3; i < length; i++)
			notify_variable_access(args[i], current_block->self);

		// The result of an access chain is a fixed expression and is not really considered a temporary.
		auto &e = compiler.set<SPIRExpression>(args[1], "", args[0], true);
		auto *backing_variable = compiler.maybe_get_backing_variable(ptr);
		e.loaded_from = backing_variable ? backing_variable->self : 0;

		// Other backends might use SPIRAccessChain for this later.
		compiler.ir.ids[args[1]].set_allow_type_rewrite();
		break;
	}

	case OpCopyMemory:
	{
		if (length < 2)
			return false;

		uint32_t lhs = args[0];
		uint32_t rhs = args[1];
		auto *var = compiler.maybe_get_backing_variable(lhs);

		// If we store through an access chain, we have a partial write.
		if (var)
		{
			accessed_variables_to_block[var->self].insert(current_block->self);
			if (var->self == lhs)
				complete_write_variables_to_block[var->self].insert(current_block->self);
			else
				partial_write_variables_to_block[var->self].insert(current_block->self);
		}

		var = compiler.maybe_get_backing_variable(rhs);
		if (var)
			accessed_variables_to_block[var->self].insert(current_block->self);
		break;
	}

	case OpCopyObject:
	{
		if (length < 3)
			return false;

		auto *var = compiler.maybe_get_backing_variable(args[2]);
		if (var)
			accessed_variables_to_block[var->self].insert(current_block->self);

		// Might try to copy a Phi variable here.
		notify_variable_access(args[2], current_block->self);
		break;
	}

	case OpLoad:
	{
		if (length < 3)
			return false;
		uint32_t ptr = args[2];
		auto *var = compiler.maybe_get_backing_variable(ptr);
		if (var)
			accessed_variables_to_block[var->self].insert(current_block->self);

		// Loaded value is a temporary.
		notify_variable_access(args[1], current_block->self);
		break;
	}

	case OpFunctionCall:
	{
		if (length < 3)
			return false;

		length -= 3;
		args += 3;

		for (uint32_t i = 0; i < length; i++)
		{
			auto *var = compiler.maybe_get_backing_variable(args[i]);
			if (var)
			{
				accessed_variables_to_block[var->self].insert(current_block->self);
				// Assume we can get partial writes to this variable.
				partial_write_variables_to_block[var->self].insert(current_block->self);
			}

			// Cannot easily prove if argument we pass to a function is completely written.
			// Usually, functions write to a dummy variable,
			// which is then copied to in full to the real argument.

			// Might try to copy a Phi variable here.
			notify_variable_access(args[i], current_block->self);
		}

		// Return value may be a temporary.
		notify_variable_access(args[1], current_block->self);
		break;
	}

	case OpExtInst:
	{
		for (uint32_t i = 4; i < length; i++)
			notify_variable_access(args[i], current_block->self);
		notify_variable_access(args[1], current_block->self);
		break;
	}

	case OpArrayLength:
		// Uses literals, but cannot be a phi variable or temporary, so ignore.
		break;

		// Atomics shouldn't be able to access function-local variables.
		// Some GLSL builtins access a pointer.

	case OpCompositeInsert:
	case OpVectorShuffle:
		// Specialize for opcode which contains literals.
		for (uint32_t i = 1; i < 4; i++)
			notify_variable_access(args[i], current_block->self);
		break;

	case OpCompositeExtract:
		// Specialize for opcode which contains literals.
		for (uint32_t i = 1; i < 3; i++)
			notify_variable_access(args[i], current_block->self);
		break;

	case OpImageWrite:
		for (uint32_t i = 0; i < length; i++)
		{
			// Argument 3 is a literal.
			if (i != 3)
				notify_variable_access(args[i], current_block->self);
		}
		break;

	case OpImageSampleImplicitLod:
	case OpImageSampleExplicitLod:
	case OpImageSparseSampleImplicitLod:
	case OpImageSparseSampleExplicitLod:
	case OpImageSampleProjImplicitLod:
	case OpImageSampleProjExplicitLod:
	case OpImageSparseSampleProjImplicitLod:
	case OpImageSparseSampleProjExplicitLod:
	case OpImageFetch:
	case OpImageSparseFetch:
	case OpImageRead:
	case OpImageSparseRead:
		for (uint32_t i = 1; i < length; i++)
		{
			// Argument 4 is a literal.
			if (i != 4)
				notify_variable_access(args[i], current_block->self);
		}
		break;

	case OpImageSampleDrefImplicitLod:
	case OpImageSampleDrefExplicitLod:
	case OpImageSparseSampleDrefImplicitLod:
	case OpImageSparseSampleDrefExplicitLod:
	case OpImageSampleProjDrefImplicitLod:
	case OpImageSampleProjDrefExplicitLod:
	case OpImageSparseSampleProjDrefImplicitLod:
	case OpImageSparseSampleProjDrefExplicitLod:
	case OpImageGather:
	case OpImageSparseGather:
	case OpImageDrefGather:
	case OpImageSparseDrefGather:
		for (uint32_t i = 1; i < length; i++)
		{
			// Argument 5 is a literal.
			if (i != 5)
				notify_variable_access(args[i], current_block->self);
		}
		break;

	default:
	{
		// Rather dirty way of figuring out where Phi variables are used.
		// As long as only IDs are used, we can scan through instructions and try to find any evidence that
		// the ID of a variable has been used.
		// There are potential false positives here where a literal is used in-place of an ID,
		// but worst case, it does not affect the correctness of the compile.
		// Exhaustive analysis would be better here, but it's not worth it for now.
		for (uint32_t i = 0; i < length; i++)
			notify_variable_access(args[i], current_block->self);
		break;
	}
	}
	return true;
}

Compiler::StaticExpressionAccessHandler::StaticExpressionAccessHandler(Compiler &compiler_, uint32_t variable_id_)
    : compiler(compiler_)
    , variable_id(variable_id_)
{
}

bool Compiler::StaticExpressionAccessHandler::follow_function_call(const SPIRFunction &)
{
	return false;
}

bool Compiler::StaticExpressionAccessHandler::handle(spv::Op op, const uint32_t *args, uint32_t length)
{
	switch (op)
	{
	case OpStore:
		if (length < 2)
			return false;
		if (args[0] == variable_id)
		{
			static_expression = args[1];
			write_count++;
		}
		break;

	case OpLoad:
		if (length < 3)
			return false;
		if (args[2] == variable_id && static_expression == 0) // Tried to read from variable before it was initialized.
			return false;
		break;

	case OpAccessChain:
	case OpInBoundsAccessChain:
	case OpPtrAccessChain:
		if (length < 3)
			return false;
		if (args[2] == variable_id) // If we try to access chain our candidate variable before we store to it, bail.
			return false;
		break;

	default:
		break;
	}

	return true;
}

void Compiler::find_function_local_luts(SPIRFunction &entry, const AnalyzeVariableScopeAccessHandler &handler,
                                        bool single_function)
{
	auto &cfg = *function_cfgs.find(entry.self)->second;

	// For each variable which is statically accessed.
	for (auto &accessed_var : handler.accessed_variables_to_block)
	{
		auto &blocks = accessed_var.second;
		auto &var = get<SPIRVariable>(accessed_var.first);
		auto &type = expression_type(accessed_var.first);

		// Only consider function local variables here.
		// If we only have a single function in our CFG, private storage is also fine,
		// since it behaves like a function local variable.
		bool allow_lut = var.storage == StorageClassFunction || (single_function && var.storage == StorageClassPrivate);
		if (!allow_lut)
			continue;

		// We cannot be a phi variable.
		if (var.phi_variable)
			continue;

		// Only consider arrays here.
		if (type.array.empty())
			continue;

		// If the variable has an initializer, make sure it is a constant expression.
		uint32_t static_constant_expression = 0;
		if (var.initializer)
		{
			if (ir.ids[var.initializer].get_type() != TypeConstant)
				continue;
			static_constant_expression = var.initializer;

			// There can be no stores to this variable, we have now proved we have a LUT.
			if (handler.complete_write_variables_to_block.count(var.self) != 0 ||
			    handler.partial_write_variables_to_block.count(var.self) != 0)
				continue;
		}
		else
		{
			// We can have one, and only one write to the variable, and that write needs to be a constant.

			// No partial writes allowed.
			if (handler.partial_write_variables_to_block.count(var.self) != 0)
				continue;

			auto itr = handler.complete_write_variables_to_block.find(var.self);

			// No writes?
			if (itr == end(handler.complete_write_variables_to_block))
				continue;

			// We write to the variable in more than one block.
			auto &write_blocks = itr->second;
			if (write_blocks.size() != 1)
				continue;

			// The write needs to happen in the dominating block.
			DominatorBuilder builder(cfg);
			for (auto &block : blocks)
				builder.add_block(block);
			uint32_t dominator = builder.get_dominator();

			// The complete write happened in a branch or similar, cannot deduce static expression.
			if (write_blocks.count(dominator) == 0)
				continue;

			// Find the static expression for this variable.
			StaticExpressionAccessHandler static_expression_handler(*this, var.self);
			traverse_all_reachable_opcodes(get<SPIRBlock>(dominator), static_expression_handler);

			// We want one, and exactly one write
			if (static_expression_handler.write_count != 1 || static_expression_handler.static_expression == 0)
				continue;

			// Is it a constant expression?
			if (ir.ids[static_expression_handler.static_expression].get_type() != TypeConstant)
				continue;

			// We found a LUT!
			static_constant_expression = static_expression_handler.static_expression;
		}

		get<SPIRConstant>(static_constant_expression).is_used_as_lut = true;
		var.static_expression = static_constant_expression;
		var.statically_assigned = true;
		var.remapped_variable = true;
	}
}

void Compiler::analyze_variable_scope(SPIRFunction &entry, AnalyzeVariableScopeAccessHandler &handler)
{
	// First, we map out all variable access within a function.
	// Essentially a map of block -> { variables accessed in the basic block }
	traverse_all_reachable_opcodes(entry, handler);

	auto &cfg = *function_cfgs.find(entry.self)->second;

	// Analyze if there are parameters which need to be implicitly preserved with an "in" qualifier.
	analyze_parameter_preservation(entry, cfg, handler.accessed_variables_to_block,
	                               handler.complete_write_variables_to_block);

	unordered_map<uint32_t, uint32_t> potential_loop_variables;

	// For each variable which is statically accessed.
	for (auto &var : handler.accessed_variables_to_block)
	{
		// Only deal with variables which are considered local variables in this function.
		if (find(begin(entry.local_variables), end(entry.local_variables), var.first) == end(entry.local_variables))
			continue;

		DominatorBuilder builder(cfg);
		auto &blocks = var.second;
		auto &type = expression_type(var.first);

		// Figure out which block is dominating all accesses of those variables.
		for (auto &block : blocks)
		{
			// If we're accessing a variable inside a continue block, this variable might be a loop variable.
			// We can only use loop variables with scalars, as we cannot track static expressions for vectors.
			if (is_continue(block))
			{
				// Potentially awkward case to check for.
				// We might have a variable inside a loop, which is touched by the continue block,
				// but is not actually a loop variable.
				// The continue block is dominated by the inner part of the loop, which does not make sense in high-level
				// language output because it will be declared before the body,
				// so we will have to lift the dominator up to the relevant loop header instead.
				builder.add_block(ir.continue_block_to_loop_header[block]);

				// Arrays or structs cannot be loop variables.
				if (type.vecsize == 1 && type.columns == 1 && type.basetype != SPIRType::Struct && type.array.empty())
				{
					// The variable is used in multiple continue blocks, this is not a loop
					// candidate, signal that by setting block to -1u.
					auto &potential = potential_loop_variables[var.first];

					if (potential == 0)
						potential = block;
					else
						potential = ~(0u);
				}
			}
			builder.add_block(block);
		}

		builder.lift_continue_block_dominator();

		// Add it to a per-block list of variables.
		uint32_t dominating_block = builder.get_dominator();

		// If all blocks here are dead code, this will be 0, so the variable in question
		// will be completely eliminated.
		if (dominating_block)
		{
			auto &block = get<SPIRBlock>(dominating_block);
			block.dominated_variables.push_back(var.first);
			get<SPIRVariable>(var.first).dominator = dominating_block;
		}
	}

	for (auto &var : handler.accessed_temporaries_to_block)
	{
		auto itr = handler.result_id_to_type.find(var.first);

		if (itr == end(handler.result_id_to_type))
		{
			// We found a false positive ID being used, ignore.
			// This should probably be an assert.
			continue;
		}

		// There is no point in doing domination analysis for opaque types.
		auto &type = get<SPIRType>(itr->second);
		if (type_is_opaque_value(type))
			continue;

		DominatorBuilder builder(cfg);
		bool force_temporary = false;

		// Figure out which block is dominating all accesses of those temporaries.
		auto &blocks = var.second;
		for (auto &block : blocks)
		{
			builder.add_block(block);

			// If a temporary is used in more than one block, we might have to lift continue block
			// access up to loop header like we did for variables.
			if (blocks.size() != 1 && is_continue(block))
				builder.add_block(ir.continue_block_to_loop_header[block]);
			else if (blocks.size() != 1 && is_single_block_loop(block))
			{
				// Awkward case, because the loop header is also the continue block.
				force_temporary = true;
			}
		}

		uint32_t dominating_block = builder.get_dominator();
		if (dominating_block)
		{
			// If we touch a variable in the dominating block, this is the expected setup.
			// SPIR-V normally mandates this, but we have extra cases for temporary use inside loops.
			bool first_use_is_dominator = blocks.count(dominating_block) != 0;

			if (!first_use_is_dominator || force_temporary)
			{
				// This should be very rare, but if we try to declare a temporary inside a loop,
				// and that temporary is used outside the loop as well (spirv-opt inliner likes this)
				// we should actually emit the temporary outside the loop.
				hoisted_temporaries.insert(var.first);
				forced_temporaries.insert(var.first);

				auto &block_temporaries = get<SPIRBlock>(dominating_block).declare_temporary;
				block_temporaries.emplace_back(handler.result_id_to_type[var.first], var.first);
			}
			else if (blocks.size() > 1)
			{
				// Keep track of the temporary as we might have to declare this temporary.
				// This can happen if the loop header dominates a temporary, but we have a complex fallback loop.
				// In this case, the header is actually inside the for (;;) {} block, and we have problems.
				// What we need to do is hoist the temporaries outside the for (;;) {} block in case the header block
				// declares the temporary.
				auto &block_temporaries = get<SPIRBlock>(dominating_block).potential_declare_temporary;
				block_temporaries.emplace_back(handler.result_id_to_type[var.first], var.first);
			}
		}
	}

	unordered_set<uint32_t> seen_blocks;

	// Now, try to analyze whether or not these variables are actually loop variables.
	for (auto &loop_variable : potential_loop_variables)
	{
		auto &var = get<SPIRVariable>(loop_variable.first);
		auto dominator = var.dominator;
		auto block = loop_variable.second;

		// The variable was accessed in multiple continue blocks, ignore.
		if (block == ~(0u) || block == 0)
			continue;

		// Dead code.
		if (dominator == 0)
			continue;

		uint32_t header = 0;

		// Find the loop header for this block if we are a continue block.
		{
			auto itr = ir.continue_block_to_loop_header.find(block);
			if (itr != end(ir.continue_block_to_loop_header))
			{
				header = itr->second;
			}
			else if (get<SPIRBlock>(block).continue_block == block)
			{
				// Also check for self-referential continue block.
				header = block;
			}
		}

		assert(header);
		auto &header_block = get<SPIRBlock>(header);
		auto &blocks = handler.accessed_variables_to_block[loop_variable.first];

		// If a loop variable is not used before the loop, it's probably not a loop variable.
		bool has_accessed_variable = blocks.count(header) != 0;

		// Now, there are two conditions we need to meet for the variable to be a loop variable.
		// 1. The dominating block must have a branch-free path to the loop header,
		// this way we statically know which expression should be part of the loop variable initializer.

		// Walk from the dominator, if there is one straight edge connecting
		// dominator and loop header, we statically know the loop initializer.
		bool static_loop_init = true;
		while (dominator != header)
		{
			if (blocks.count(dominator) != 0)
				has_accessed_variable = true;

			auto &succ = cfg.get_succeeding_edges(dominator);
			if (succ.size() != 1)
			{
				static_loop_init = false;
				break;
			}

			auto &pred = cfg.get_preceding_edges(succ.front());
			if (pred.size() != 1 || pred.front() != dominator)
			{
				static_loop_init = false;
				break;
			}

			dominator = succ.front();
		}

		if (!static_loop_init || !has_accessed_variable)
			continue;

		// The second condition we need to meet is that no access after the loop
		// merge can occur. Walk the CFG to see if we find anything.

		seen_blocks.clear();
		cfg.walk_from(seen_blocks, header_block.merge_block, [&](uint32_t walk_block) {
			// We found a block which accesses the variable outside the loop.
			if (blocks.find(walk_block) != end(blocks))
				static_loop_init = false;
		});

		if (!static_loop_init)
			continue;

		// We have a loop variable.
		header_block.loop_variables.push_back(loop_variable.first);
		// Need to sort here as variables come from an unordered container, and pushing stuff in wrong order
		// will break reproducability in regression runs.
		sort(begin(header_block.loop_variables), end(header_block.loop_variables));
		get<SPIRVariable>(loop_variable.first).loop_variable = true;
	}
}

Bitset Compiler::get_buffer_block_flags(uint32_t id) const
{
	return ir.get_buffer_block_flags(get<SPIRVariable>(id));
}

bool Compiler::get_common_basic_type(const SPIRType &type, SPIRType::BaseType &base_type)
{
	if (type.basetype == SPIRType::Struct)
	{
		base_type = SPIRType::Unknown;
		for (auto &member_type : type.member_types)
		{
			SPIRType::BaseType member_base;
			if (!get_common_basic_type(get<SPIRType>(member_type), member_base))
				return false;

			if (base_type == SPIRType::Unknown)
				base_type = member_base;
			else if (base_type != member_base)
				return false;
		}
		return true;
	}
	else
	{
		base_type = type.basetype;
		return true;
	}
}

void Compiler::ActiveBuiltinHandler::handle_builtin(const SPIRType &type, BuiltIn builtin,
                                                    const Bitset &decoration_flags)
{
	// If used, we will need to explicitly declare a new array size for these builtins.

	if (builtin == BuiltInClipDistance)
	{
		if (!type.array_size_literal[0])
			SPIRV_CROSS_THROW("Array size for ClipDistance must be a literal.");
		uint32_t array_size = type.array[0];
		if (array_size == 0)
			SPIRV_CROSS_THROW("Array size for ClipDistance must not be unsized.");
		compiler.clip_distance_count = array_size;
	}
	else if (builtin == BuiltInCullDistance)
	{
		if (!type.array_size_literal[0])
			SPIRV_CROSS_THROW("Array size for CullDistance must be a literal.");
		uint32_t array_size = type.array[0];
		if (array_size == 0)
			SPIRV_CROSS_THROW("Array size for CullDistance must not be unsized.");
		compiler.cull_distance_count = array_size;
	}
	else if (builtin == BuiltInPosition)
	{
		if (decoration_flags.get(DecorationInvariant))
			compiler.position_invariant = true;
	}
}

bool Compiler::ActiveBuiltinHandler::handle(spv::Op opcode, const uint32_t *args, uint32_t length)
{
	const auto add_if_builtin = [&](uint32_t id) {
		// Only handles variables here.
		// Builtins which are part of a block are handled in AccessChain.
		auto *var = compiler.maybe_get<SPIRVariable>(id);
		auto &decorations = compiler.ir.meta[id].decoration;
		if (var && decorations.builtin)
		{
			auto &type = compiler.get<SPIRType>(var->basetype);
			auto &flags =
			    type.storage == StorageClassInput ? compiler.active_input_builtins : compiler.active_output_builtins;
			flags.set(decorations.builtin_type);
			handle_builtin(type, decorations.builtin_type, decorations.decoration_flags);
		}
	};

	switch (opcode)
	{
	case OpStore:
		if (length < 1)
			return false;

		add_if_builtin(args[0]);
		break;

	case OpCopyMemory:
		if (length < 2)
			return false;

		add_if_builtin(args[0]);
		add_if_builtin(args[1]);
		break;

	case OpCopyObject:
	case OpLoad:
		if (length < 3)
			return false;

		add_if_builtin(args[2]);
		break;

	case OpSelect:
		if (length < 5)
			return false;

		add_if_builtin(args[3]);
		add_if_builtin(args[4]);
		break;

	case OpPhi:
	{
		if (length < 2)
			return false;

		uint32_t count = length - 2;
		args += 2;
		for (uint32_t i = 0; i < count; i += 2)
			add_if_builtin(args[i]);
		break;
	}

	case OpFunctionCall:
	{
		if (length < 3)
			return false;

		uint32_t count = length - 3;
		args += 3;
		for (uint32_t i = 0; i < count; i++)
			add_if_builtin(args[i]);
		break;
	}

	case OpAccessChain:
	case OpInBoundsAccessChain:
	case OpPtrAccessChain:
	{
		if (length < 4)
			return false;

		// Only consider global variables, cannot consider variables in functions yet, or other
		// access chains as they have not been created yet.
		auto *var = compiler.maybe_get<SPIRVariable>(args[2]);
		if (!var)
			break;

		// Required if we access chain into builtins like gl_GlobalInvocationID.
		add_if_builtin(args[2]);

		// Start traversing type hierarchy at the proper non-pointer types.
		auto *type = &compiler.get_variable_data_type(*var);

		auto &flags =
		    var->storage == StorageClassInput ? compiler.active_input_builtins : compiler.active_output_builtins;

		uint32_t count = length - 3;
		args += 3;
		for (uint32_t i = 0; i < count; i++)
		{
			// Pointers
			if (opcode == OpPtrAccessChain && i == 0)
			{
				type = &compiler.get<SPIRType>(type->parent_type);
				continue;
			}

			// Arrays
			if (!type->array.empty())
			{
				type = &compiler.get<SPIRType>(type->parent_type);
			}
			// Structs
			else if (type->basetype == SPIRType::Struct)
			{
				uint32_t index = compiler.get<SPIRConstant>(args[i]).scalar();

				if (index < uint32_t(compiler.ir.meta[type->self].members.size()))
				{
					auto &decorations = compiler.ir.meta[type->self].members[index];
					if (decorations.builtin)
					{
						flags.set(decorations.builtin_type);
						handle_builtin(compiler.get<SPIRType>(type->member_types[index]), decorations.builtin_type,
						               decorations.decoration_flags);
					}
				}

				type = &compiler.get<SPIRType>(type->member_types[index]);
			}
			else
			{
				// No point in traversing further. We won't find any extra builtins.
				break;
			}
		}
		break;
	}

	default:
		break;
	}

	return true;
}

void Compiler::update_active_builtins()
{
	active_input_builtins.reset();
	active_output_builtins.reset();
	cull_distance_count = 0;
	clip_distance_count = 0;
	ActiveBuiltinHandler handler(*this);
	traverse_all_reachable_opcodes(get<SPIRFunction>(ir.default_entry_point), handler);
}

// Returns whether this shader uses a builtin of the storage class
bool Compiler::has_active_builtin(BuiltIn builtin, StorageClass storage)
{
	const Bitset *flags;
	switch (storage)
	{
	case StorageClassInput:
		flags = &active_input_builtins;
		break;
	case StorageClassOutput:
		flags = &active_output_builtins;
		break;

	default:
		return false;
	}
	return flags->get(builtin);
}

void Compiler::analyze_image_and_sampler_usage()
{
	CombinedImageSamplerDrefHandler dref_handler(*this);
	traverse_all_reachable_opcodes(get<SPIRFunction>(ir.default_entry_point), dref_handler);

	CombinedImageSamplerUsageHandler handler(*this, dref_handler.dref_combined_samplers);
	traverse_all_reachable_opcodes(get<SPIRFunction>(ir.default_entry_point), handler);
	comparison_ids = move(handler.comparison_ids);
	need_subpass_input = handler.need_subpass_input;

	// Forward information from separate images and samplers into combined image samplers.
	for (auto &combined : combined_image_samplers)
		if (comparison_ids.count(combined.sampler_id))
			comparison_ids.insert(combined.combined_id);
}

bool Compiler::CombinedImageSamplerDrefHandler::handle(spv::Op opcode, const uint32_t *args, uint32_t)
{
	// Mark all sampled images which are used with Dref.
	switch (opcode)
	{
	case OpImageSampleDrefExplicitLod:
	case OpImageSampleDrefImplicitLod:
	case OpImageSampleProjDrefExplicitLod:
	case OpImageSampleProjDrefImplicitLod:
	case OpImageSparseSampleProjDrefImplicitLod:
	case OpImageSparseSampleDrefImplicitLod:
	case OpImageSparseSampleProjDrefExplicitLod:
	case OpImageSparseSampleDrefExplicitLod:
	case OpImageDrefGather:
	case OpImageSparseDrefGather:
		dref_combined_samplers.insert(args[2]);
		return true;

	default:
		break;
	}

	return true;
}

void Compiler::build_function_control_flow_graphs_and_analyze()
{
	CFGBuilder handler(*this);
	handler.function_cfgs[ir.default_entry_point].reset(new CFG(*this, get<SPIRFunction>(ir.default_entry_point)));
	traverse_all_reachable_opcodes(get<SPIRFunction>(ir.default_entry_point), handler);
	function_cfgs = move(handler.function_cfgs);
	bool single_function = function_cfgs.size() <= 1;

	for (auto &f : function_cfgs)
	{
		auto &func = get<SPIRFunction>(f.first);
		AnalyzeVariableScopeAccessHandler scope_handler(*this, func);
		analyze_variable_scope(func, scope_handler);
		find_function_local_luts(func, scope_handler, single_function);

		// Check if we can actually use the loop variables we found in analyze_variable_scope.
		// To use multiple initializers, we need the same type and qualifiers.
		for (auto block : func.blocks)
		{
			auto &b = get<SPIRBlock>(block);
			if (b.loop_variables.size() < 2)
				continue;

			auto &flags = get_decoration_bitset(b.loop_variables.front());
			uint32_t type = get<SPIRVariable>(b.loop_variables.front()).basetype;
			bool invalid_initializers = false;
			for (auto loop_variable : b.loop_variables)
			{
				if (flags != get_decoration_bitset(loop_variable) ||
				    type != get<SPIRVariable>(b.loop_variables.front()).basetype)
				{
					invalid_initializers = true;
					break;
				}
			}

			if (invalid_initializers)
			{
				for (auto loop_variable : b.loop_variables)
					get<SPIRVariable>(loop_variable).loop_variable = false;
				b.loop_variables.clear();
			}
		}
	}
}

Compiler::CFGBuilder::CFGBuilder(Compiler &compiler_)
    : compiler(compiler_)
{
}

bool Compiler::CFGBuilder::handle(spv::Op, const uint32_t *, uint32_t)
{
	return true;
}

bool Compiler::CFGBuilder::follow_function_call(const SPIRFunction &func)
{
	if (function_cfgs.find(func.self) == end(function_cfgs))
	{
		function_cfgs[func.self].reset(new CFG(compiler, func));
		return true;
	}
	else
		return false;
}

bool Compiler::CombinedImageSamplerUsageHandler::begin_function_scope(const uint32_t *args, uint32_t length)
{
	if (length < 3)
		return false;

	auto &func = compiler.get<SPIRFunction>(args[2]);
	const auto *arg = &args[3];
	length -= 3;

	for (uint32_t i = 0; i < length; i++)
	{
		auto &argument = func.arguments[i];
		dependency_hierarchy[argument.id].insert(arg[i]);
	}

	return true;
}

void Compiler::CombinedImageSamplerUsageHandler::add_hierarchy_to_comparison_ids(uint32_t id)
{
	// Traverse the variable dependency hierarchy and tag everything in its path with comparison ids.
	comparison_ids.insert(id);
	for (auto &dep_id : dependency_hierarchy[id])
		add_hierarchy_to_comparison_ids(dep_id);
}

bool Compiler::CombinedImageSamplerUsageHandler::handle(Op opcode, const uint32_t *args, uint32_t length)
{
	switch (opcode)
	{
	case OpAccessChain:
	case OpInBoundsAccessChain:
	case OpPtrAccessChain:
	case OpLoad:
	{
		if (length < 3)
			return false;
		dependency_hierarchy[args[1]].insert(args[2]);

		// Ideally defer this to OpImageRead, but then we'd need to track loaded IDs.
		// If we load an image, we're going to use it and there is little harm in declaring an unused gl_FragCoord.
		auto &type = compiler.get<SPIRType>(args[0]);
		if (type.image.dim == DimSubpassData)
			need_subpass_input = true;

		// If we load a SampledImage and it will be used with Dref, propagate the state up.
		if (dref_combined_samplers.count(args[1]) != 0)
			add_hierarchy_to_comparison_ids(args[1]);
		break;
	}

	case OpSampledImage:
	{
		if (length < 4)
			return false;

		uint32_t result_type = args[0];
		uint32_t result_id = args[1];
		auto &type = compiler.get<SPIRType>(result_type);
		if (type.image.depth || dref_combined_samplers.count(result_id) != 0)
		{
			// This image must be a depth image.
			uint32_t image = args[2];
			add_hierarchy_to_comparison_ids(image);

			// This sampler must be a SamplerComparisonState, and not a regular SamplerState.
			uint32_t sampler = args[3];
			add_hierarchy_to_comparison_ids(sampler);

			// Mark the OpSampledImage itself as being comparison state.
			comparison_ids.insert(result_id);
		}
		return true;
	}

	default:
		break;
	}

	return true;
}

bool Compiler::buffer_is_hlsl_counter_buffer(uint32_t id) const
{
	auto *m = ir.find_meta(id);
	return m && m->hlsl_is_magic_counter_buffer;
}

bool Compiler::buffer_get_hlsl_counter_buffer(uint32_t id, uint32_t &counter_id) const
{
	auto *m = ir.find_meta(id);

	// First, check for the proper decoration.
	if (m && m->hlsl_magic_counter_buffer != 0)
	{
		counter_id = m->hlsl_magic_counter_buffer;
		return true;
	}
	else
		return false;
}

void Compiler::make_constant_null(uint32_t id, uint32_t type)
{
	auto &constant_type = get<SPIRType>(type);

	if (constant_type.pointer)
	{
		auto &constant = set<SPIRConstant>(id, type);
		constant.make_null(constant_type);
	}
	else if (!constant_type.array.empty())
	{
		assert(constant_type.parent_type);
		uint32_t parent_id = ir.increase_bound_by(1);
		make_constant_null(parent_id, constant_type.parent_type);

		if (!constant_type.array_size_literal.back())
			SPIRV_CROSS_THROW("Array size of OpConstantNull must be a literal.");

		SmallVector<uint32_t> elements(constant_type.array.back());
		for (uint32_t i = 0; i < constant_type.array.back(); i++)
			elements[i] = parent_id;
		set<SPIRConstant>(id, type, elements.data(), uint32_t(elements.size()), false);
	}
	else if (!constant_type.member_types.empty())
	{
		uint32_t member_ids = ir.increase_bound_by(uint32_t(constant_type.member_types.size()));
		SmallVector<uint32_t> elements(constant_type.member_types.size());
		for (uint32_t i = 0; i < constant_type.member_types.size(); i++)
		{
			make_constant_null(member_ids + i, constant_type.member_types[i]);
			elements[i] = member_ids + i;
		}
		set<SPIRConstant>(id, type, elements.data(), uint32_t(elements.size()), false);
	}
	else
	{
		auto &constant = set<SPIRConstant>(id, type);
		constant.make_null(constant_type);
	}
}

const SmallVector<spv::Capability> &Compiler::get_declared_capabilities() const
{
	return ir.declared_capabilities;
}

const SmallVector<std::string> &Compiler::get_declared_extensions() const
{
	return ir.declared_extensions;
}

std::string Compiler::get_remapped_declared_block_name(uint32_t id) const
{
	auto itr = declared_block_names.find(id);
	if (itr != end(declared_block_names))
		return itr->second;
	else
	{
		auto &var = get<SPIRVariable>(id);
		auto &type = get<SPIRType>(var.basetype);

		auto *type_meta = ir.find_meta(type.self);
		auto *block_name = type_meta ? &type_meta->decoration.alias : nullptr;
		return (!block_name || block_name->empty()) ? get_block_fallback_name(id) : *block_name;
	}
}

bool Compiler::instruction_to_result_type(uint32_t &result_type, uint32_t &result_id, spv::Op op, const uint32_t *args,
                                          uint32_t length)
{
	// Most instructions follow the pattern of <result-type> <result-id> <arguments>.
	// There are some exceptions.
	switch (op)
	{
	case OpStore:
	case OpCopyMemory:
	case OpCopyMemorySized:
	case OpImageWrite:
	case OpAtomicStore:
	case OpAtomicFlagClear:
	case OpEmitStreamVertex:
	case OpEndStreamPrimitive:
	case OpControlBarrier:
	case OpMemoryBarrier:
	case OpGroupWaitEvents:
	case OpRetainEvent:
	case OpReleaseEvent:
	case OpSetUserEventStatus:
	case OpCaptureEventProfilingInfo:
	case OpCommitReadPipe:
	case OpCommitWritePipe:
	case OpGroupCommitReadPipe:
	case OpGroupCommitWritePipe:
		return false;

	default:
		if (length > 1 && maybe_get<SPIRType>(args[0]) != nullptr)
		{
			result_type = args[0];
			result_id = args[1];
			return true;
		}
		else
			return false;
	}
}

Bitset Compiler::combined_decoration_for_member(const SPIRType &type, uint32_t index) const
{
	Bitset flags;
	auto *type_meta = ir.find_meta(type.self);

	if (type_meta)
	{
		auto &memb = type_meta->members;
		if (index >= memb.size())
			return flags;
		auto &dec = memb[index];

		// If our type is a struct, traverse all the members as well recursively.
		flags.merge_or(dec.decoration_flags);
		for (uint32_t i = 0; i < type.member_types.size(); i++)
			flags.merge_or(combined_decoration_for_member(get<SPIRType>(type.member_types[i]), i));
	}

	return flags;
}

bool Compiler::is_desktop_only_format(spv::ImageFormat format)
{
	switch (format)
	{
	// Desktop-only formats
	case ImageFormatR11fG11fB10f:
	case ImageFormatR16f:
	case ImageFormatRgb10A2:
	case ImageFormatR8:
	case ImageFormatRg8:
	case ImageFormatR16:
	case ImageFormatRg16:
	case ImageFormatRgba16:
	case ImageFormatR16Snorm:
	case ImageFormatRg16Snorm:
	case ImageFormatRgba16Snorm:
	case ImageFormatR8Snorm:
	case ImageFormatRg8Snorm:
	case ImageFormatR8ui:
	case ImageFormatRg8ui:
	case ImageFormatR16ui:
	case ImageFormatRgb10a2ui:
	case ImageFormatR8i:
	case ImageFormatRg8i:
	case ImageFormatR16i:
		return true;
	default:
		break;
	}

	return false;
}

bool Compiler::image_is_comparison(const SPIRType &type, uint32_t id) const
{
	return type.image.depth || (comparison_ids.count(id) != 0);
}

bool Compiler::type_is_opaque_value(const SPIRType &type) const
{
	return !type.pointer && (type.basetype == SPIRType::SampledImage || type.basetype == SPIRType::Image ||
	                         type.basetype == SPIRType::Sampler);
}

// Make these member functions so we can easily break on any force_recompile events.
void Compiler::force_recompile()
{
	is_force_recompile = true;
}

bool Compiler::is_forcing_recompilation() const
{
	return is_force_recompile;
}

void Compiler::clear_force_recompile()
{
	is_force_recompile = false;
}
