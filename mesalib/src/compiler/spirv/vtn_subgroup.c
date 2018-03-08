/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "vtn_private.h"

static void
vtn_build_subgroup_instr(struct vtn_builder *b,
                         nir_intrinsic_op nir_op,
                         struct vtn_ssa_value *dst,
                         struct vtn_ssa_value *src0,
                         nir_ssa_def *index,
                         unsigned const_idx0,
                         unsigned const_idx1)
{
   /* Some of the subgroup operations take an index.  SPIR-V allows this to be
    * any integer type.  To make things simpler for drivers, we only support
    * 32-bit indices.
    */
   if (index && index->bit_size != 32)
      index = nir_u2u32(&b->nb, index);

   vtn_assert(dst->type == src0->type);
   if (!glsl_type_is_vector_or_scalar(dst->type)) {
      for (unsigned i = 0; i < glsl_get_length(dst->type); i++) {
         vtn_build_subgroup_instr(b, nir_op, dst->elems[i],
                                  src0->elems[i], index,
                                  const_idx0, const_idx1);
      }
      return;
   }

   nir_intrinsic_instr *intrin =
      nir_intrinsic_instr_create(b->nb.shader, nir_op);
   nir_ssa_dest_init_for_type(&intrin->instr, &intrin->dest,
                              dst->type, NULL);
   intrin->num_components = intrin->dest.ssa.num_components;

   intrin->src[0] = nir_src_for_ssa(src0->def);
   if (index)
      intrin->src[1] = nir_src_for_ssa(index);

   intrin->const_index[0] = const_idx0;
   intrin->const_index[1] = const_idx1;

   nir_builder_instr_insert(&b->nb, &intrin->instr);

   dst->def = &intrin->dest.ssa;
}

void
vtn_handle_subgroup(struct vtn_builder *b, SpvOp opcode,
                    const uint32_t *w, unsigned count)
{
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);

   val->ssa = vtn_create_ssa_value(b, val->type->type);

   switch (opcode) {
   case SpvOpGroupNonUniformElect: {
      vtn_fail_if(val->type->type != glsl_bool_type(),
                  "OpGroupNonUniformElect must return a Bool");
      nir_intrinsic_instr *elect =
         nir_intrinsic_instr_create(b->nb.shader, nir_intrinsic_elect);
      nir_ssa_dest_init(&elect->instr, &elect->dest, 1, 32, NULL);
      nir_builder_instr_insert(&b->nb, &elect->instr);
      val->ssa->def = &elect->dest.ssa;
      break;
   }

   case SpvOpGroupNonUniformBallot: {
      vtn_fail_if(val->type->type != glsl_vector_type(GLSL_TYPE_UINT, 4),
                  "OpGroupNonUniformBallot must return a uvec4");
      nir_intrinsic_instr *ballot =
         nir_intrinsic_instr_create(b->nb.shader, nir_intrinsic_ballot);
      ballot->src[0] = nir_src_for_ssa(vtn_ssa_value(b, w[4])->def);
      nir_ssa_dest_init(&ballot->instr, &ballot->dest, 4, 32, NULL);
      ballot->num_components = 4;
      nir_builder_instr_insert(&b->nb, &ballot->instr);
      val->ssa->def = &ballot->dest.ssa;
      break;
   }

   case SpvOpGroupNonUniformInverseBallot: {
      /* This one is just a BallotBitfieldExtract with subgroup invocation.
       * We could add a NIR intrinsic but it's easier to just lower it on the
       * spot.
       */
      nir_intrinsic_instr *intrin =
         nir_intrinsic_instr_create(b->nb.shader,
                                    nir_intrinsic_ballot_bitfield_extract);

      intrin->src[0] = nir_src_for_ssa(vtn_ssa_value(b, w[4])->def);
      intrin->src[1] = nir_src_for_ssa(nir_load_subgroup_invocation(&b->nb));

      nir_ssa_dest_init(&intrin->instr, &intrin->dest, 1, 32, NULL);
      nir_builder_instr_insert(&b->nb, &intrin->instr);

      val->ssa->def = &intrin->dest.ssa;
      break;
   }

   case SpvOpGroupNonUniformBallotBitExtract:
   case SpvOpGroupNonUniformBallotBitCount:
   case SpvOpGroupNonUniformBallotFindLSB:
   case SpvOpGroupNonUniformBallotFindMSB: {
      nir_ssa_def *src0, *src1 = NULL;
      nir_intrinsic_op op;
      switch (opcode) {
      case SpvOpGroupNonUniformBallotBitExtract:
         op = nir_intrinsic_ballot_bitfield_extract;
         src0 = vtn_ssa_value(b, w[4])->def;
         src1 = vtn_ssa_value(b, w[5])->def;
         break;
      case SpvOpGroupNonUniformBallotBitCount:
         switch ((SpvGroupOperation)w[4]) {
         case SpvGroupOperationReduce:
            op = nir_intrinsic_ballot_bit_count_reduce;
            break;
         case SpvGroupOperationInclusiveScan:
            op = nir_intrinsic_ballot_bit_count_inclusive;
            break;
         case SpvGroupOperationExclusiveScan:
            op = nir_intrinsic_ballot_bit_count_exclusive;
            break;
         default:
            unreachable("Invalid group operation");
         }
         src0 = vtn_ssa_value(b, w[5])->def;
         break;
      case SpvOpGroupNonUniformBallotFindLSB:
         op = nir_intrinsic_ballot_find_lsb;
         src0 = vtn_ssa_value(b, w[4])->def;
         break;
      case SpvOpGroupNonUniformBallotFindMSB:
         op = nir_intrinsic_ballot_find_msb;
         src0 = vtn_ssa_value(b, w[4])->def;
         break;
      default:
         unreachable("Unhandled opcode");
      }

      nir_intrinsic_instr *intrin =
         nir_intrinsic_instr_create(b->nb.shader, op);

      intrin->src[0] = nir_src_for_ssa(src0);
      if (src1)
         intrin->src[1] = nir_src_for_ssa(src1);

      nir_ssa_dest_init(&intrin->instr, &intrin->dest, 1, 32, NULL);
      nir_builder_instr_insert(&b->nb, &intrin->instr);

      val->ssa->def = &intrin->dest.ssa;
      break;
   }

   case SpvOpGroupNonUniformBroadcastFirst:
      vtn_build_subgroup_instr(b, nir_intrinsic_read_first_invocation,
                               val->ssa, vtn_ssa_value(b, w[4]), NULL, 0, 0);
      break;

   case SpvOpGroupNonUniformBroadcast:
      vtn_build_subgroup_instr(b, nir_intrinsic_read_invocation,
                               val->ssa, vtn_ssa_value(b, w[4]),
                               vtn_ssa_value(b, w[5])->def, 0, 0);
      break;

   case SpvOpGroupNonUniformAll:
   case SpvOpGroupNonUniformAny:
   case SpvOpGroupNonUniformAllEqual: {
      vtn_fail_if(val->type->type != glsl_bool_type(),
                  "OpGroupNonUniform(All|Any|AllEqual) must return a bool");
      nir_intrinsic_op op;
      switch (opcode) {
      case SpvOpGroupNonUniformAll:
         op = nir_intrinsic_vote_all;
         break;
      case SpvOpGroupNonUniformAny:
         op = nir_intrinsic_vote_any;
         break;
      case SpvOpGroupNonUniformAllEqual: {
         switch (glsl_get_base_type(val->type->type)) {
         case GLSL_TYPE_FLOAT:
         case GLSL_TYPE_DOUBLE:
            op = nir_intrinsic_vote_feq;
            break;
         case GLSL_TYPE_UINT:
         case GLSL_TYPE_INT:
         case GLSL_TYPE_UINT64:
         case GLSL_TYPE_INT64:
         case GLSL_TYPE_BOOL:
            op = nir_intrinsic_vote_ieq;
            break;
         default:
            unreachable("Unhandled type");
         }
         break;
      }
      default:
         unreachable("Unhandled opcode");
      }

      nir_ssa_def *src0 = vtn_ssa_value(b, w[4])->def;

      nir_intrinsic_instr *intrin =
         nir_intrinsic_instr_create(b->nb.shader, op);
      intrin->num_components = src0->num_components;
      intrin->src[0] = nir_src_for_ssa(src0);
      nir_ssa_dest_init(&intrin->instr, &intrin->dest, 1, 32, NULL);
      nir_builder_instr_insert(&b->nb, &intrin->instr);

      val->ssa->def = &intrin->dest.ssa;
      break;
   }

   case SpvOpGroupNonUniformShuffle:
   case SpvOpGroupNonUniformShuffleXor:
   case SpvOpGroupNonUniformShuffleUp:
   case SpvOpGroupNonUniformShuffleDown: {
      nir_intrinsic_op op;
      switch (opcode) {
      case SpvOpGroupNonUniformShuffle:
         op = nir_intrinsic_shuffle;
         break;
      case SpvOpGroupNonUniformShuffleXor:
         op = nir_intrinsic_shuffle_xor;
         break;
      case SpvOpGroupNonUniformShuffleUp:
         op = nir_intrinsic_shuffle_up;
         break;
      case SpvOpGroupNonUniformShuffleDown:
         op = nir_intrinsic_shuffle_down;
         break;
      default:
         unreachable("Invalid opcode");
      }
      vtn_build_subgroup_instr(b, op, val->ssa, vtn_ssa_value(b, w[4]),
                               vtn_ssa_value(b, w[5])->def, 0, 0);
      break;
   }

   case SpvOpGroupNonUniformQuadBroadcast:
      vtn_build_subgroup_instr(b, nir_intrinsic_quad_broadcast,
                               val->ssa, vtn_ssa_value(b, w[4]),
                               vtn_ssa_value(b, w[5])->def, 0, 0);
      break;

   case SpvOpGroupNonUniformQuadSwap: {
      unsigned direction = vtn_constant_value(b, w[5])->values[0].u32[0];
      nir_intrinsic_op op;
      switch (direction) {
      case 0:
         op = nir_intrinsic_quad_swap_horizontal;
         break;
      case 1:
         op = nir_intrinsic_quad_swap_vertical;
         break;
      case 2:
         op = nir_intrinsic_quad_swap_diagonal;
         break;
      }
      vtn_build_subgroup_instr(b, op, val->ssa, vtn_ssa_value(b, w[4]),
                               NULL, 0, 0);
      break;
   }

   case SpvOpGroupNonUniformIAdd:
   case SpvOpGroupNonUniformFAdd:
   case SpvOpGroupNonUniformIMul:
   case SpvOpGroupNonUniformFMul:
   case SpvOpGroupNonUniformSMin:
   case SpvOpGroupNonUniformUMin:
   case SpvOpGroupNonUniformFMin:
   case SpvOpGroupNonUniformSMax:
   case SpvOpGroupNonUniformUMax:
   case SpvOpGroupNonUniformFMax:
   case SpvOpGroupNonUniformBitwiseAnd:
   case SpvOpGroupNonUniformBitwiseOr:
   case SpvOpGroupNonUniformBitwiseXor:
   case SpvOpGroupNonUniformLogicalAnd:
   case SpvOpGroupNonUniformLogicalOr:
   case SpvOpGroupNonUniformLogicalXor: {
      nir_op reduction_op;
      switch (opcode) {
      case SpvOpGroupNonUniformIAdd:
         reduction_op = nir_op_iadd;
         break;
      case SpvOpGroupNonUniformFAdd:
         reduction_op = nir_op_fadd;
         break;
      case SpvOpGroupNonUniformIMul:
         reduction_op = nir_op_imul;
         break;
      case SpvOpGroupNonUniformFMul:
         reduction_op = nir_op_fmul;
         break;
      case SpvOpGroupNonUniformSMin:
         reduction_op = nir_op_imin;
         break;
      case SpvOpGroupNonUniformUMin:
         reduction_op = nir_op_umin;
         break;
      case SpvOpGroupNonUniformFMin:
         reduction_op = nir_op_fmin;
         break;
      case SpvOpGroupNonUniformSMax:
         reduction_op = nir_op_imax;
         break;
      case SpvOpGroupNonUniformUMax:
         reduction_op = nir_op_umax;
         break;
      case SpvOpGroupNonUniformFMax:
         reduction_op = nir_op_fmax;
         break;
      case SpvOpGroupNonUniformBitwiseAnd:
      case SpvOpGroupNonUniformLogicalAnd:
         reduction_op = nir_op_iand;
         break;
      case SpvOpGroupNonUniformBitwiseOr:
      case SpvOpGroupNonUniformLogicalOr:
         reduction_op = nir_op_ior;
         break;
      case SpvOpGroupNonUniformBitwiseXor:
      case SpvOpGroupNonUniformLogicalXor:
         reduction_op = nir_op_ixor;
         break;
      default:
         unreachable("Invalid reduction operation");
      }

      nir_intrinsic_op op;
      unsigned cluster_size = 0;
      switch ((SpvGroupOperation)w[4]) {
      case SpvGroupOperationReduce:
         op = nir_intrinsic_reduce;
         break;
      case SpvGroupOperationInclusiveScan:
         op = nir_intrinsic_inclusive_scan;
         break;
      case SpvGroupOperationExclusiveScan:
         op = nir_intrinsic_exclusive_scan;
         break;
      case SpvGroupOperationClusteredReduce:
         op = nir_intrinsic_reduce;
         assert(count == 7);
         cluster_size = vtn_constant_value(b, w[6])->values[0].u32[0];
         break;
      default:
         unreachable("Invalid group operation");
      }

      vtn_build_subgroup_instr(b, op, val->ssa, vtn_ssa_value(b, w[5]),
                               NULL, reduction_op, cluster_size);
      break;
   }

   default:
      unreachable("Invalid SPIR-V opcode");
   }
}
