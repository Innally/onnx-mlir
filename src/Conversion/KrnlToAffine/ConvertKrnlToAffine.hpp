/*
 * SPDX-License-Identifier: Apache-2.0
 */

//====------ ConvertKrnlToAffine.hpp - Krnl Dialect Lowering --------------===//
//
// Copyright 2019-2022 The IBM Research Authors.
//
// =============================================================================
//
// This file declares the lowering of Krnl operations to the affine dialect.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "src/Dialect/Krnl/KrnlOps.hpp"
#include "src/Pass/Passes.hpp"
#include "src/Support/Common.hpp"

namespace onnx_mlir {
namespace krnl {

// We use here a Affine builder that generates Krnl Load and Store ops instead
// of the affine memory ops directly. This is because we can still generrate
// Krnl Ops while lowring the dialect, and the big advantage of the Krnl memory
// operations is that they distinguish themselves if they are affine or not.
using AffineBuilderKrnlMem = GenericAffineBuilder<KrnlLoadOp, KrnlStoreOp>;

// To assist unroll and jam
using UnrollAndJamRecord = std::pair<AffineForOp, int64_t>;
using UnrollAndJamList = SmallVector<UnrollAndJamRecord, 4>;
using UnrollAndJamMap = std::map<Operation *, UnrollAndJamList *>;

void populateKrnlToAffineConversion(LLVMTypeConverter &typeConverter,
    RewritePatternSet &patterns, MLIRContext *ctx);

void populateLoweringKrnlCopyFromBufferOpPattern(TypeConverter &typeConverter,
    RewritePatternSet &patterns, MLIRContext *ctx);

void populateLoweringKrnlCopyToBufferOpPattern(TypeConverter &typeConverter,
    RewritePatternSet &patterns, MLIRContext *ctx);

void populateLoweringKrnlLoadOpPattern(TypeConverter &typeConverter,
    RewritePatternSet &patterns, MLIRContext *ctx);

void populateLoweringKrnlStoreOpPattern(TypeConverter &typeConverter,
    RewritePatternSet &patterns, MLIRContext *ctx);

void populateLoweringKrnlMatmultOpPattern(TypeConverter &typeConverter,
    RewritePatternSet &patterns, MLIRContext *ctx);

void populateLoweringKrnlMemsetOpPattern(TypeConverter &typeConverter,
    RewritePatternSet &patterns, MLIRContext *ctx);

void populateLoweringKrnlTerminatorOpPattern(TypeConverter &typeConverter,
    RewritePatternSet &patterns, MLIRContext *ctx);

} // namespace krnl
} // namespace onnx_mlir
