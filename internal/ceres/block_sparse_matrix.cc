// Ceres Solver - A fast non-linear least squares minimizer
// Copyright 2022 Google Inc. All rights reserved.
// http://ceres-solver.org/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors may be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Author: sameeragarwal@google.com (Sameer Agarwal)

#include "ceres/block_sparse_matrix.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <random>
#include <vector>

#include "ceres/block_structure.h"
#include "ceres/crs_matrix.h"
#include "ceres/internal/eigen.h"
#include "ceres/small_blas.h"
#include "ceres/triplet_sparse_matrix.h"
#include "glog/logging.h"

namespace ceres::internal {

using std::vector;

BlockSparseMatrix::BlockSparseMatrix(
    CompressedRowBlockStructure* block_structure)
    : num_rows_(0),
      num_cols_(0),
      num_nonzeros_(0),
      block_structure_(block_structure) {
  CHECK(block_structure_ != nullptr);

  // Count the number of columns in the matrix.
  for (auto& col : block_structure_->cols) {
    num_cols_ += col.size;
  }

  // Count the number of non-zero entries and the number of rows in
  // the matrix.
  for (int i = 0; i < block_structure_->rows.size(); ++i) {
    int row_block_size = block_structure_->rows[i].block.size;
    num_rows_ += row_block_size;

    const vector<Cell>& cells = block_structure_->rows[i].cells;
    for (const auto& cell : cells) {
      int col_block_id = cell.block_id;
      int col_block_size = block_structure_->cols[col_block_id].size;
      num_nonzeros_ += col_block_size * row_block_size;
    }
  }

  CHECK_GE(num_rows_, 0);
  CHECK_GE(num_cols_, 0);
  CHECK_GE(num_nonzeros_, 0);
  VLOG(2) << "Allocating values array with " << num_nonzeros_ * sizeof(double)
          << " bytes.";  // NOLINT
  values_ = std::make_unique<double[]>(num_nonzeros_);
  max_num_nonzeros_ = num_nonzeros_;
  CHECK(values_ != nullptr);
}

void BlockSparseMatrix::SetZero() {
  std::fill(values_.get(), values_.get() + num_nonzeros_, 0.0);
}

void BlockSparseMatrix::RightMultiplyAndAccumulate(const double* x,
                                                   double* y) const {
  CHECK(x != nullptr);
  CHECK(y != nullptr);

  for (int i = 0; i < block_structure_->rows.size(); ++i) {
    int row_block_pos = block_structure_->rows[i].block.position;
    int row_block_size = block_structure_->rows[i].block.size;
    const vector<Cell>& cells = block_structure_->rows[i].cells;
    for (const auto& cell : cells) {
      int col_block_id = cell.block_id;
      int col_block_size = block_structure_->cols[col_block_id].size;
      int col_block_pos = block_structure_->cols[col_block_id].position;
      MatrixVectorMultiply<Eigen::Dynamic, Eigen::Dynamic, 1>(
          values_.get() + cell.position,
          row_block_size,
          col_block_size,
          x + col_block_pos,
          y + row_block_pos);
    }
  }
}

void BlockSparseMatrix::LeftMultiplyAndAccumulate(const double* x,
                                                  double* y) const {
  CHECK(x != nullptr);
  CHECK(y != nullptr);

  for (int i = 0; i < block_structure_->rows.size(); ++i) {
    int row_block_pos = block_structure_->rows[i].block.position;
    int row_block_size = block_structure_->rows[i].block.size;
    const vector<Cell>& cells = block_structure_->rows[i].cells;
    for (const auto& cell : cells) {
      int col_block_id = cell.block_id;
      int col_block_size = block_structure_->cols[col_block_id].size;
      int col_block_pos = block_structure_->cols[col_block_id].position;
      MatrixTransposeVectorMultiply<Eigen::Dynamic, Eigen::Dynamic, 1>(
          values_.get() + cell.position,
          row_block_size,
          col_block_size,
          x + row_block_pos,
          y + col_block_pos);
    }
  }
}

void BlockSparseMatrix::SquaredColumnNorm(double* x) const {
  CHECK(x != nullptr);
  VectorRef(x, num_cols_).setZero();
  for (int i = 0; i < block_structure_->rows.size(); ++i) {
    int row_block_size = block_structure_->rows[i].block.size;
    const vector<Cell>& cells = block_structure_->rows[i].cells;
    for (const auto& cell : cells) {
      int col_block_id = cell.block_id;
      int col_block_size = block_structure_->cols[col_block_id].size;
      int col_block_pos = block_structure_->cols[col_block_id].position;
      const MatrixRef m(
          values_.get() + cell.position, row_block_size, col_block_size);
      VectorRef(x + col_block_pos, col_block_size) += m.colwise().squaredNorm();
    }
  }
}

void BlockSparseMatrix::ScaleColumns(const double* scale) {
  CHECK(scale != nullptr);

  for (int i = 0; i < block_structure_->rows.size(); ++i) {
    int row_block_size = block_structure_->rows[i].block.size;
    const vector<Cell>& cells = block_structure_->rows[i].cells;
    for (const auto& cell : cells) {
      int col_block_id = cell.block_id;
      int col_block_size = block_structure_->cols[col_block_id].size;
      int col_block_pos = block_structure_->cols[col_block_id].position;
      MatrixRef m(
          values_.get() + cell.position, row_block_size, col_block_size);
      m *= ConstVectorRef(scale + col_block_pos, col_block_size).asDiagonal();
    }
  }
}

void BlockSparseMatrix::ToCompressedRowSparseMatrix(
    CompressedRowSparseMatrix* crs_matrix) const {
  {
    TripletSparseMatrix ts_matrix;
    this->ToTripletSparseMatrix(&ts_matrix);
    *crs_matrix =
        *CompressedRowSparseMatrix::FromTripletSparseMatrix(ts_matrix);
  }

  int num_row_blocks = block_structure_->rows.size();
  auto& row_blocks = *crs_matrix->mutable_row_blocks();
  row_blocks.resize(num_row_blocks);
  for (int i = 0; i < num_row_blocks; ++i) {
    row_blocks[i] = block_structure_->rows[i].block.size;
  }

  int num_col_blocks = block_structure_->cols.size();
  auto& col_blocks = *crs_matrix->mutable_col_blocks();
  col_blocks.resize(num_col_blocks);
  for (int i = 0; i < num_col_blocks; ++i) {
    col_blocks[i] = block_structure_->cols[i].size;
  }
}

void BlockSparseMatrix::ToDenseMatrix(Matrix* dense_matrix) const {
  CHECK(dense_matrix != nullptr);

  dense_matrix->resize(num_rows_, num_cols_);
  dense_matrix->setZero();
  Matrix& m = *dense_matrix;

  for (int i = 0; i < block_structure_->rows.size(); ++i) {
    int row_block_pos = block_structure_->rows[i].block.position;
    int row_block_size = block_structure_->rows[i].block.size;
    const vector<Cell>& cells = block_structure_->rows[i].cells;
    for (const auto& cell : cells) {
      int col_block_id = cell.block_id;
      int col_block_size = block_structure_->cols[col_block_id].size;
      int col_block_pos = block_structure_->cols[col_block_id].position;
      int jac_pos = cell.position;
      m.block(row_block_pos, col_block_pos, row_block_size, col_block_size) +=
          MatrixRef(values_.get() + jac_pos, row_block_size, col_block_size);
    }
  }
}

void BlockSparseMatrix::ToTripletSparseMatrix(
    TripletSparseMatrix* matrix) const {
  CHECK(matrix != nullptr);

  matrix->Reserve(num_nonzeros_);
  matrix->Resize(num_rows_, num_cols_);
  matrix->SetZero();

  for (int i = 0; i < block_structure_->rows.size(); ++i) {
    int row_block_pos = block_structure_->rows[i].block.position;
    int row_block_size = block_structure_->rows[i].block.size;
    const vector<Cell>& cells = block_structure_->rows[i].cells;
    for (const auto& cell : cells) {
      int col_block_id = cell.block_id;
      int col_block_size = block_structure_->cols[col_block_id].size;
      int col_block_pos = block_structure_->cols[col_block_id].position;
      int jac_pos = cell.position;
      for (int r = 0; r < row_block_size; ++r) {
        for (int c = 0; c < col_block_size; ++c, ++jac_pos) {
          matrix->mutable_rows()[jac_pos] = row_block_pos + r;
          matrix->mutable_cols()[jac_pos] = col_block_pos + c;
          matrix->mutable_values()[jac_pos] = values_[jac_pos];
        }
      }
    }
  }
  matrix->set_num_nonzeros(num_nonzeros_);
}

// Return a pointer to the block structure. We continue to hold
// ownership of the object though.
const CompressedRowBlockStructure* BlockSparseMatrix::block_structure() const {
  return block_structure_.get();
}

void BlockSparseMatrix::ToTextFile(FILE* file) const {
  CHECK(file != nullptr);
  for (int i = 0; i < block_structure_->rows.size(); ++i) {
    const int row_block_pos = block_structure_->rows[i].block.position;
    const int row_block_size = block_structure_->rows[i].block.size;
    const vector<Cell>& cells = block_structure_->rows[i].cells;
    for (const auto& cell : cells) {
      const int col_block_id = cell.block_id;
      const int col_block_size = block_structure_->cols[col_block_id].size;
      const int col_block_pos = block_structure_->cols[col_block_id].position;
      int jac_pos = cell.position;
      for (int r = 0; r < row_block_size; ++r) {
        for (int c = 0; c < col_block_size; ++c) {
          fprintf(file,
                  "% 10d % 10d %17f\n",
                  row_block_pos + r,
                  col_block_pos + c,
                  values_[jac_pos++]);
        }
      }
    }
  }
}

std::unique_ptr<BlockSparseMatrix> BlockSparseMatrix::CreateDiagonalMatrix(
    const double* diagonal, const std::vector<Block>& column_blocks) {
  // Create the block structure for the diagonal matrix.
  auto* bs = new CompressedRowBlockStructure();
  bs->cols = column_blocks;
  int position = 0;
  bs->rows.resize(column_blocks.size(), CompressedRow(1));
  for (int i = 0; i < column_blocks.size(); ++i) {
    CompressedRow& row = bs->rows[i];
    row.block = column_blocks[i];
    Cell& cell = row.cells[0];
    cell.block_id = i;
    cell.position = position;
    position += row.block.size * row.block.size;
  }

  // Create the BlockSparseMatrix with the given block structure.
  auto matrix = std::make_unique<BlockSparseMatrix>(bs);
  matrix->SetZero();

  // Fill the values array of the block sparse matrix.
  double* values = matrix->mutable_values();
  for (const auto& column_block : column_blocks) {
    const int size = column_block.size;
    for (int j = 0; j < size; ++j) {
      // (j + 1) * size is compact way of accessing the (j,j) entry.
      values[j * (size + 1)] = diagonal[j];
    }
    diagonal += size;
    values += size * size;
  }

  return matrix;
}

void BlockSparseMatrix::AppendRows(const BlockSparseMatrix& m) {
  CHECK_EQ(m.num_cols(), num_cols());
  const CompressedRowBlockStructure* m_bs = m.block_structure();
  CHECK_EQ(m_bs->cols.size(), block_structure_->cols.size());

  const int old_num_nonzeros = num_nonzeros_;
  const int old_num_row_blocks = block_structure_->rows.size();
  block_structure_->rows.resize(old_num_row_blocks + m_bs->rows.size());

  for (int i = 0; i < m_bs->rows.size(); ++i) {
    const CompressedRow& m_row = m_bs->rows[i];
    CompressedRow& row = block_structure_->rows[old_num_row_blocks + i];
    row.block.size = m_row.block.size;
    row.block.position = num_rows_;
    num_rows_ += m_row.block.size;
    row.cells.resize(m_row.cells.size());
    for (int c = 0; c < m_row.cells.size(); ++c) {
      const int block_id = m_row.cells[c].block_id;
      row.cells[c].block_id = block_id;
      row.cells[c].position = num_nonzeros_;
      num_nonzeros_ += m_row.block.size * m_bs->cols[block_id].size;
    }
  }

  if (num_nonzeros_ > max_num_nonzeros_) {
    std::unique_ptr<double[]> new_values =
        std::make_unique<double[]>(num_nonzeros_);
    std::copy_n(values_.get(), old_num_nonzeros, new_values.get());
    values_ = std::move(new_values);
    max_num_nonzeros_ = num_nonzeros_;
  }

  std::copy(m.values(),
            m.values() + m.num_nonzeros(),
            values_.get() + old_num_nonzeros);
}

void BlockSparseMatrix::DeleteRowBlocks(const int delta_row_blocks) {
  const int num_row_blocks = block_structure_->rows.size();
  int delta_num_nonzeros = 0;
  int delta_num_rows = 0;
  const std::vector<Block>& column_blocks = block_structure_->cols;
  for (int i = 0; i < delta_row_blocks; ++i) {
    const CompressedRow& row = block_structure_->rows[num_row_blocks - i - 1];
    delta_num_rows += row.block.size;
    for (int c = 0; c < row.cells.size(); ++c) {
      const Cell& cell = row.cells[c];
      delta_num_nonzeros += row.block.size * column_blocks[cell.block_id].size;
    }
  }
  num_nonzeros_ -= delta_num_nonzeros;
  num_rows_ -= delta_num_rows;
  block_structure_->rows.resize(num_row_blocks - delta_row_blocks);
}

std::unique_ptr<BlockSparseMatrix> BlockSparseMatrix::CreateRandomMatrix(
    const BlockSparseMatrix::RandomMatrixOptions& options, std::mt19937& prng) {
  CHECK_GT(options.num_row_blocks, 0);
  CHECK_GT(options.min_row_block_size, 0);
  CHECK_GT(options.max_row_block_size, 0);
  CHECK_LE(options.min_row_block_size, options.max_row_block_size);
  CHECK_GT(options.block_density, 0.0);
  CHECK_LE(options.block_density, 1.0);

  std::uniform_int_distribution<int> col_distribution(
      options.min_col_block_size, options.max_col_block_size);
  std::uniform_int_distribution<int> row_distribution(
      options.min_row_block_size, options.max_row_block_size);
  auto* bs = new CompressedRowBlockStructure();
  if (options.col_blocks.empty()) {
    CHECK_GT(options.num_col_blocks, 0);
    CHECK_GT(options.min_col_block_size, 0);
    CHECK_GT(options.max_col_block_size, 0);
    CHECK_LE(options.min_col_block_size, options.max_col_block_size);

    // Generate the col block structure.
    int col_block_position = 0;
    for (int i = 0; i < options.num_col_blocks; ++i) {
      const int col_block_size = col_distribution(prng);
      bs->cols.emplace_back(col_block_size, col_block_position);
      col_block_position += col_block_size;
    }
  } else {
    bs->cols = options.col_blocks;
  }

  bool matrix_has_blocks = false;
  std::uniform_real_distribution<double> uniform01(0.0, 1.0);
  while (!matrix_has_blocks) {
    VLOG(1) << "Clearing";
    bs->rows.clear();
    int row_block_position = 0;
    int value_position = 0;
    for (int r = 0; r < options.num_row_blocks; ++r) {
      const int row_block_size = row_distribution(prng);
      bs->rows.emplace_back();
      CompressedRow& row = bs->rows.back();
      row.block.size = row_block_size;
      row.block.position = row_block_position;
      row_block_position += row_block_size;
      for (int c = 0; c < bs->cols.size(); ++c) {
        if (uniform01(prng) > options.block_density) continue;

        row.cells.emplace_back();
        Cell& cell = row.cells.back();
        cell.block_id = c;
        cell.position = value_position;
        value_position += row_block_size * bs->cols[c].size;
        matrix_has_blocks = true;
      }
    }
  }

  auto matrix = std::make_unique<BlockSparseMatrix>(bs);
  double* values = matrix->mutable_values();
  std::normal_distribution<double> standard_normal_distribution;
  std::generate_n(
      values, matrix->num_nonzeros(), [&standard_normal_distribution, &prng] {
        return standard_normal_distribution(prng);
      });

  return matrix;
}

}  // namespace ceres::internal
