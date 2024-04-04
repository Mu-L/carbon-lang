// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "toolchain/check/merge.h"

#include "toolchain/base/kind_switch.h"
#include "toolchain/check/function.h"
#include "toolchain/check/import_ref.h"
#include "toolchain/sem_ir/ids.h"
#include "toolchain/sem_ir/typed_insts.h"

namespace Carbon::Check {

auto ResolvePrevInstForMerge(Context& context, Parse::NodeId node_id,
                             SemIR::InstId prev_inst_id) -> InstForMerge {
  auto prev_inst = context.insts().Get(prev_inst_id);
  auto import_ref = prev_inst.TryAs<SemIR::AnyImportRef>();
  // If not imported, use the instruction directly.
  if (!import_ref) {
    return {.inst = prev_inst,
            .import_ir_inst_id = SemIR::ImportIRInstId::Invalid};
  }

  // If the import ref was previously used, print a diagnostic.
  if (auto import_ref_used = prev_inst.TryAs<SemIR::ImportRefUsed>()) {
    CARBON_DIAGNOSTIC(
        RedeclOfUsedImport, Error,
        "Redeclaration of imported entity that was previously used.");
    CARBON_DIAGNOSTIC(UsedImportLoc, Note, "Import used here.");
    context.emitter()
        .Build(node_id, RedeclOfUsedImport)
        .Note(import_ref_used->used_id, UsedImportLoc)
        .Emit();
  }

  // Follow the import ref.
  return {.inst = context.insts().Get(
              context.constant_values().Get(prev_inst_id).inst_id()),
          .import_ir_inst_id = import_ref->import_ir_inst_id};
}

// Returns the instruction to consider when merging the given inst_id. Returns
// nullopt if merging is infeasible and no diagnostic should be printed.
static auto ResolveMergeableInst(Context& context, SemIR::InstId inst_id)
    -> std::optional<InstForMerge> {
  auto inst = context.insts().Get(inst_id);
  switch (inst.kind()) {
    case SemIR::ImportRefUnloaded::Kind:
      // Load before merging.
      LoadImportRef(context, inst_id, SemIR::LocId::Invalid);
      break;

    case SemIR::ImportRefUsed::Kind:
      // Already loaded.
      break;

    case SemIR::Namespace::Kind:
      // Return back the namespace directly.
      return {
          {.inst = inst, .import_ir_inst_id = SemIR::ImportIRInstId::Invalid}};

    default:
      CARBON_FATAL() << "Unexpected inst kind passed to ResolveMergeableInst: "
                     << inst;
  }

  auto const_id = context.constant_values().Get(inst_id);
  // TODO: Function and type declarations are constant, but `var` declarations
  // are non-constant and should still merge.
  if (!const_id.is_constant()) {
    return std::nullopt;
  }
  return {
      {.inst = context.insts().Get(const_id.inst_id()),
       .import_ir_inst_id = inst.As<SemIR::AnyImportRef>().import_ir_inst_id}};
}

auto ReplacePrevInstForMerge(Context& context, SemIR::NameScopeId scope_id,
                             SemIR::NameId name_id, SemIR::InstId new_inst_id)
    -> void {
  auto& names = context.name_scopes().Get(scope_id).names;
  auto it = names.find(name_id);
  if (it != names.end()) {
    it->second = new_inst_id;
  }
}

auto MergeImportRef(Context& context, SemIR::InstId new_inst_id,
                    SemIR::InstId prev_inst_id) -> void {
  auto new_inst = ResolveMergeableInst(context, new_inst_id);
  auto prev_inst = ResolveMergeableInst(context, prev_inst_id);
  if (!new_inst || !prev_inst) {
    // TODO: Once `var` declarations get an associated instruction for handling,
    // it might be more appropriate to return without diagnosing here, to handle
    // invalid declarations.
    context.DiagnoseDuplicateName(new_inst_id, prev_inst_id);
    return;
  }

  if (new_inst->inst.kind() != prev_inst->inst.kind()) {
    context.DiagnoseDuplicateName(new_inst_id, prev_inst_id);
    return;
  }

  CARBON_KIND_SWITCH(new_inst->inst) {
    case CARBON_KIND(SemIR::FunctionDecl new_decl): {
      auto prev_decl = prev_inst->inst.As<SemIR::FunctionDecl>();

      auto new_fn = context.functions().Get(new_decl.function_id);
      // TODO: May need to "spoil" the new function to prevent it from being
      // emitted, since it will already be added.
      MergeFunctionRedecl(context, context.insts().GetLocId(new_inst_id),
                          new_fn,
                          /*new_is_import=*/true,
                          /*new_is_definition=*/false, prev_decl.function_id,
                          prev_inst->import_ir_inst_id);
      return;
    }
    default:
      context.TODO(new_inst_id, "Merging not yet supported.");
      return;
  }
}

}  // namespace Carbon::Check
