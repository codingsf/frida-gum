/*
 * Copyright (C) 2010-2017 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumthumbwriter.h"

#include "gumarmreg.h"
#include "gumlibc.h"
#include "gummemory.h"
#include "gumprocess.h"

#include <string.h>

#define GUM_MAX_LABEL_COUNT       100
#define GUM_MAX_LREF_COUNT        (3 * GUM_MAX_LABEL_COUNT)
#define GUM_MAX_LITERAL_REF_COUNT 100

typedef guint GumThumbMemoryOperation;

struct _GumThumbLabelMapping
{
  gconstpointer id;
  GumAddress address;
};

struct _GumThumbLabelRef
{
  gconstpointer id;
  guint16 * insn;
  GumAddress pc;
};

struct _GumThumbLiteralRef
{
  guint32 val;
  guint16 * insn;
  GumAddress pc;
};

enum _GumThumbMemoryOperation
{
  GUM_THUMB_MEMORY_LOAD,
  GUM_THUMB_MEMORY_STORE
};

static GumAddress gum_thumb_writer_lookup_address_for_label_id (
    GumThumbWriter * self, gconstpointer id);
static void gum_thumb_writer_put_argument_list_setup (GumThumbWriter * self,
    guint n_args, const GumArgument * args);
static void gum_thumb_writer_put_argument_list_setup_va (GumThumbWriter * self,
    guint n_args, va_list vl);
static void gum_thumb_writer_put_argument_list_teardown (GumThumbWriter * self,
    guint n_args);
static void gum_thumb_writer_put_branch_imm (GumThumbWriter * self,
    GumAddress target, gboolean link, gboolean thumb);
static gboolean gum_thumb_writer_put_push_or_pop_regs (GumThumbWriter * self,
    guint16 narrow_opcode, guint16 wide_opcode, GumArmMetaReg special_reg,
    guint n_regs, const arm_reg * regs);
static gboolean gum_thumb_writer_put_push_or_pop_regs_va (GumThumbWriter * self,
    guint16 narrow_opcode, guint16 wide_opcode, GumArmMetaReg special_reg,
    guint n_regs, arm_reg first_reg, va_list vl);
static gboolean gum_thumb_writer_put_transfer_reg_reg_offset (
    GumThumbWriter * self, GumThumbMemoryOperation operation, arm_reg left_reg,
    arm_reg right_reg, gsize right_offset);

static gboolean gum_instruction_is_t1_load (guint16 instruction);

GumThumbWriter *
gum_thumb_writer_new (gpointer code_address)
{
  GumThumbWriter * writer;

  writer = g_slice_new (GumThumbWriter);

  gum_thumb_writer_init (writer, code_address);

  return writer;
}

GumThumbWriter *
gum_thumb_writer_ref (GumThumbWriter * writer)
{
  g_atomic_int_inc (&writer->ref_count);

  return writer;
}

void
gum_thumb_writer_unref (GumThumbWriter * writer)
{
  if (g_atomic_int_dec_and_test (&writer->ref_count))
  {
    gum_thumb_writer_clear (writer);

    g_slice_free (GumThumbWriter, writer);
  }
}

void
gum_thumb_writer_init (GumThumbWriter * writer,
                       gpointer code_address)
{
  writer->ref_count = 1;

  writer->id_to_address = g_new (GumThumbLabelMapping, GUM_MAX_LABEL_COUNT);
  writer->label_refs = g_new (GumThumbLabelRef, GUM_MAX_LREF_COUNT);
  writer->literal_refs = g_new (GumThumbLiteralRef, GUM_MAX_LITERAL_REF_COUNT);

  gum_thumb_writer_reset (writer, code_address);
}

void
gum_thumb_writer_clear (GumThumbWriter * writer)
{
  gum_thumb_writer_flush (writer);

  g_free (writer->id_to_address);
  g_free (writer->label_refs);
  g_free (writer->literal_refs);
}

void
gum_thumb_writer_reset (GumThumbWriter * writer,
                        gpointer code_address)
{
  writer->target_os = gum_process_get_native_os ();

  writer->base = code_address;
  writer->code = code_address;
  writer->pc = GUM_ADDRESS (code_address);

  writer->id_to_address_len = 0;
  writer->label_refs_len = 0;
  writer->literal_refs_len = 0;
}

void
gum_thumb_writer_set_target_os (GumThumbWriter * self,
                                GumOS os)
{
  self->target_os = os;
}

gpointer
gum_thumb_writer_cur (GumThumbWriter * self)
{
  return self->code;
}

guint
gum_thumb_writer_offset (GumThumbWriter * self)
{
  return (guint) (self->code - self->base) * sizeof (guint16);
}

void
gum_thumb_writer_skip (GumThumbWriter * self,
                       guint n_bytes)
{
  self->code = (guint16 *) (((guint8 *) self->code) + n_bytes);
  self->pc += n_bytes;
}

gboolean
gum_thumb_writer_flush (GumThumbWriter * self)
{
  if (self->label_refs_len > 0)
  {
    guint label_idx;

    for (label_idx = 0; label_idx != self->label_refs_len; label_idx++)
    {
      GumThumbLabelRef * r = &self->label_refs[label_idx];
      GumAddress target_address;
      gssize distance;
      guint16 insn;

      target_address =
          gum_thumb_writer_lookup_address_for_label_id (self, r->id);
      if (target_address == 0)
        goto error;

      distance = ((gint32) target_address - (gint32) r->pc) / 2;

      insn = GUINT16_FROM_LE (*r->insn);
      if ((insn & 0xf000) == 0xd000)
      {
        if (!GUM_IS_WITHIN_INT8_RANGE (distance))
          goto error;
        insn |= distance & GUM_INT8_MASK;
      }
      else if ((insn & 0xf800) == 0xe000)
      {
        if (!GUM_IS_WITHIN_INT11_RANGE (distance))
          goto error;
        insn |= distance & GUM_INT11_MASK;
      }
      else
      {
        guint16 i, imm5;

        if (!GUM_IS_WITHIN_UINT7_RANGE (distance))
          goto error;

        i = (distance >> 5) & 1;
        imm5 = distance & GUM_INT5_MASK;

        insn |= (i << 9) | (imm5 << 3);
      }

      *r->insn = GUINT16_TO_LE (insn);
    }
    self->label_refs_len = 0;
  }

  if (self->literal_refs_len > 0)
  {
    gboolean need_aligned_slots;
    guint32 * first_slot, * last_slot;
    guint ref_idx;

    need_aligned_slots = FALSE;
    for (ref_idx = 0; ref_idx != self->literal_refs_len; ref_idx++)
    {
      guint16 insn = GUINT16_FROM_LE (self->literal_refs[ref_idx].insn[0]);
      if (gum_instruction_is_t1_load (insn))
        need_aligned_slots = TRUE;
    }

    if (need_aligned_slots && (self->pc & 3) != 0)
      gum_thumb_writer_put_nop (self);

    first_slot = (guint32 *) self->code;
    last_slot = first_slot;

    for (ref_idx = 0; ref_idx != self->literal_refs_len; ref_idx++)
    {
      GumThumbLiteralRef * r;
      guint16 insn;
      guint32 * slot;
      GumAddress slot_pc;
      gsize distance_in_bytes;

      r = &self->literal_refs[ref_idx];
      insn = GUINT16_FROM_LE (r->insn[0]);

      for (slot = first_slot; slot != last_slot; slot++)
      {
        if (*slot == r->val)
          break;
      }

      if (slot == last_slot)
      {
        *slot = r->val;
        self->code += 2;
        self->pc += 4;
        last_slot++;
      }

      slot_pc = self->pc - ((guint8 *) last_slot - (guint8 *) first_slot) +
          ((guint8 *) slot - (guint8 *) first_slot);

      distance_in_bytes = slot_pc - (r->pc & ~((GumAddress) 3));

      if (gum_instruction_is_t1_load (insn))
      {
        r->insn[0] = GUINT16_TO_LE (insn | (distance_in_bytes / 4));
      }
      else
      {
        r->insn[1] = GUINT16_TO_LE (GUINT16_FROM_LE (r->insn[1]) |
            distance_in_bytes);
      }
    }
    self->literal_refs_len = 0;
  }

  return TRUE;

error:
  {
    self->label_refs_len = 0;
    self->literal_refs_len = 0;

    return FALSE;
  }
}

static GumAddress
gum_thumb_writer_lookup_address_for_label_id (GumThumbWriter * self,
                                              gconstpointer id)
{
  guint i;

  for (i = 0; i < self->id_to_address_len; i++)
  {
    GumThumbLabelMapping * map = &self->id_to_address[i];
    if (map->id == id)
      return map->address;
  }

  return 0;
}

static gboolean
gum_thumb_writer_add_address_for_label_id (GumThumbWriter * self,
                                           gconstpointer id,
                                           GumAddress address)
{
  GumThumbLabelMapping * map;

  if (self->id_to_address_len == GUM_MAX_LABEL_COUNT)
    return FALSE;

  map = &self->id_to_address[self->id_to_address_len++];
  map->id = id;
  map->address = address;

  return TRUE;
}

gboolean
gum_thumb_writer_put_label (GumThumbWriter * self,
                            gconstpointer id)
{
  if (gum_thumb_writer_lookup_address_for_label_id (self, id) != 0)
    return FALSE;

  return gum_thumb_writer_add_address_for_label_id (self, id, self->pc);
}

static gboolean
gum_thumb_writer_add_label_reference_here (GumThumbWriter * self,
                                           gconstpointer id)
{
  GumThumbLabelRef * r;

  if (self->label_refs_len == GUM_MAX_LREF_COUNT)
    return FALSE;

  r = &self->label_refs[self->label_refs_len++];
  r->id = id;
  r->insn = self->code;
  r->pc = self->pc + 4;

  return TRUE;
}

static gboolean
gum_thumb_writer_add_literal_reference_here (GumThumbWriter * self,
                                             guint32 val)
{
  GumThumbLiteralRef * r;

  if (self->literal_refs_len == GUM_MAX_LITERAL_REF_COUNT)
    return FALSE;

  r = &self->literal_refs[self->literal_refs_len++];
  r->val = val;
  r->insn = self->code;
  r->pc = self->pc + 4;

  return TRUE;
}

void
gum_thumb_writer_put_call_address_with_arguments (GumThumbWriter * self,
                                                  GumAddress func,
                                                  guint n_args,
                                                  ...)
{
  va_list vl;

  va_start (vl, n_args);
  gum_thumb_writer_put_argument_list_setup_va (self, n_args, vl);
  va_end (vl);

  gum_thumb_writer_put_ldr_reg_address (self, ARM_REG_LR, func);
  gum_thumb_writer_put_blx_reg (self, ARM_REG_LR);

  gum_thumb_writer_put_argument_list_teardown (self, n_args);
}

void
gum_thumb_writer_put_call_address_with_arguments_array (
    GumThumbWriter * self,
    GumAddress func,
    guint n_args,
    const GumArgument * args)
{
  gum_thumb_writer_put_argument_list_setup (self, n_args, args);

  gum_thumb_writer_put_ldr_reg_address (self, ARM_REG_LR, func);
  gum_thumb_writer_put_blx_reg (self, ARM_REG_LR);

  gum_thumb_writer_put_argument_list_teardown (self, n_args);
}

void
gum_thumb_writer_put_call_reg_with_arguments (GumThumbWriter * self,
                                              arm_reg reg,
                                              guint n_args,
                                              ...)
{
  va_list vl;

  va_start (vl, n_args);
  gum_thumb_writer_put_argument_list_setup_va (self, n_args, vl);
  va_end (vl);

  gum_thumb_writer_put_blx_reg (self, reg);

  gum_thumb_writer_put_argument_list_teardown (self, n_args);
}

void
gum_thumb_writer_put_call_reg_with_arguments_array (GumThumbWriter * self,
                                                    arm_reg reg,
                                                    guint n_args,
                                                    const GumArgument * args)
{
  gum_thumb_writer_put_argument_list_setup (self, n_args, args);

  gum_thumb_writer_put_blx_reg (self, reg);

  gum_thumb_writer_put_argument_list_teardown (self, n_args);
}

static void
gum_thumb_writer_put_argument_list_setup (GumThumbWriter * self,
                                          guint n_args,
                                          const GumArgument * args)
{
  gint arg_index;

  for (arg_index = (gint) n_args - 1; arg_index >= 0; arg_index--)
  {
    const GumArgument * arg = &args[arg_index];
    arm_reg r = ARM_REG_R0 + arg_index;

    if (arg_index < 4)
    {
      if (arg->type == GUM_ARG_ADDRESS)
      {
        gum_thumb_writer_put_ldr_reg_address (self, r, arg->value.address);
      }
      else
      {
        if (arg->value.reg != r)
          gum_thumb_writer_put_mov_reg_reg (self, r, arg->value.reg);
      }
    }
    else
    {
      if (arg->type == GUM_ARG_ADDRESS)
      {
        gum_thumb_writer_put_ldr_reg_address (self, ARM_REG_R0,
            arg->value.address);
        gum_thumb_writer_put_push_regs (self, 1, ARM_REG_R0);
      }
      else
      {
        gum_thumb_writer_put_push_regs (self, 1, arg->value.reg);
      }
    }
  }
}

static void
gum_thumb_writer_put_argument_list_setup_va (GumThumbWriter * self,
                                             guint n_args,
                                             va_list vl)
{
  GumArgument * args;
  guint arg_index;

  args = g_alloca (n_args * sizeof (GumArgument));

  for (arg_index = 0; arg_index != n_args; arg_index++)
  {
    GumArgument * arg = &args[arg_index];

    arg->type = va_arg (vl, GumArgType);
    if (arg->type == GUM_ARG_ADDRESS)
      arg->value.address = va_arg (vl, GumAddress);
    else if (arg->type == GUM_ARG_REGISTER)
      arg->value.reg = va_arg (vl, arm_reg);
    else
      g_assert_not_reached ();
  }

  gum_thumb_writer_put_argument_list_setup (self, n_args, args);
}

static void
gum_thumb_writer_put_argument_list_teardown (GumThumbWriter * self,
                                             guint n_args)
{
  (void) self;
  (void) n_args;
}

void
gum_thumb_writer_put_b_imm (GumThumbWriter * self,
                            GumAddress target)
{
  gum_thumb_writer_put_branch_imm (self, target, FALSE, TRUE);
}

void
gum_thumb_writer_put_bx_reg (GumThumbWriter * self,
                             arm_reg reg)
{
  GumArmRegInfo ri;

  gum_arm_reg_describe (reg, &ri);

  gum_thumb_writer_put_instruction (self, 0x4700 | (ri.index << 3));
}

void
gum_thumb_writer_put_blx_reg (GumThumbWriter * self,
                              arm_reg reg)
{
  GumArmRegInfo ri;

  gum_arm_reg_describe (reg, &ri);

  gum_thumb_writer_put_instruction (self, 0x4780 | (ri.index << 3));
}

void
gum_thumb_writer_put_bl_imm (GumThumbWriter * self,
                             GumAddress target)
{
  gum_thumb_writer_put_branch_imm (self, target, TRUE, TRUE);
}

void
gum_thumb_writer_put_blx_imm (GumThumbWriter * self,
                              GumAddress target)
{
  gum_thumb_writer_put_branch_imm (self, target, TRUE, FALSE);
}

static void
gum_thumb_writer_put_branch_imm (GumThumbWriter * self,
                                 GumAddress target,
                                 gboolean link,
                                 gboolean thumb)
{
  union
  {
    gint32 i;
    guint32 u;
  } distance;
  guint16 s, j1, j2, imm10, imm11;

  distance.i = ((gint32) (target & ~((GumAddress) 1)) -
      (gint32) (self->pc + 4)) / 2;

  s = (distance.u >> 31) & 1;
  j1 = (~((distance.u >> 22) ^ s)) & 1;
  j2 = (~((distance.u >> 21) ^ s)) & 1;

  imm10 = (distance.u >> 11) & GUM_INT10_MASK;
  imm11 = distance.u & GUM_INT11_MASK;

  gum_thumb_writer_put_instruction (self, 0xf000 | (s << 10) | imm10);
  gum_thumb_writer_put_instruction (self, 0x8000 | (link << 14) | (j1 << 13) |
      (thumb << 12) | (j2 << 11) | imm11);
}

void
gum_thumb_writer_put_cmp_reg_imm (GumThumbWriter * self,
                                  arm_reg reg,
                                  guint8 imm_value)
{
  GumArmRegInfo ri;

  gum_arm_reg_describe (reg, &ri);

  gum_thumb_writer_put_instruction (self, 0x2800 | (ri.index << 8) | imm_value);
}

gboolean
gum_thumb_writer_put_b_label (GumThumbWriter * self,
                              gconstpointer label_id)
{
  if (!gum_thumb_writer_add_label_reference_here (self, label_id))
    return FALSE;

  gum_thumb_writer_put_instruction (self, 0xe000);

  return TRUE;
}

gboolean
gum_thumb_writer_put_beq_label (GumThumbWriter * self,
                                gconstpointer label_id)
{
  return gum_thumb_writer_put_b_cond_label (self, ARM_CC_EQ, label_id);
}

gboolean
gum_thumb_writer_put_bne_label (GumThumbWriter * self,
                                gconstpointer label_id)
{
  return gum_thumb_writer_put_b_cond_label (self, ARM_CC_NE, label_id);
}

gboolean
gum_thumb_writer_put_b_cond_label (GumThumbWriter * self,
                                   arm_cc cc,
                                   gconstpointer label_id)
{
  if (!gum_thumb_writer_add_label_reference_here (self, label_id))
    return FALSE;

  gum_thumb_writer_put_instruction (self, 0xd000 | ((cc - 1) << 8));

  return TRUE;
}

gboolean
gum_thumb_writer_put_cbz_reg_label (GumThumbWriter * self,
                                    arm_reg reg,
                                    gconstpointer label_id)
{
  GumArmRegInfo ri;

  gum_arm_reg_describe (reg, &ri);

  if (!gum_thumb_writer_add_label_reference_here (self, label_id))
    return FALSE;

  gum_thumb_writer_put_instruction (self, 0xb100 | ri.index);

  return TRUE;
}

gboolean
gum_thumb_writer_put_cbnz_reg_label (GumThumbWriter * self,
                                     arm_reg reg,
                                     gconstpointer label_id)
{
  GumArmRegInfo ri;

  gum_arm_reg_describe (reg, &ri);

  if (!gum_thumb_writer_add_label_reference_here (self, label_id))
    return FALSE;

  gum_thumb_writer_put_instruction (self, 0xb900 | ri.index);

  return TRUE;
}

gboolean
gum_thumb_writer_put_push_regs (GumThumbWriter * self,
                                guint n_regs,
                                arm_reg first_reg,
                                ...)
{
  gboolean success;
  va_list vl;

  va_start (vl, first_reg);
  success = gum_thumb_writer_put_push_or_pop_regs_va (self, 0xb400, 0xe92d,
      GUM_ARM_MREG_LR, n_regs, first_reg, vl);
  va_end (vl);

  return success;
}

gboolean
gum_thumb_writer_put_push_regs_array (GumThumbWriter * self,
                                      guint n_regs,
                                      const arm_reg * regs)
{
  return gum_thumb_writer_put_push_or_pop_regs (self, 0xb400, 0xe92d,
      GUM_ARM_MREG_LR, n_regs, regs);
}

gboolean
gum_thumb_writer_put_pop_regs (GumThumbWriter * self,
                               guint n_regs,
                               arm_reg first_reg,
                               ...)
{
  gboolean success;
  va_list vl;

  va_start (vl, first_reg);
  success = gum_thumb_writer_put_push_or_pop_regs_va (self, 0xbc00, 0xe8bd,
      GUM_ARM_MREG_PC, n_regs, first_reg, vl);
  va_end (vl);

  return success;
}

gboolean
gum_thumb_writer_put_pop_regs_array (GumThumbWriter * self,
                                     guint n_regs,
                                     const arm_reg * regs)
{
  return gum_thumb_writer_put_push_or_pop_regs (self, 0xbc00, 0xe8bd,
      GUM_ARM_MREG_PC, n_regs, regs);
}

static gboolean
gum_thumb_writer_put_push_or_pop_regs (GumThumbWriter * self,
                                       guint16 narrow_opcode,
                                       guint16 wide_opcode,
                                       GumArmMetaReg special_reg,
                                       guint n_regs,
                                       const arm_reg * regs)
{
  GumArmRegInfo * items;
  gboolean need_wide_instruction;
  guint reg_index;

  if (n_regs == 0)
    return FALSE;

  items = g_alloca (n_regs * sizeof (GumArmRegInfo));
  need_wide_instruction = FALSE;
  for (reg_index = 0; reg_index != n_regs; reg_index++)
  {
    GumArmRegInfo * ri = &items[reg_index];
    gboolean is_low_reg;

    gum_arm_reg_describe (regs[reg_index], ri);

    is_low_reg = (ri->meta >= GUM_ARM_MREG_R0 && ri->meta <= GUM_ARM_MREG_R7);
    if (!is_low_reg && ri->meta != special_reg)
      need_wide_instruction = TRUE;
  }

  if (need_wide_instruction)
  {
    guint16 mask = 0;

    gum_thumb_writer_put_instruction (self, wide_opcode);

    for (reg_index = 0; reg_index != n_regs; reg_index++)
    {
      const GumArmRegInfo * ri = &items[reg_index];

      mask |= (1 << ri->index);
    }

    gum_thumb_writer_put_instruction (self, mask);
  }
  else
  {
    guint16 insn = narrow_opcode;

    for (reg_index = 0; reg_index != n_regs; reg_index++)
    {
      const GumArmRegInfo * ri = &items[reg_index];

      if (ri->meta == special_reg)
        insn |= 0x0100;
      else
        insn |= (1 << ri->index);
    }

    gum_thumb_writer_put_instruction (self, insn);
  }

  return TRUE;
}

static gboolean
gum_thumb_writer_put_push_or_pop_regs_va (GumThumbWriter * self,
                                          guint16 narrow_opcode,
                                          guint16 wide_opcode,
                                          GumArmMetaReg special_reg,
                                          guint n_regs,
                                          arm_reg first_reg,
                                          va_list vl)
{
  arm_reg * regs;
  guint reg_index;

  g_assert_cmpuint (n_regs, !=, 0);

  regs = g_alloca (n_regs * sizeof (GumArmRegInfo));

  for (reg_index = 0; reg_index != n_regs; reg_index++)
  {
    regs[reg_index] = (reg_index == 0) ? first_reg : va_arg (vl, arm_reg);
  }

  return gum_thumb_writer_put_push_or_pop_regs (self, narrow_opcode,
      wide_opcode, special_reg, n_regs, regs);
}

gboolean
gum_thumb_writer_put_ldr_reg_address (GumThumbWriter * self,
                                      arm_reg reg,
                                      GumAddress address)
{
  return gum_thumb_writer_put_ldr_reg_u32 (self, reg, (guint32) address);
}

gboolean
gum_thumb_writer_put_ldr_reg_u32 (GumThumbWriter * self,
                                  arm_reg reg,
                                  guint32 val)
{
  GumArmRegInfo ri;

  gum_arm_reg_describe (reg, &ri);

  if (!gum_thumb_writer_add_literal_reference_here (self, val))
    return FALSE;

  if (ri.meta <= GUM_ARM_MREG_R7)
  {
    gum_thumb_writer_put_instruction (self, 0x4800 | (ri.index << 8));
  }
  else
  {
    gboolean add = TRUE;

    gum_thumb_writer_put_instruction (self, 0xf85f | (add << 7));
    gum_thumb_writer_put_instruction (self, (ri.index << 12));
  }

  return TRUE;
}

void
gum_thumb_writer_put_ldr_reg_reg (GumThumbWriter * self,
                                  arm_reg dst_reg,
                                  arm_reg src_reg)
{
  gum_thumb_writer_put_ldr_reg_reg_offset (self, dst_reg, src_reg, 0);
}

gboolean
gum_thumb_writer_put_ldr_reg_reg_offset (GumThumbWriter * self,
                                         arm_reg dst_reg,
                                         arm_reg src_reg,
                                         gsize src_offset)
{
  return gum_thumb_writer_put_transfer_reg_reg_offset (self,
      GUM_THUMB_MEMORY_LOAD, dst_reg, src_reg, src_offset);
}

void
gum_thumb_writer_put_str_reg_reg (GumThumbWriter * self,
                                  arm_reg src_reg,
                                  arm_reg dst_reg)
{
  gum_thumb_writer_put_str_reg_reg_offset (self, src_reg, dst_reg, 0);
}

gboolean
gum_thumb_writer_put_str_reg_reg_offset (GumThumbWriter * self,
                                         arm_reg src_reg,
                                         arm_reg dst_reg,
                                         gsize dst_offset)
{
  return gum_thumb_writer_put_transfer_reg_reg_offset (self,
      GUM_THUMB_MEMORY_STORE, src_reg, dst_reg, dst_offset);
}

static gboolean
gum_thumb_writer_put_transfer_reg_reg_offset (GumThumbWriter * self,
                                              GumThumbMemoryOperation operation,
                                              arm_reg left_reg,
                                              arm_reg right_reg,
                                              gsize right_offset)
{
  GumArmRegInfo lr, rr;

  gum_arm_reg_describe (left_reg, &lr);
  gum_arm_reg_describe (right_reg, &rr);

  if (lr.meta <= GUM_ARM_MREG_R7 &&
      (rr.meta <= GUM_ARM_MREG_R7 || rr.meta == GUM_ARM_MREG_SP) &&
      ((rr.meta == GUM_ARM_MREG_SP && right_offset <= 1020) ||
       (rr.meta != GUM_ARM_MREG_SP && right_offset <= 124)) &&
      (right_offset % 4) == 0)
  {
    guint16 insn;

    if (rr.meta == GUM_ARM_MREG_SP)
      insn = 0x9000 | (lr.index << 8) | (right_offset / 4);
    else
      insn = 0x6000 | (right_offset / 4) << 6 | (rr.index << 3) | lr.index;

    if (operation == GUM_THUMB_MEMORY_LOAD)
      insn |= 0x0800;

    gum_thumb_writer_put_instruction (self, insn);
  }
  else
  {
    if (right_offset > 4095)
      return FALSE;

    gum_thumb_writer_put_instruction (self, 0xf8c0 |
        ((operation == GUM_THUMB_MEMORY_LOAD) ? 0x0010 : 0x0000) |
        rr.index);
    gum_thumb_writer_put_instruction (self, (lr.index << 12) | right_offset);
  }

  return TRUE;
}

void
gum_thumb_writer_put_mov_reg_reg (GumThumbWriter * self,
                                  arm_reg dst_reg,
                                  arm_reg src_reg)
{
  GumArmRegInfo dst, src;
  guint16 insn;

  gum_arm_reg_describe (dst_reg, &dst);
  gum_arm_reg_describe (src_reg, &src);

  if (dst.meta <= GUM_ARM_MREG_R7 && src.meta <= GUM_ARM_MREG_R7)
  {
    insn = 0x1c00 | (src.index << 3) | dst.index;
  }
  else
  {
    guint16 dst_is_high;
    guint dst_index;

    if (dst.meta > GUM_ARM_MREG_R7)
    {
      dst_is_high = 1;
      dst_index = dst.index - GUM_ARM_MREG_R8;
    }
    else
    {
      dst_is_high = 0;
      dst_index = dst.index;
    }

    insn = 0x4600 | (dst_is_high << 7) | (src.index << 3) | dst_index;
  }

  gum_thumb_writer_put_instruction (self, insn);
}

void
gum_thumb_writer_put_mov_reg_u8 (GumThumbWriter * self,
                                 arm_reg dst_reg,
                                 guint8 imm_value)
{
  GumArmRegInfo dst;

  gum_arm_reg_describe (dst_reg, &dst);

  gum_thumb_writer_put_instruction (self, 0x2000 | (dst.index << 8) |
      imm_value);
}

gboolean
gum_thumb_writer_put_add_reg_imm (GumThumbWriter * self,
                                  arm_reg dst_reg,
                                  gssize imm_value)
{
  GumArmRegInfo dst;
  guint16 sign_mask, insn;

  gum_arm_reg_describe (dst_reg, &dst);

  sign_mask = 0x0000;
  if (dst.meta == GUM_ARM_MREG_SP)
  {
    if (imm_value % 4 != 0)
      return FALSE;

    if (imm_value < 0)
      sign_mask = 0x0080;

    insn = 0xb000 | sign_mask | ABS (imm_value / 4);
  }
  else
  {
    if (imm_value < 0)
      sign_mask = 0x0800;

    insn = 0x3000 | sign_mask | (dst.index << 8) | ABS (imm_value);
  }

  gum_thumb_writer_put_instruction (self, insn);

  return TRUE;
}

void
gum_thumb_writer_put_add_reg_reg (GumThumbWriter * self,
                                  arm_reg dst_reg,
                                  arm_reg src_reg)
{
  gum_thumb_writer_put_add_reg_reg_reg (self, dst_reg, dst_reg, src_reg);
}

void
gum_thumb_writer_put_add_reg_reg_reg (GumThumbWriter * self,
                                      arm_reg dst_reg,
                                      arm_reg left_reg,
                                      arm_reg right_reg)
{
  GumArmRegInfo dst, left, right;
  guint16 insn;

  gum_arm_reg_describe (dst_reg, &dst);
  gum_arm_reg_describe (left_reg, &left);
  gum_arm_reg_describe (right_reg, &right);

  if (left.meta == dst.meta)
  {
    insn = 0x4400;

    if (dst.meta <= GUM_ARM_MREG_R7)
      insn |= dst.index;
    else
      insn |= 0x0080 | (dst.index - GUM_ARM_MREG_R8);
    insn |= (right.index << 3);
  }
  else
  {
    insn = 0x1800 | (right.index << 6) | (left.index << 3) | dst.index;
  }

  gum_thumb_writer_put_instruction (self, insn);
}

gboolean
gum_thumb_writer_put_add_reg_reg_imm (GumThumbWriter * self,
                                      arm_reg dst_reg,
                                      arm_reg left_reg,
                                      gssize right_value)
{
  GumArmRegInfo dst, left;
  guint16 insn;

  gum_arm_reg_describe (dst_reg, &dst);
  gum_arm_reg_describe (left_reg, &left);

  if (left.meta == dst.meta)
  {
    return gum_thumb_writer_put_add_reg_imm (self, dst_reg, right_value);
  }

  if (left.meta == GUM_ARM_MREG_SP || left.meta == GUM_ARM_MREG_PC)
  {
    guint16 base_mask;

    if (right_value < 0 || right_value % 4 != 0)
      return FALSE;

    if (left.meta == GUM_ARM_MREG_SP)
      base_mask = 0x0800;
    else
      base_mask = 0x0000;

    insn = 0xa000 | base_mask | (dst.index << 8) | (right_value / 4);
  }
  else
  {
    guint16 sign_mask = 0x0000;

    if (ABS (right_value) > 7)
      return FALSE;

    if (right_value < 0)
      sign_mask = 0x0200;

    insn = 0x1c00 | sign_mask | (ABS (right_value) << 6) | (left.index << 3) |
        dst.index;
  }

  gum_thumb_writer_put_instruction (self, insn);

  return TRUE;
}

gboolean
gum_thumb_writer_put_sub_reg_imm (GumThumbWriter * self,
                                  arm_reg dst_reg,
                                  gssize imm_value)
{
  return gum_thumb_writer_put_add_reg_imm (self, dst_reg, -imm_value);
}

void
gum_thumb_writer_put_sub_reg_reg (GumThumbWriter * self,
                                  arm_reg dst_reg,
                                  arm_reg src_reg)
{
  gum_thumb_writer_put_sub_reg_reg_reg (self, dst_reg, dst_reg, src_reg);
}

void
gum_thumb_writer_put_sub_reg_reg_reg (GumThumbWriter * self,
                                      arm_reg dst_reg,
                                      arm_reg left_reg,
                                      arm_reg right_reg)
{
  GumArmRegInfo dst, left, right;
  guint16 insn;

  gum_arm_reg_describe (dst_reg, &dst);
  gum_arm_reg_describe (left_reg, &left);
  gum_arm_reg_describe (right_reg, &right);

  insn = 0x1a00 | (right.index << 6) | (left.index << 3) | dst.index;

  gum_thumb_writer_put_instruction (self, insn);
}

gboolean
gum_thumb_writer_put_sub_reg_reg_imm (GumThumbWriter * self,
                                      arm_reg dst_reg,
                                      arm_reg left_reg,
                                      gssize right_value)
{
  return gum_thumb_writer_put_add_reg_reg_imm (self, dst_reg, left_reg,
      -right_value);
}

void
gum_thumb_writer_put_nop (GumThumbWriter * self)
{
  gum_thumb_writer_put_instruction (self, 0x46c0);
}

void
gum_thumb_writer_put_bkpt_imm (GumThumbWriter * self,
                               guint8 imm)
{
  gum_thumb_writer_put_instruction (self, 0xbe00 | imm);
}

void
gum_thumb_writer_put_breakpoint (GumThumbWriter * self)
{
  switch (self->target_os)
  {
    case GUM_OS_LINUX:
    case GUM_OS_ANDROID:
      gum_thumb_writer_put_instruction (self, 0xde01);
      break;
    default:
      gum_thumb_writer_put_bkpt_imm (self, 0);
      gum_thumb_writer_put_bx_reg (self, ARM_REG_LR);
      break;
  }
}

void
gum_thumb_writer_put_instruction (GumThumbWriter * self,
                                  guint16 insn)
{
  *self->code++ = GUINT16_TO_LE (insn);
  self->pc += 2;
}

gboolean
gum_thumb_writer_put_bytes (GumThumbWriter * self,
                            const guint8 * data,
                            guint n)
{
  if (n % 2 != 0)
    return FALSE;

  gum_memcpy (self->code, data, n);
  self->code += n / sizeof (guint16);
  self->pc += n;

  return TRUE;
}

static gboolean
gum_instruction_is_t1_load (guint16 instruction)
{
  return (instruction & 0xf800) == 0x4800;
}
