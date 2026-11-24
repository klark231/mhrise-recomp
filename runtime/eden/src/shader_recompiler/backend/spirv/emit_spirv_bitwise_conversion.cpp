// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

void EmitBitCastU16F16(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBitCastU32F32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.U32[1], value);
}

void EmitBitCastU64F64(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

void EmitBitCastF16U16(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBitCastF32U32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.F32[1], value);
}

void EmitBitCastF64U64(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitPackUint2x32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.U64, value);
}

Id EmitUnpackUint2x32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.U32[2], value);
}

Id EmitPackFloat2x16(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.U32[1], value);
}

Id EmitUnpackFloat2x16(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.F16[2], value);
}

namespace {
// A float's half bits, rounded to nearest even, with integer operations (and one float add for the half denormals):
// infinity and NaN stay so, what's too big for a half becomes infinity.
Id HalfBits(EmitContext& ctx, Id value) {
    const Id u32{ctx.U32[1]};
    const Id bits{ctx.OpBitcast(u32, value)};
    const Id sign{ctx.OpBitwiseAnd(u32, bits, ctx.Const(0x80000000U))};
    const Id abs{ctx.OpBitwiseXor(u32, bits, sign)};
    const Id nan{ctx.OpUGreaterThan(ctx.U1, abs, ctx.Const(0x7f800000U))};
    const Id big{ctx.OpSelect(u32, nan, ctx.Const(0x7e00U), ctx.Const(0x7c00U))};
    // Below the smallest normal half: the add leaves the denormal's bits at the bottom of the sum's mantissa
    const Id magic{ctx.Const(0x3f000000U)};
    const Id sum{ctx.OpFAdd(ctx.F32[1], ctx.OpBitcast(ctx.F32[1], abs), ctx.OpBitcast(ctx.F32[1], magic))};
    const Id denormal{ctx.OpISub(u32, ctx.OpBitcast(u32, sum), magic)};
    // Normal: the exponent rebiased, the mantissa rounded (ties to the even one)
    const Id odd{ctx.OpBitwiseAnd(u32, ctx.OpShiftRightLogical(u32, abs, ctx.Const(13U)), ctx.Const(1U))};
    const Id rounded{ctx.OpIAdd(u32, ctx.OpIAdd(u32, abs, ctx.Const(0xc8000fffU)), odd)};
    const Id normal{ctx.OpShiftRightLogical(u32, rounded, ctx.Const(13U))};
    Id half{ctx.OpSelect(u32, ctx.OpULessThan(ctx.U1, abs, ctx.Const(0x38800000U)), denormal, normal)};
    half = ctx.OpSelect(u32, ctx.OpUGreaterThanEqual(ctx.U1, abs, ctx.Const(0x47800000U)), big, half);
    return ctx.OpBitwiseOr(u32, half, ctx.OpShiftRightLogical(u32, sign, ctx.Const(16U)));
}
} // Anonymous namespace

Id EmitPackHalf2x16(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_pack_half && ctx.stage != Stage::Fragment && ctx.stage != Stage::Compute) {
        const Id low{HalfBits(ctx, ctx.OpCompositeExtract(ctx.F32[1], value, 0U))};
        const Id high{HalfBits(ctx, ctx.OpCompositeExtract(ctx.F32[1], value, 1U))};
        return ctx.OpBitwiseOr(ctx.U32[1], low, ctx.OpShiftLeftLogical(ctx.U32[1], high, ctx.Const(16U)));
    }
    return ctx.OpPackHalf2x16(ctx.U32[1], value);
}

Id EmitUnpackHalf2x16(EmitContext& ctx, Id value) {
    return ctx.OpUnpackHalf2x16(ctx.F32[2], value);
}

Id EmitPackDouble2x32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.F64[1], value);
}

Id EmitUnpackDouble2x32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.U32[2], value);
}

} // namespace Shader::Backend::SPIRV
