/*
 * SPDX-License-Identifier: Apache-2.0
 */

//===------ KrnlToAffineHelper.hpp ----------------------------------------===//
//
// Copyright 2022 The IBM Research Authors.
//
// =============================================================================
//
// Declare utility functions for the Krnl to Affine dialect conversion.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "src/Dialect/ONNX/IndexExpr.hpp"

namespace onnx_mlir {
namespace krnl {

/// Compute the normalized trip count of a loop as:
///   trip = min(UB - GI, block);
mlir::IndexExpr trip(
    mlir::IndexExpr UB, mlir::IndexExpr block, mlir::IndexExpr GI);

} // namespace krnl
} // namespace onnx_mlir
