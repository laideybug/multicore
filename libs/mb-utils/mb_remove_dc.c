#include "mb_utils.h"

void mb_remove_dc(size_t rows, size_t cols, float matrix[rows][cols]) {
    float ones_vector[rows];
    float mean_vector[cols];
    float mult_matrix[rows][cols];

    mb_fill_vector(rows, ones_vector, 1.0f);
    mb_column_mean(rows, cols, matrix, mean_vector);

    for (int j = 0; j < rows; ++j) {
        for (int k = 0; k < cols; ++k) {
            mult_matrix[j][k] = ones_vector[j] * mean_vector[k];
        }
    }

    for (int j = 0; j < rows; ++j) {
        for (int k = 0; k < cols; ++k) {
            matrix[j][k] -= mult_matrix[j][k];
        }
    }
}
