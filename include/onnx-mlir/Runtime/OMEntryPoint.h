/*
 * SPDX-License-Identifier: Apache-2.0
 */

//===----------- OMEntryPoint.h - OMEntryPoint Declaration header ---------===//
//
// Copyright 2022 The IBM Research Authors.
//
// =============================================================================
//
// This file contains declaration of OMEntryPoint API functions.
//
//===----------------------------------------------------------------------===//

#ifdef __cplusplus
#pragma once

extern "C" {
#endif

/**
 * \brief Return all entry point names in a model. These entry point names are
 * the symbols of the inference functions in the model. Users use them to run
 * inference, e.g. by calling `entry_point_name(OMTensorList).
 *
 * An entry point name can be passed to functions `omInputSignature` and
 * `omOutputSignature` to query its input and output signatures, respectively.
 *
 * @return an array of strings. The array ends with NULL. For example:
 * ["run_add", "run_sub", NULL].
 */
const char *const *omQueryEntryPoints();

#ifdef __cplusplus
}
#endif
