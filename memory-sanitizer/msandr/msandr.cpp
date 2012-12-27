/* Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This file is a part of MemorySanitizer.

// Implementation of DynamoRIO instrumentation for MSan

#include "dr_api.h"
#include "drutil.h"
#include "drmgr.h"
#include "drsyscall.h"

#include <sys/mman.h>

#include <algorithm>
#include <string>
#include <set>
#include <vector>

using std::string;

#define TESTALL(mask, var) (((mask) & (var)) == (mask))
#define TESTANY(mask, var) (((mask) & (var)) != 0)

#define CHECK_IMPL(condition, file, line) \
    do { \
      if (!(condition)) { \
        dr_printf("Check failed: `%s`\nat %s:%d\n", #condition, file, line); \
        dr_abort(); \
      } \
    } while(0)  // TODO: stacktrace

#define CHECK(condition) CHECK_IMPL(condition, __FILE__, __LINE__)

// #define VERBOSE
// #define VERBOSE_VERBOSE

#if defined(VERBOSE_VERBOSE) && !defined(VERBOSE)
# define VERBOSE
#endif

#ifndef BINARY_INSTRUMENTED
# define BINARY_INSTRUMENTED 1
#endif

namespace {

class ModuleData {
 public:
  ModuleData();
  ModuleData(const module_data_t *info);
  // Yes, we want default copy, assign, and dtor semantics.

 public:
  app_pc start_;
  app_pc end_;
  // Full path to the module.
  string path_;
  module_handle_t handle_;
  bool should_instrument_;
  bool executed_;
};

string g_app_path;

int msan_retval_tls_offset;
int msan_param_tls_offset;

// A vector of loaded modules sorted by module bounds.  We lookup the current PC
// in here from the bb event.  This is better than an rb tree because the lookup
// is faster and the bb event occurs far more than the module load event.
std::vector<ModuleData> g_module_list;

ModuleData::ModuleData()
  : start_(NULL),
    end_(NULL),
    path_(""),
    handle_(NULL),
    should_instrument_(false),
    executed_(false)
{}

ModuleData::ModuleData(const module_data_t *info)
  : start_(info->start),
    end_(info->end),
    path_(info->full_path),
    handle_(info->handle),
    // We'll check the black/white lists later and adjust this.
    should_instrument_(true),
    executed_(false)
{}

int (*__msan_get_retval_tls_offset)();
int (*__msan_get_param_tls_offset)();

void InitializeMSanCallbacks() {
  module_data_t *app = dr_lookup_module_by_name(dr_get_application_name());
  if (!app) {
    dr_printf("%s - oops, dr_lookup_module_by_name failed!\n", dr_get_application_name());
    CHECK(app);
  }
  g_app_path = app->full_path;


  const char* callback_name = "__msan_get_retval_tls_offset";
  __msan_get_retval_tls_offset = (int(*)())dr_get_proc_address(app->handle, callback_name);
  if (__msan_get_retval_tls_offset == NULL) {
    dr_printf("Couldn't find `%s` in %s\n", callback_name, app->full_path);
    CHECK(__msan_get_retval_tls_offset);
  }

  callback_name = "__msan_get_param_tls_offset";
  __msan_get_param_tls_offset = (int(*)())dr_get_proc_address(app->handle, callback_name);
  if (__msan_get_param_tls_offset == NULL) {
    dr_printf("Couldn't find `%s` in %s\n", callback_name, app->full_path);
    CHECK(__msan_get_param_tls_offset);
  }
}

// typedef unsigned long uptr;

// #define MEM_TO_SHADOW(mem) ((mem) & ~0x400000000000ULL)
// static const uptr kMemBeg     = 0x7f0000000000;
// static const uptr kMemEnd     = 0x7fffffffffff;
// static const uptr kShadowBeg  = MEM_TO_SHADOW(kMemBeg);
// static const uptr kShadowEnd  = MEM_TO_SHADOW(kMemEnd);
// static const uptr kBad1Beg    = 0x200000;
// static const uptr kBad1End    = kShadowBeg - 1;
// static const uptr kBad2Beg    = kShadowEnd + 1;
// static const uptr kBad2End    = kMemBeg - 1;

// void InitializeMSanShadow() {
//   void* shadow = mmap((void*)kShadowBeg,
//       kShadowEnd - kShadowBeg,
//       PROT_READ | PROT_WRITE,
//       MAP_PRIVATE | MAP_ANON |
//       MAP_FIXED | MAP_NORESERVE,
//       0, 0);
//   CHECK(shadow == (void*)kShadowBeg);

//   void* bad2 = mmap((void*)(kBad2Beg), kBad2End - kBad2Beg,
//       PROT_NONE,
//       MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE,
//       -1, 0);
//   CHECK(bad2 == (void*)kBad2Beg);
// }

// TODO: Handle absolute addresses and PC-relative addresses.
// TODO: Handle TLS accesses via FS or GS.  DR assumes all other segments have a
// zero base anyway.
bool OperandIsInteresting(opnd_t opnd) {
  return (opnd_is_base_disp(opnd) &&
      opnd_get_segment(opnd) != DR_SEG_FS &&
      opnd_get_segment(opnd) != DR_SEG_GS);
}

bool WantToInstrument(instr_t *instr) {
  // TODO: skip push instructions?
  switch (instr_get_opcode(instr)) {
  // TODO: support the instructions excluded below:
  case OP_rep_cmps:
    // f3 a6    rep cmps %ds:(%rsi) %es:(%rdi) %rsi %rdi %rcx -> %rsi %rdi %rcx
    return false;
  }

  // Labels appear due to drutil_expand_rep_string()
  if (instr_is_label(instr))
    return false;

  CHECK(instr_ok_to_mangle(instr) == true);

  if (instr_writes_memory(instr)) {
    for (int d = 0; d < instr_num_dsts(instr); d++) {
      opnd_t op = instr_get_dst(instr, d);
      if (OperandIsInteresting(op))
        return true;
    }
  }

  return false;
}

#define PRE(at, what) instrlist_meta_preinsert(bb, at, INSTR_CREATE_##what);
#define PREF(at, what) instrlist_meta_preinsert(bb, at, what);

void InstrumentMops(void *drcontext, instrlist_t *bb,
                           instr_t *i, opnd_t op, bool is_write)
{
  bool need_to_restore_eflags = false;
  uint flags = instr_get_arith_flags(i);
  // TODO: do something smarter with flags and spills in general?
  // For example, spill them only once for a sequence of instrumented
  // instructions that don't change/read flags.

  if (!TESTALL(EFLAGS_WRITE_6, flags) || TESTANY(EFLAGS_READ_6, flags)) {
#if defined(VERBOSE_VERBOSE)
    dr_printf("Spilling eflags...\n");
#endif
    need_to_restore_eflags = true;
    // TODO: Maybe sometimes don't need to 'seto'.
    // TODO: Maybe sometimes don't want to spill XAX here?
    // TODO: No need to spill XAX here if XAX is not used in the BB.
    dr_save_reg(drcontext, bb, i, DR_REG_XAX, SPILL_SLOT_1);
    dr_save_arith_flags_to_xax(drcontext, bb, i);
    dr_save_reg(drcontext, bb, i, DR_REG_XAX, SPILL_SLOT_3);
    dr_restore_reg(drcontext, bb, i, DR_REG_XAX, SPILL_SLOT_1);
  }

#if 0
  dr_printf("==DRMSAN== DEBUG: %d %d %d %d %d %d\n",
            opnd_is_memory_reference(op),
            opnd_is_base_disp(op),
            opnd_is_base_disp(op) ? opnd_get_index(op) : -1,
            opnd_is_far_memory_reference(op),
            opnd_is_reg_pointer_sized(op),
            opnd_is_base_disp(op) ? opnd_get_disp(op) : -1
            );
#endif

  reg_id_t R1;
  bool address_in_R1 = false;
  if (opnd_is_base_disp(op) && opnd_get_index(op) == DR_REG_NULL &&
      opnd_get_disp(op) == 0) {
    // If this is a simple access with no offset or index, we can just use the
    // base for R1.
    address_in_R1 = true;
    R1 = opnd_get_base(op);
  } else {
    // Otherwise, we need to compute the addr into R1.
    // TODO: reuse some spare register? e.g. r15 on x64
    // TODO: might be used as a non-mem-ref register?
    R1 = DR_REG_XAX;
  }
  CHECK(reg_is_pointer_sized(R1));  // otherwise R1_8 and R2 may be wrong.
  reg_id_t R1_8 = reg_32_to_opsz(IF_X64_ELSE(reg_64_to_32(R1), R1), OPSZ_1);

  // Pick R2 that's not R1 or used by the operand.  It's OK if the instr uses
  // R2 elsewhere, since we'll restore it before instr.
  reg_id_t GPR_TO_USE_FOR_R2[] = {
    DR_REG_XAX, DR_REG_XBX, DR_REG_XCX, DR_REG_XDX
    // Don't forget to update the +4 below if you add anything else!
  };
  std::set<reg_id_t> unused_registers(GPR_TO_USE_FOR_R2, GPR_TO_USE_FOR_R2+4);
  unused_registers.erase(R1);
  for (int j = 0; j < opnd_num_regs_used(op); j++) {
    unused_registers.erase(opnd_get_reg_used(op, j));
  }

  CHECK(unused_registers.size() > 0);
  reg_id_t R2 = *unused_registers.begin(),
           R2_8 = reg_resize_to_opsz(R2, OPSZ_1);
  CHECK(R1 != R2);

  // Save the current values of R1 and R2.
  dr_save_reg(drcontext, bb, i, R1, SPILL_SLOT_1);
  // TODO: Something smarter than spilling a "fixed" register R2?
  dr_save_reg(drcontext, bb, i, R2, SPILL_SLOT_2);

  if (!address_in_R1)
    CHECK(drutil_insert_get_mem_addr(drcontext, bb, i, op, R1, R2));
  PRE(i, mov_imm(drcontext, opnd_create_reg(R2), OPND_CREATE_INT64(0xffffbfffffffffff)));
  PRE(i, and(drcontext, opnd_create_reg(R1), opnd_create_reg(R2)));
  // There is no mov_st of a 64-bit immediate, so...
  opnd_size_t op_size = opnd_get_size(op);
  CHECK(op_size != OPSZ_NA);
  uint access_size = opnd_size_in_bytes(op_size);
  if (access_size <= 4) {
    PRE(i, mov_st(drcontext, opnd_create_base_disp(R1, DR_REG_NULL, 0, 0, op_size),
            opnd_create_immed_int((ptr_int_t)0, op_size)));
  } else {
    // FIXME: tail?
    for (uint ofs = 0; ofs < access_size; ofs += 4) {
      PRE(i, mov_st(drcontext, OPND_CREATE_MEM32(R1, ofs), OPND_CREATE_INT32(0)));
    }
  }

  // Restore the registers and flags.
  dr_restore_reg(drcontext, bb, i, R1, SPILL_SLOT_1);
  dr_restore_reg(drcontext, bb, i, R2, SPILL_SLOT_2);

  if (need_to_restore_eflags) {
#if defined(VERBOSE_VERBOSE)
    dr_printf("Restoring eflags\n");
#endif
    // TODO: Check if it's reverse to the dr_restore_reg above and optimize.
    dr_save_reg(drcontext, bb, i, DR_REG_XAX, SPILL_SLOT_1);
    dr_restore_reg(drcontext, bb, i, DR_REG_XAX, SPILL_SLOT_3);
    dr_restore_arith_flags_from_xax(drcontext, bb, i);
    dr_restore_reg(drcontext, bb, i, DR_REG_XAX, SPILL_SLOT_1);
  }

  // The original instruction is left untouched. The above instrumentation is just
  // a prefix.
}


void InstrumentReturn(void *drcontext, instrlist_t *bb,
                           instr_t *i)
{

  instr_t* instr;

  dr_save_reg(drcontext, bb, i, DR_REG_XAX, SPILL_SLOT_1);

  // FIXME: I hope this does not change the flags.
  bool res = dr_insert_get_seg_base(drcontext, bb, i, DR_SEG_FS, DR_REG_XAX);
  CHECK(res);

  // TODO: unpoison more bytes?
  PRE(i, mov_st(drcontext,
          OPND_CREATE_MEM64(DR_REG_XAX, msan_retval_tls_offset),
          OPND_CREATE_INT32(0)));

  dr_restore_reg(drcontext, bb, i, DR_REG_XAX, SPILL_SLOT_1);

  // The original instruction is left untouched. The above instrumentation is just
  // a prefix.
}

void InstrumentIndirectBranch(void *drcontext, instrlist_t *bb,
    instr_t *i)
{

  instr_t* instr;

  dr_save_reg(drcontext, bb, i, DR_REG_XAX, SPILL_SLOT_1);

  // FIXME: I hope this does not change the flags.
  bool res = dr_insert_get_seg_base(drcontext, bb, i, DR_SEG_FS, DR_REG_XAX);
  CHECK(res);

  // TODO: unpoison more bytes?
  PRE(i, mov_st(drcontext,
          OPND_CREATE_MEM64(DR_REG_XAX, msan_param_tls_offset),
          OPND_CREATE_INT32(0)));
  PRE(i, mov_st(drcontext,
          OPND_CREATE_MEM64(DR_REG_XAX, msan_param_tls_offset + 8),
          OPND_CREATE_INT32(0)));
  PRE(i, mov_st(drcontext,
          OPND_CREATE_MEM64(DR_REG_XAX, msan_param_tls_offset + 16),
          OPND_CREATE_INT32(0)));
  PRE(i, mov_st(drcontext,
          OPND_CREATE_MEM64(DR_REG_XAX, msan_param_tls_offset + 24),
          OPND_CREATE_INT32(0)));
  PRE(i, mov_st(drcontext,
          OPND_CREATE_MEM64(DR_REG_XAX, msan_param_tls_offset + 32),
          OPND_CREATE_INT32(0)));
  PRE(i, mov_st(drcontext,
          OPND_CREATE_MEM64(DR_REG_XAX, msan_param_tls_offset + 40),
          OPND_CREATE_INT32(0)));

  dr_restore_reg(drcontext, bb, i, DR_REG_XAX, SPILL_SLOT_1);

  // The original instruction is left untouched. The above instrumentation is just
  // a prefix.
}


// For use with binary search.  Modules shouldn't overlap, so we shouldn't have
// to look at end_.  If that can happen, we won't support such an application.
bool ModuleDataCompareStart(const ModuleData &left,
                                   const ModuleData &right) {
  return left.start_ < right.start_;
}

// Look up the module containing PC.  Should be relatively fast, as its called
// for each bb instrumentation.
ModuleData *LookupModuleByPC(app_pc pc) {
  ModuleData fake_mod_data;
  fake_mod_data.start_ = pc;
  std::vector<ModuleData>::iterator it =
      lower_bound(g_module_list.begin(), g_module_list.end(), fake_mod_data,
                  ModuleDataCompareStart);
  // if (it == g_module_list.end())
  //   return NULL;
  if (it == g_module_list.end() || pc < it->start_)
    --it;
  CHECK(it->start_ <= pc);
  if (pc >= it->end_) {
    // We're past the end of this module.  We shouldn't be in the next module,
    // or lower_bound lied to us.
    ++it;
    CHECK(it == g_module_list.end() || pc < it->start_);
    return NULL;
  }

  // OK, we found the module.
  return &*it;
}

bool ShouldInstrumentNonModuleCode() {
  return true;
}

bool ShouldInstrumentModule(ModuleData *mod_data) {
  // TODO(rnk): Flags for blacklist would get wired in here.
  generic_func_t p = dr_get_proc_address(mod_data->handle_, "__msan_track_origins");
  return !p;
}

bool ShouldInstrumentPc(app_pc pc, ModuleData** pmod_data) {
  ModuleData *mod_data = LookupModuleByPC(pc);
  if (pmod_data) *pmod_data = mod_data;
  if (mod_data != NULL) {
    // This module is on a blacklist.
    if (!mod_data->should_instrument_) {
      return false;
    }
  } else if (!ShouldInstrumentNonModuleCode()) {
    return false;
  }
  return true;
}

// TODO(rnk): Make sure we instrument after __asan_init.
dr_emit_flags_t event_basic_block_app2app(void *drcontext, void *tag, instrlist_t *bb,
                                  bool for_trace, bool translating) {
  app_pc pc = dr_fragment_app_pc(tag);

  if (ShouldInstrumentPc(pc, NULL))
    CHECK(drutil_expand_rep_string(drcontext, bb));

  return DR_EMIT_DEFAULT;
}

dr_emit_flags_t event_basic_block(void *drcontext, void *tag, instrlist_t *bb,
                                  bool for_trace, bool translating) {
  app_pc pc = dr_fragment_app_pc(tag);
  ModuleData* mod_data;

  if (!ShouldInstrumentPc(pc, &mod_data))
    return DR_EMIT_DEFAULT;

#if defined(VERBOSE)
# if defined(VERBOSE_VERBOSE)
  dr_printf("============================================================\n");
# endif
  string mod_path = (mod_data ? mod_data->path_ : "<no module, JITed?>");
  if (mod_data && !mod_data->executed_) {
    mod_data->executed_ = true;  // Nevermind this race.
    dr_printf("Executing from new module: %s\n", mod_path.c_str());
  }
  dr_printf("BB to be instrumented: %p [from %s]; translating = %s\n",
            pc, mod_path.c_str(), translating ? "true" : "false");
  if (mod_data) {
    // Match standard asan trace format for free symbols.
    // #0 0x7f6e35cf2e45  (/blah/foo.so+0x11fe45)
    dr_printf(" #0 %p (%s+%p)\n", pc,
              mod_data->path_.c_str(),
              pc - mod_data->start_);
  }
# if defined(VERBOSE_VERBOSE)
  instrlist_disassemble(drcontext, pc, bb, STDOUT);
  instr_t *instr;
  for (instr = instrlist_first(bb); instr; instr = instr_get_next(instr)) {
    dr_printf("opcode: %d\n", instr_get_opcode(instr));
  }
# endif
#endif

  for (instr_t *i = instrlist_first(bb); i != NULL; i = instr_get_next(i)) {
    int opcode = instr_get_opcode(i);
    if (opcode == OP_ret || opcode == OP_ret_far) {
      InstrumentReturn(drcontext, bb, i);
      continue;
    }

    if (opcode == OP_call_ind || opcode == OP_call_far_ind ||
        opcode == OP_jmp_ind || opcode == OP_jmp_far_ind) {
      InstrumentIndirectBranch(drcontext, bb, i);
      continue;
    }

    if (!WantToInstrument(i))
      continue;

#if defined(VERBOSE_VERBOSE)
    app_pc orig_pc = dr_fragment_app_pc(tag);
    uint flags = instr_get_arith_flags(i);
    dr_printf("+%d -> to be instrumented! [opcode=%d, flags = 0x%08X]\n",
              instr_get_app_pc(i) - orig_pc, instr_get_opcode(i), flags);
#endif

    if (instr_writes_memory(i)) {
      // Instrument memory writes
      // bool instrumented_anything = false;
      for (int d = 0; d < instr_num_dsts(i); d++) {
        opnd_t op = instr_get_dst(i, d);
        if (!OperandIsInteresting(op))
          continue;

        // CHECK(!instrumented_anything);
        // instrumented_anything = true;
        InstrumentMops(drcontext, bb, i, op, true);
        break; // only instrumenting the first dst
      }
    }
  }

  // TODO: optimize away redundant restore-spill pairs?

#if defined(VERBOSE_VERBOSE)
  pc = dr_fragment_app_pc(tag);
  dr_printf("\nFinished instrumenting dynamorio_basic_block(PC="PFX")\n", pc);
  instrlist_disassemble(drcontext, pc, bb, STDOUT);
#endif
  return DR_EMIT_DEFAULT;
}

void event_module_load(void *drcontext, const module_data_t *info, bool loaded) {
  // Insert the module into the list while maintaining the ordering.
  ModuleData mod_data(info);
  std::vector<ModuleData>::iterator it =
      upper_bound(g_module_list.begin(), g_module_list.end(), mod_data,
                  ModuleDataCompareStart);
  it = g_module_list.insert(it, mod_data);
  // Check if we should instrument this module.
  it->should_instrument_ = ShouldInstrumentModule(&*it);
  dr_module_set_should_instrument(info->handle, it->should_instrument_);

#if defined(VERBOSE)
  dr_printf("==DRMSAN== Loaded module: %s [%p...%p], instrumentation is %s\n",
            info->full_path, info->start, info->end,
            it->should_instrument_ ? "on" : "off");
#endif
}

void event_module_unload(void *drcontext, const module_data_t *info) {
#if defined(VERBOSE)
  dr_printf("==DRMSAN== Unloaded module: %s [%p...%p]\n",
            info->full_path, info->start, info->end);
#endif

  // Remove the module from the list.
  ModuleData mod_data(info);
  std::vector<ModuleData>::iterator it =
      lower_bound(g_module_list.begin(), g_module_list.end(), mod_data,
                  ModuleDataCompareStart);
  // It's a bug if we didn't actually find the module.
  CHECK(it != g_module_list.end() &&
        it->start_ == mod_data.start_ &&
        it->end_ == mod_data.end_ &&
        it->path_ == mod_data.path_);
  g_module_list.erase(it);
}

void event_exit() {
#if defined(VERBOSE)
  dr_printf("==DRMSAN== DONE\n");
#endif
}

}  // namespace

DR_EXPORT void dr_init(client_id_t id) {
  drmgr_init();
  drutil_init();

  string app_name = dr_get_application_name();
  // This blacklist will still run these apps through DR's code cache.  On the
  // other hand, we are able to follow children of these apps.
  // TODO(rnk): Once DR has detach, we could just detach here.  Alternatively,
  // if DR had a fork or exec hook to let us decide there, that would be nice.
  // TODO: make the blacklist cmd-adjustable.
  if (app_name == "python" ||
      app_name == "bash" || app_name == "sh" ||
      app_name == "true" || app_name == "exit" ||
      app_name == "yes" || app_name == "echo")
    return;

  InitializeMSanCallbacks();
  // FIXME: the shadow is initialized earlier when DR calls one of our wrapper functions
  // This may change one day.
  // TODO: make this more robust.
  // InitializeMSanShadow();

  void* drcontext = dr_get_current_drcontext();

  dr_switch_to_app_state(drcontext);
  msan_retval_tls_offset = __msan_get_retval_tls_offset();
  msan_param_tls_offset = __msan_get_param_tls_offset();
  dr_switch_to_dr_state(drcontext);
#if defined(VERBOSE)
  dr_printf("__msan_retval_tls offset: %d\n", msan_retval_tls_offset);
  dr_printf("__msan_param_tls offset: %d\n", msan_param_tls_offset);
#endif

  // Standard DR events.
  dr_register_exit_event(event_exit);

  drmgr_priority_t priority = {
    sizeof(priority), /* size of struct */
    "msandr",         /* name of our operation */
    NULL,             /* optional name of operation we should precede */
    NULL,             /* optional name of operation we should follow */
    0};               /* numeric priority */

  drmgr_register_bb_app2app_event(event_basic_block_app2app, &priority);
  drmgr_register_bb_instru2instru_event(event_basic_block, &priority);
  drmgr_register_module_load_event(event_module_load);
  drmgr_register_module_unload_event(event_module_unload);
#if defined(VERBOSE)
  dr_printf("==DRMSAN== Starting!\n");
#endif
}
