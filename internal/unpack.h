// Copyright 2015 The Gemmlowp Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// unpack.h: unpacking the result blocks computed by compute.h,
// storing them into the destination matrix.

#ifndef GEMMLOWP_INTERNAL_UNPACK_H_
#define GEMMLOWP_INTERNAL_UNPACK_H_

#include "allocator.h"
#include "block_params.h"
#include "output.h"
#include "pack.h"

#include <cmath>

namespace gemmlowp {

class PackedResult {
 public:
  PackedResult(Allocator* _allocator, const BlockParams& _block_params)
      : allocator_(_allocator), block_params_(_block_params) {
    matrix_handle_ = allocator_->Reserve<std::int32_t>(block_params_.l2_rows *
                                                       block_params_.l2_cols);
  }

  ~PackedResult() {}

  MatrixMap<std::int32_t, MapOrder::ColMajor> Map() {
    return MatrixMap<std::int32_t, MapOrder::ColMajor>(
        allocator_->GetPointer<std::int32_t>(matrix_handle_),
        block_params_.l2_rows, block_params_.l2_cols, block_params_.l2_rows);
  }

  MatrixMap<const std::int32_t, MapOrder::ColMajor> Map() const {
    return MatrixMap<const std::int32_t, MapOrder::ColMajor>(
        allocator_->GetPointer<const std::int32_t>(matrix_handle_),
        block_params_.l2_rows, block_params_.l2_cols, block_params_.l2_rows);
  }

 private:
  Allocator* allocator_;
  Allocator::Handle matrix_handle_;
  const BlockParams& block_params_;
};

struct MatrixBlockBounds {
  int start_row;
  int start_col;
  int rows;
  int cols;

  MatrixBlockBounds(int start_row_, int start_col_, int rows_, int cols_)
      : start_row(start_row_),
        start_col(start_col_),
        rows(rows_),
        cols(cols_) {}
};

template <typename RegisterBlockType, typename SrcMapType,
    typename LhsOffset, typename RhsOffset,
    typename OutputPipelineExecutorType, typename DstType>
void UnpackResultBlock(const SrcMapType& src, const OutputPipelineExecutorType& executor, DstType* dst,
    const VectorMap<const std::int32_t, VectorShape::Col>& lhs_sums_of_each_slice,
    const VectorMap<const std::int32_t, VectorShape::Row>& rhs_sums_of_each_slice,
    const LhsOffset& lhs_offset, const RhsOffset& rhs_offset,
    int depth,
    int src_row, int src_col, int dst_row, int dst_col) {

      auto acc =
        Load<RegisterBlockType>(src, src_row, src_col);
      const auto& lhs_sums_of_each_slice_block =
        LoadForBroadcasting<RegisterBlockType>(lhs_sums_of_each_slice, src_row);
      const auto& rhs_sums_of_each_slice_block =
        LoadForBroadcasting<RegisterBlockType>(rhs_sums_of_each_slice, src_col);
      const auto& lhs_offset_block =
        LoadForBroadcasting<RegisterBlockType>(lhs_offset, dst_row);
      const auto& rhs_offset_block =
        LoadForBroadcasting<RegisterBlockType>(rhs_offset, dst_col);
      BroadcastMulAdd(
            lhs_sums_of_each_slice_block,
            rhs_offset_block,
            &acc
          );
      BroadcastMulAdd(
            rhs_sums_of_each_slice_block,
            lhs_offset_block,
            &acc
          );
      BroadcastMulAdd(
            rhs_offset_block,
            lhs_offset_block,
            ConstantMultiplierInt32(depth),
            &acc
          );
      executor.Execute(acc, dst, dst_row, dst_col);


}

template <typename ResultBlockType, typename PackedResultType,
          typename LhsOffset, typename RhsOffset, typename OutputPipelineType>
struct UnpackResultImpl {
};

template <typename ResultScalarType, typename PackedResultType,
          typename LhsOffset, typename RhsOffset, typename OutputPipelineType>
struct UnpackResultImpl<MatrixMap<ResultScalarType, MapOrder::ColMajor>,
                 PackedResultType, LhsOffset, RhsOffset, OutputPipelineType>
{
  using ResultBlockType = MatrixMap<ResultScalarType, MapOrder::ColMajor>;
  static void Run(ResultBlockType* dst, const MatrixBlockBounds& dst_block,
                  const PackedResultType& src, int depth,
                  const std::int32_t* lhs_sums_of_each_slice_ptr,
                  const std::int32_t* rhs_sums_of_each_slice_ptr,
                  const LhsOffset& lhs_offset, const RhsOffset& rhs_offset,
                  const OutputPipelineType& output_pipeline) {
    ScopedProfilingLabel label("unpack to column-major destination");
    assert(dst_block.start_row >= 0);
    assert(dst_block.start_row + dst_block.rows <= dst->rows());
    assert(dst_block.start_col >= 0);
    assert(dst_block.start_col + dst_block.cols <= dst->cols());
    const auto src_map = src.Map();
    const VectorMap<const std::int32_t, VectorShape::Col>
       lhs_sums_of_each_slice(lhs_sums_of_each_slice_ptr, dst_block.rows);
    const VectorMap<const std::int32_t, VectorShape::Row>
       rhs_sums_of_each_slice(rhs_sums_of_each_slice_ptr, dst_block.cols);
    using Int32x1x1 = RegisterBlock<std::int32_t,1,1>;
    using Int32x4x1 = RegisterBlock<std::int32_t,4,1>;
    using Int32x8x1 = RegisterBlock<std::int32_t,8,1>;
    using Int32x1x4 = RegisterBlock<std::int32_t,1,4>;
    using Int32x4x4 = RegisterBlock<std::int32_t,4,4>;
    using Int32x8x4 = RegisterBlock<std::int32_t,8,4>;

    OutputPipelineExecutor<OutputPipelineType, Int32x1x1>
        output_pipeline_executor_1x1(output_pipeline);
    OutputPipelineExecutor<OutputPipelineType, Int32x4x1>
        output_pipeline_executor_4x1(output_pipeline);
    OutputPipelineExecutor<OutputPipelineType, Int32x8x1>
        output_pipeline_executor_8x1(output_pipeline);
    OutputPipelineExecutor<OutputPipelineType, Int32x1x4>
        output_pipeline_executor_1x4(output_pipeline);
    OutputPipelineExecutor<OutputPipelineType, Int32x4x4>
        output_pipeline_executor_4x4(output_pipeline);
    OutputPipelineExecutor<OutputPipelineType, Int32x8x4>
        output_pipeline_executor_8x4(output_pipeline);

    int c = 0;
    for (; c <= dst_block.cols - 4; c += 4) {
      int r = 0;
      for (; r <= dst_block.rows - 8; r += 8) {
        UnpackResultBlock<Int32x8x4>(src_map, output_pipeline_executor_8x4, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
      for (; r <= dst_block.rows - 4; r += 4) {
        UnpackResultBlock<Int32x4x4>(src_map, output_pipeline_executor_4x4, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
      for (; r < dst_block.rows; r++) {
        UnpackResultBlock<Int32x1x4>(src_map, output_pipeline_executor_1x4, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
    }
    for (; c < dst_block.cols; c++) {
      int r = 0;
      for (; r <= dst_block.rows - 8; r += 8) {
        UnpackResultBlock<Int32x8x1>(src_map, output_pipeline_executor_8x1, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
      for (; r <= dst_block.rows - 4; r += 4) {
        UnpackResultBlock<Int32x4x1>(src_map, output_pipeline_executor_4x1, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
      for (; r < dst_block.rows; r++) {
        UnpackResultBlock<Int32x1x1>(src_map, output_pipeline_executor_1x1, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
    }
  }
};

template <typename ResultScalarType, typename PackedResultType,
          typename LhsOffset, typename RhsOffset, typename OutputPipelineType>
struct UnpackResultImpl<MatrixMap<ResultScalarType, MapOrder::RowMajor>,
                 PackedResultType, LhsOffset, RhsOffset, OutputPipelineType>
{
  using ResultBlockType = MatrixMap<ResultScalarType, MapOrder::RowMajor>;
  static void Run(ResultBlockType* dst, const MatrixBlockBounds& dst_block,
                  const PackedResultType& src, int depth,
                  const std::int32_t* lhs_sums_of_each_slice_ptr,
                  const std::int32_t* rhs_sums_of_each_slice_ptr,
                  const LhsOffset& lhs_offset, const RhsOffset& rhs_offset,
                  const OutputPipelineType& output_pipeline) {
    ScopedProfilingLabel label("unpack to row-major destination");
    assert(dst_block.start_row >= 0);
    assert(dst_block.start_row + dst_block.rows <= dst->rows());
    assert(dst_block.start_col >= 0);
    assert(dst_block.start_col + dst_block.cols <= dst->cols());
    const auto src_map = src.Map();
    const VectorMap<const std::int32_t, VectorShape::Col>
       lhs_sums_of_each_slice(lhs_sums_of_each_slice_ptr, dst_block.rows);
    const VectorMap<const std::int32_t, VectorShape::Row>
       rhs_sums_of_each_slice(rhs_sums_of_each_slice_ptr, dst_block.cols);
    using Int32x1x1 = RegisterBlock<std::int32_t,1,1>;
    using Int32x4x1 = RegisterBlock<std::int32_t,4,1>;
    using Int32x8x1 = RegisterBlock<std::int32_t,8,1>;
    using Int32x1x4 = RegisterBlock<std::int32_t,1,4>;
    using Int32x4x4 = RegisterBlock<std::int32_t,4,4>;
    using Int32x8x4 = RegisterBlock<std::int32_t,8,4>;

    OutputPipelineExecutor<OutputPipelineType, Int32x1x1>
        output_pipeline_executor_1x1(output_pipeline);
    OutputPipelineExecutor<OutputPipelineType, Int32x4x1>
        output_pipeline_executor_4x1(output_pipeline);
    OutputPipelineExecutor<OutputPipelineType, Int32x8x1>
        output_pipeline_executor_8x1(output_pipeline);
    OutputPipelineExecutor<OutputPipelineType, Int32x1x4>
        output_pipeline_executor_1x4(output_pipeline);
    OutputPipelineExecutor<OutputPipelineType, Int32x4x4>
        output_pipeline_executor_4x4(output_pipeline);
    OutputPipelineExecutor<OutputPipelineType, Int32x8x4>
        output_pipeline_executor_8x4(output_pipeline);

    int c = 0;
    for (; c <= dst_block.cols - 4; c += 4) {
      int r = 0;
      for (; r <= dst_block.rows - 8; r += 8) {
        UnpackResultBlock<Int32x8x4>(src_map, output_pipeline_executor_8x4, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
      for (; r <= dst_block.rows - 4; r += 4) {
        UnpackResultBlock<Int32x4x4>(src_map, output_pipeline_executor_4x4, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
      for (; r < dst_block.rows; r++) {
        UnpackResultBlock<Int32x1x4>(src_map, output_pipeline_executor_1x4, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
    }
    for (; c < dst_block.cols; c++) {
      int r = 0;
      for (; r <= dst_block.rows - 8; r += 8) {
        UnpackResultBlock<Int32x8x1>(src_map, output_pipeline_executor_8x1, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
      for (; r <= dst_block.rows - 4; r += 4) {
        UnpackResultBlock<Int32x4x1>(src_map, output_pipeline_executor_4x1, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
      for (; r < dst_block.rows; r++) {
        UnpackResultBlock<Int32x1x1>(src_map, output_pipeline_executor_1x1, dst,
          lhs_sums_of_each_slice, rhs_sums_of_each_slice,
          lhs_offset, rhs_offset,
          depth,
          r, c, r + dst_block.start_row, c + dst_block.start_col);
      }
    }
  }
};

template <typename ResultBlockType, typename PackedResultType,
          typename LhsOffset, typename RhsOffset, typename OutputPipelineType>
void UnpackResult(ResultBlockType* dst, const MatrixBlockBounds& dst_block,
                  const PackedResultType& src, int depth,
                  const std::int32_t* lhs_sums_of_each_slice_ptr,
                  const std::int32_t* rhs_sums_of_each_slice_ptr,
                  const LhsOffset& lhs_offset, const RhsOffset& rhs_offset,
                  const OutputPipelineType& output_pipeline) {
  UnpackResultImpl<ResultBlockType, PackedResultType, LhsOffset, RhsOffset, OutputPipelineType>::Run(
    dst, dst_block, src, depth, lhs_sums_of_each_slice_ptr, rhs_sums_of_each_slice_ptr,
    lhs_offset, rhs_offset, output_pipeline);
}

}  // end namespace gemmlowp

#endif  // GEMMLOWP_INTERNAL_UNPACK_H_
