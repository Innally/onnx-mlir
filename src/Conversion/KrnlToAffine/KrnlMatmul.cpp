/*
 * SPDX-License-Identifier: Apache-2.0
 */

//===-------------- KrnlMatmul.cpp - Lower KrnlMatmulOp -------------------===//
//
// Copyright 2019-2020 The IBM Research Authors.
//
// =============================================================================
//
// This file lowers the KrnlMatmulOp operator.
//
//===----------------------------------------------------------------------===//

#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinTypes.h"

#include "src/Conversion/KrnlToAffine/ConvertKrnlToAffine.hpp"
#include "src/Conversion/KrnlToAffine/KrnlToAffineHelper.hpp"
#include "src/Conversion/KrnlToLLVM/RuntimeAPI.hpp"
#include "src/Dialect/Krnl/KrnlOps.hpp"
#include "src/Support/KrnlSupport.hpp"
#include "llvm/Support/Debug.h"

#include <mutex>

#define DEBUG_TYPE "krnl_to_affine"

using namespace mlir;
using namespace onnx_mlir;

namespace onnx_mlir {
namespace krnl {

static constexpr int BUFFER_ALIGN = 64;
extern UnrollAndJamMap unrollAndJamMap;
extern std::mutex unrollAndJamMutex;

// Affine expressions compared to >= 0
static IndexExpr isFullTile(IndexExpr UB, IndexExpr block, IndexExpr GI) {
  // Determine if the current tile is full. It is full if the begining of
  // the tile (nGI) is smaller or equal to UB - bloc, namely
  //   PredicateIndexExpr nIsFullTile = (nGI <= (nUB - nBlock));
  // However, if UB is divisible by Block, then its full no matter what.
  if (UB.isLiteral() && (UB.getLiteral() % block.getLiteral() == 0)) {
    // Last tile is guaranteed to be full because UB is divisable by block.
    return LiteralIndexExpr(1); // 1 >= 0 is true
  }
  // true if GI <= (UB - block), namely UB - block - GI >= 0
  IndexExpr res = UB - block - GI;
  return res;
}

static IndexExpr partialTrip(IndexExpr UB, IndexExpr block, IndexExpr GI) {
  // Trip count for partial tiles: leftover = UB - GI in general. If UB is
  // known at compile time, then without loss of generality, leftover = (UB-
  // GI) % Block, and since GI is by definition a multiple of Block (GI is
  // index at begining of tile), then leftover = UB % Block.
  //   IndexExpr nPartialTrip = nUB.isLiteral() ? nUB % nBlock : nUB - nGI;
  if (UB.isLiteral()) {
    IndexExpr partialTrip = UB % block;
    assert(partialTrip.isLiteral() && "op on 2 literals has to be literal");
    return partialTrip;
  }
  // don't have to take the mod since we know we have a partial tile already.
  return UB - GI;
}

// KrnlMatmul will be lowered to vector and affine expressions
class KrnlMatmulLowering : public ConversionPattern {
public:
  explicit KrnlMatmulLowering(
      TypeConverter &typeConverter, MLIRContext *context)
      : ConversionPattern(
            typeConverter, KrnlMatMulOp::getOperationName(), 1, context) {}

  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const override {
    auto matmulOp = cast<KrnlMatMulOp>(op);
    KrnlMatMulOpAdaptor operandAdaptor(matmulOp);
    // Option.
    bool fullUnrollAndJam = matmulOp.unroll();

    // Operands and types.
    Type elementType =
        operandAdaptor.A().getType().cast<MemRefType>().getElementType();
    bool simdize = matmulOp.simdize();
    // Init scope and emit constants.
    Location loc = matmulOp.getLoc();
    AffineBuilderKrnlMem createAffine(rewriter, loc);
    IndexExprScope indexScope(createAffine);

    // Gather A, B, C tile sizes.
    SmallVector<IndexExpr, 2> aTileSize, bTileSize, cTileSize;
    Value A(operandAdaptor.A()), B(operandAdaptor.B()), C(operandAdaptor.C());
    MemRefBoundsIndexCapture aBounds(A), bBounds(B), cBounds(C);
    int64_t aRank(aBounds.getRank()), bRank(bBounds.getRank()),
        cRank(cBounds.getRank());
    // Tile sizes for A/B/C are determined by their memref unless explicitly
    // specified by an optional argument. That allows A/B/C memrefs to be
    // padded if needed for SIMD/unroll and jam, for example.
    ArrayAttributeIndexCapture aSizeCapture(matmulOp.aTileSizeAttr());
    if (aSizeCapture.size())
      aTileSize = {aSizeCapture.getLiteral(0), aSizeCapture.getLiteral(1)};
    else
      aTileSize = {aBounds.getSymbol(aRank - 2), aBounds.getSymbol(aRank - 1)};
    ArrayAttributeIndexCapture bSizeCapture(matmulOp.bTileSizeAttr());
    if (bSizeCapture.size())
      bTileSize = {bSizeCapture.getLiteral(0), bSizeCapture.getLiteral(1)};
    else
      bTileSize = {bBounds.getSymbol(bRank - 2), bBounds.getSymbol(bRank - 1)};
    ArrayAttributeIndexCapture cSizeCapture(matmulOp.cTileSizeAttr());
    if (cSizeCapture.size())
      cTileSize = {cSizeCapture.getLiteral(0), cSizeCapture.getLiteral(1)};
    else
      cTileSize = {cBounds.getSymbol(cRank - 2), cBounds.getSymbol(cRank - 1)};

    // Gather N, M, K compute tile size. This is the size of the computations,
    // if the tile is full. Because computation in the buffers could be further
    // subtiled, the default size can be overridden from the tile sizes using
    // the computeTileSize attribute. Tiles may not be full if they are at the
    // outer boundaries of the original data.
    IndexExpr iComputeTileSize = cTileSize[0];
    IndexExpr jComputeTileSize = cTileSize[1];
    IndexExpr kComputeTileSize = aTileSize[1];
    ArrayAttributeIndexCapture computeSizeCapture(
        matmulOp.computeTileSizeAttr());
    if (computeSizeCapture.size()) {
      iComputeTileSize = computeSizeCapture.getLiteral(0);
      jComputeTileSize = computeSizeCapture.getLiteral(1);
      kComputeTileSize = computeSizeCapture.getLiteral(2);
    }

    // If we simdize, its along M for the full compute tile.
    IndexExpr vectorLen = jComputeTileSize;
    if (!vectorLen.isLiteral()) {
      // Cannot simdize if the vector length is not a compile time constant.
      simdize = false;
      LLVM_DEBUG(llvm::dbgs() << "Matmul: No simd due to vl not a literal\n");
    }
    if (!simdize)
      vectorLen = LiteralIndexExpr(1);

    // Now get global start indices, which would define the first element of the
    // tiles in the original computations.
    DimIndexExpr iComputeStart(operandAdaptor.iComputeStart()),
        jComputeStart(operandAdaptor.jComputeStart()),
        kComputeStart(operandAdaptor.kComputeStart());
    // And get the global upper bound of the original computations.
    SymbolIndexExpr iGlobalUB(operandAdaptor.iGlobalUB()),
        jGlobalUB(operandAdaptor.jGlobalUB()),
        kGlobalUB(operandAdaptor.kGlobalUB());
    // A[i, k];
    SmallVector<IndexExpr, 4> aStart, bStart, cStart;
    for (int t = 0; t < aRank - 2; t++)
      aStart.emplace_back(SymbolIndexExpr(operandAdaptor.aMemStart()[t]));
    aStart.emplace_back(
        iComputeStart - DimIndexExpr(operandAdaptor.aMemStart()[aRank - 2]));
    aStart.emplace_back(
        kComputeStart - DimIndexExpr(operandAdaptor.aMemStart()[aRank - 1]));
    // B[k, j];
    for (int t = 0; t < bRank - 2; t++)
      bStart.emplace_back(SymbolIndexExpr(operandAdaptor.bMemStart()[t]));
    bStart.emplace_back(
        kComputeStart - DimIndexExpr(operandAdaptor.bMemStart()[bRank - 2]));
    bStart.emplace_back(
        jComputeStart - DimIndexExpr(operandAdaptor.bMemStart()[bRank - 1]));
    // C[i, j]
    for (int t = 0; t < cRank - 2; t++)
      cStart.emplace_back(SymbolIndexExpr(operandAdaptor.cMemStart()[t]));
    cStart.emplace_back(
        iComputeStart - DimIndexExpr(operandAdaptor.cMemStart()[cRank - 2]));
    cStart.emplace_back(
        jComputeStart - DimIndexExpr(operandAdaptor.cMemStart()[cRank - 1]));

    // Now determine if we have full/partial tiles. This is determined by the
    // outer dimensions of the original computations, as by definition tiling
    // within the buffer always results in full tiles. In other words, partial
    // tiles only occurs because of "runing out" of the original data.
    IndexExpr iIsFullTile =
        isFullTile(iGlobalUB, iComputeTileSize, iComputeStart);
    IndexExpr jIsFullTile =
        isFullTile(jGlobalUB, jComputeTileSize, jComputeStart);
    IndexExpr kIsFullTile =
        isFullTile(kGlobalUB, kComputeTileSize, kComputeStart);
    SmallVector<IndexExpr, 3> allFullTiles = {
        iIsFullTile, jIsFullTile, kIsFullTile};

    SmallVector<IndexExpr, 1> jFullTiles = {jIsFullTile};
    // And if the tiles are not full, determine how many elements to compute.
    // With overcompute, this could be relaxed.
    IndexExpr iTrip = trip(
        iGlobalUB, iComputeTileSize, iComputeStart); // May or may not be full.
    IndexExpr jTrip = trip(
        jGlobalUB, jComputeTileSize, jComputeStart); // May or may not be full.
    IndexExpr kTrip = trip(
        kGlobalUB, kComputeTileSize, kComputeStart); // May or may not be full.
    IndexExpr jPartialTrip =
        partialTrip(jGlobalUB, jComputeTileSize, jComputeStart);

    if (simdize) {
      // SIMD code generator.
      // clang-format off
      createAffine.ifThenElse(indexScope, allFullTiles,
        /* then full tiles */ [&](AffineBuilderKrnlMem &createAffine) {
        genSimd(rewriter, loc, matmulOp, elementType, aStart, bStart, cStart,
          iComputeTileSize, jComputeTileSize, kComputeTileSize,
          vectorLen, fullUnrollAndJam); 
      }, /* has some partial tiles */ [&](AffineBuilderKrnlMem &createAffine) {
        // Trip regardless of full/partial for N & K
        // Test if SIMD dim (M) is full.
        createAffine.ifThenElse(indexScope, jFullTiles,
          /* full SIMD */ [&](AffineBuilderKrnlMem &createAffine) {
          genSimd(rewriter, loc, matmulOp, elementType, aStart, bStart, cStart,
            iTrip, jComputeTileSize, kTrip, vectorLen, /*unroll*/ false);
        }, /* else partial SIMD */ [&](AffineBuilderKrnlMem &createAffine) {
          // TODO: evaluate if get performance from partial SIMD
          if (false && jPartialTrip.isLiteral() && jPartialTrip.getLiteral() >=2) {
            // has a known trip count along the simd dimension of at least 2
            // elements, use simd again.
            genSimd(rewriter, loc, matmulOp, elementType, aStart, bStart, cStart,
              iTrip, jPartialTrip, kTrip, vectorLen, /*unroll*/ false);
          } else {
            genScalar(rewriter, matmulOp, elementType, aStart, bStart,  cStart,
              iTrip, jPartialTrip, kTrip, /*unroll*/ false);
          }
        });
      });
      // clang-format on
    } else {
      // Scalar code generator.
      // clang-format off
      createAffine.ifThenElse(indexScope, allFullTiles,
        /* then full */ [&](AffineBuilderKrnlMem &createAffine) {
        genScalar(rewriter, matmulOp, elementType, aStart, bStart, cStart,
          iComputeTileSize, jComputeTileSize, kComputeTileSize,
          fullUnrollAndJam); 
      }, /* else partial */ [&](AffineBuilderKrnlMem &createAffine) {
        genScalar(rewriter, matmulOp, elementType, aStart, bStart, cStart,
          iTrip, jTrip, kTrip, false);
      });
      // clang-format on
    }
    rewriter.eraseOp(op);
    return success();
  }

private:
  void genScalar(PatternRewriter &rewriter, KrnlMatMulOp op, Type elementType,
      ArrayRef<IndexExpr> aStart, ArrayRef<IndexExpr> bStart,
      ArrayRef<IndexExpr> cStart, IndexExpr I, IndexExpr J, IndexExpr K,
      bool unrollJam) const {
    // Get operands.
    KrnlMatMulOpAdaptor operandAdaptor(op);
    Location loc = op.getLoc();
    AffineBuilderKrnlMem createAffine(rewriter, loc);
    MemRefBuilder createMemRef(createAffine);

    Value A(operandAdaptor.A()), B(operandAdaptor.B()), C(operandAdaptor.C());
    int64_t aRank(aStart.size()), bRank(bStart.size()), cRank(cStart.size());
    int64_t unrollFactor = (unrollJam && J.isLiteral()) ? J.getLiteral() : 1;
    // Have to privatize CTmpType by unroll factor (1 if none).
    MemRefType CTmpType = MemRefType::get({unrollFactor}, elementType);
    assert(BUFFER_ALIGN >= gDefaultAllocAlign);
    Value TmpC = createMemRef.alignedAlloc(CTmpType, BUFFER_ALIGN);

    // For i, j loops.
    LiteralIndexExpr zero(0);
    Value jSaved;
    createAffine.forIE(
        zero, I, 1, [&](AffineBuilderKrnlMem &createAffine, Value i) {
          createAffine.forIE(
              zero, J, 1, [&](AffineBuilderKrnlMem &createAffine, Value j) {
                MathBuilder createMath(createAffine);
                // Defines induction variables, and possibly initialize C.
                jSaved = j;
                // Alloc and init temp c storage.
                SmallVector<Value, 4> cAccess;
                // CC(i + cStart0.getValue(), j + cStart1.getValue());
                IndexExpr::getValues(cStart, cAccess);
                cAccess[cRank - 2] = createMath.add(i, cAccess[cRank - 2]);
                cAccess[cRank - 1] = createMath.add(j, cAccess[cRank - 1]);
                Value initVal = createAffine.load(C, cAccess);
                Value tmpCAccess = (unrollFactor > 1) ? j : zero.getValue();
                createAffine.store(initVal, TmpC, tmpCAccess);
                // TTmpC() = affine_load(C, cAccess);
                // Sum over k.
                createAffine.forIE(zero, K, 1,
                    [&](AffineBuilderKrnlMem &createAffine, Value k) {
                      MathBuilder createMath(createAffine);
                      SmallVector<Value, 4> aAccess, bAccess;
                      // AA(i + aStart0.getValue(), k + aStart1.getValue())
                      IndexExpr::getValues(aStart, aAccess);
                      aAccess[aRank - 2] =
                          createMath.add(i, aAccess[aRank - 2]);
                      aAccess[aRank - 1] =
                          createMath.add(k, aAccess[aRank - 1]);
                      Value a = createAffine.load(A, aAccess);
                      // BB(k + bStart0.getValue(), j + bStart1.getValue())
                      IndexExpr::getValues(bStart, bAccess);
                      bAccess[bRank - 2] =
                          createMath.add(k, bAccess[bRank - 2]);
                      bAccess[bRank - 1] =
                          createMath.add(j, bAccess[bRank - 1]);
                      Value b = createAffine.load(B, bAccess);
                      Value res = createMath.mul(a, b);
                      res = createMath.add(
                          res, createAffine.load(TmpC, tmpCAccess));
                      createAffine.store(res, TmpC, tmpCAccess);
                      // TTmpC() = a * b + TTmpC();
                    });
                // Store temp result into C(i, j)
                Value finalVal = createAffine.load(TmpC, tmpCAccess);
                createAffine.store(finalVal, C, cAccess);
                // affine_store(TTmpC(), C, cAccess);
              });
        });
    if (unrollJam && J.isLiteral()) {
      UnrollAndJamRecord record(
          getForInductionVarOwner(jSaved), J.getLiteral());
      getUnrollAndJamList(op)->emplace_back(record);
    }
  }

  void genSimd(PatternRewriter &rewriter, Location loc, KrnlMatMulOp op,
      Type elementType, ArrayRef<IndexExpr> aStart, ArrayRef<IndexExpr> bStart,
      ArrayRef<IndexExpr> cStart, IndexExpr I, IndexExpr J, IndexExpr K,
      IndexExpr vectorLen, bool unrollJam) const {
    // can simdize only if K is compile time
    assert(J.isLiteral() &&
           "can only simdize with compile time blocking factor on simd axis");
    AffineBuilderKrnlMem createAffine(rewriter, loc);
    MemRefBuilder createMemRef(rewriter, loc);
    // Get operands.
    KrnlMatMulOpAdaptor operandAdaptor = KrnlMatMulOpAdaptor(op);
    Value A(operandAdaptor.A()), B(operandAdaptor.B()), C(operandAdaptor.C());
    int64_t aRank(aStart.size()), bRank(bStart.size()), cRank(cStart.size());

    // Generate the vector type conversions.
    int64_t VL = vectorLen.getLiteral();
    VectorType vecType = VectorType::get({VL}, elementType);
    int64_t unrollFactor = (unrollJam && I.isLiteral()) ? I.getLiteral() : 1;
    // Have to privatize CTmpType by unroll factor (1 if none).
    MemRefType CTmpType = MemRefType::get({unrollFactor}, vecType);
    assert(BUFFER_ALIGN >= gDefaultAllocAlign);
    Value TmpC = createMemRef.alignedAlloca(CTmpType, BUFFER_ALIGN);

    // Iterates over the I indices (j are simd dim).
    Value iSaved, kSaved;
    LiteralIndexExpr zero(0);
    createAffine.forIE(
        zero, I, 1, [&](AffineBuilderKrnlMem &createAffine, Value i) {
          MultiDialectBuilder<MathBuilder, VectorBuilder> create(createAffine);
          iSaved = i; // Saved for unroll and jam.
          // Alloca temp vector TmpC and save C(i)/0.0 into it.
          SmallVector<Value, 4> cAccess;
          // cAccess = {i + cStart0.getValue(), cStart1.getValue()};
          IndexExpr::getValues(cStart, cAccess);
          cAccess[cRank - 2] = create.math.add(i, cAccess[cRank - 2]);
          Value initVal = create.vec.load(vecType, C, cAccess);
          Value tmpCAccess = (unrollFactor > 1) ? i : zero.getValue();
          createAffine.store(initVal, TmpC, tmpCAccess);
          // Sum over k.
          createAffine.forIE(
              zero, K, 1, [&](AffineBuilderKrnlMem &createAffine, Value k) {
                MultiDialectBuilder<MathBuilder, VectorBuilder> create(
                    createAffine);
                kSaved = k;
                // Value a = AA(i + aStart0.getValue(), k + aStart1.getValue());
                SmallVector<Value, 4> aAccess, bAccess;
                IndexExpr::getValues(aStart, aAccess);
                aAccess[aRank - 2] = create.math.add(i, aAccess[aRank - 2]);
                aAccess[aRank - 1] = create.math.add(k, aAccess[aRank - 1]);
                Value a = createAffine.load(A, aAccess);
                // Value va = vector_broadcast(vecType, a);
                Value va = create.vec.broadcast(vecType, a);
                // bAccess = {k + bStart0.getValue(), bStart1.getValue()};
                IndexExpr::getValues(bStart, bAccess);
                bAccess[bRank - 2] = create.math.add(k, bAccess[bRank - 2]);
                Value vb = create.vec.load(vecType, B, bAccess);
                // TTmpC() = vector_fma(va, vb, TTmpC());
                Value tmpVal = createAffine.load(TmpC, tmpCAccess);
                Value res = create.vec.fma(va, vb, tmpVal);
                createAffine.store(res, TmpC, tmpCAccess);
              });
          // Store temp result into C(i)
          Value tmpResults = createAffine.load(TmpC, tmpCAccess);
          int64_t JLit = J.getLiteral();
          if (JLit != VL) {
            // create vector constant
            SmallVector<int64_t, 8> mask;
            for (int64_t i = 0; i < VL; i++)
              mask.emplace_back((i < JLit) ? i : VL + i);
            // permute
            Value originalCvec = create.vec.load(vecType, C, cAccess);
            tmpResults = create.vec.shuffle(tmpResults, originalCvec, mask);
          }
          // CCvec(i + CStart0.getValue(), CStart1.getValue()) = tmpResults;
          create.vec.store(tmpResults, C, cAccess);
        });

    if (unrollJam && (I.isLiteral() || K.isLiteral())) {
      auto list = getUnrollAndJamList(op);
      if (K.isLiteral()) {
        int64_t kUnroll = K.getLiteral();
        // We know there is no unrolling along I, make a bigger cutoff.
        int64_t cutoff = (!I.isLiteral() || I.getLiteral() < 2) ? 8 : 4;
        if (kUnroll >= cutoff) {
          // When kUnroll is too big, reduce it by a divisor.
          for (int64_t m = cutoff; m >= 1; --m) {
            if (kUnroll % m == 0) {
              kUnroll = m;
              break;
            }
          }
        }
        if (kUnroll > 1) {
          LLVM_DEBUG(
              llvm::dbgs() << "Matmul: unroll k by " << kUnroll << "\n";);
          UnrollAndJamRecord record(getForInductionVarOwner(kSaved), kUnroll);
          list->emplace_back(record);
        }
      }
      if (I.isLiteral() && I.getLiteral() > 1) {
        LLVM_DEBUG(llvm::dbgs()
                   << "Matmul: unroll i by " << (int)I.getLiteral() << "\n");
        UnrollAndJamRecord record(
            getForInductionVarOwner(iSaved), I.getLiteral());
        list->emplace_back(record);
      }
    }
  }

  UnrollAndJamList *getUnrollAndJamList(Operation *op) const {
    Operation *currFuncOp = getContainingFunction(op);
    assert(currFuncOp && "function expected");
    const std::lock_guard<std::mutex> lock(unrollAndJamMutex);
    UnrollAndJamList *currUnrollAndJamList = unrollAndJamMap[currFuncOp];
    assert(currUnrollAndJamList && "expected list for function");
    return currUnrollAndJamList;
  }
};

void populateLoweringKrnlMatmultOpPattern(TypeConverter &typeConverter,
    RewritePatternSet &patterns, MLIRContext *ctx) {
  patterns.insert<KrnlMatmulLowering>(typeConverter, ctx);
}

} // namespace krnl
} // namespace onnx_mlir
