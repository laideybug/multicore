#include "mb_utils.h"

void mb_mean(size_t rows, size_t cols, float matrix[rows][cols], float mean_vector[cols]) {
    float column_total;

    for (int j = 0; j < cols; ++j) {
        column_total = 0.0f;

        for (int k = 0; k < rows; ++k) {
            column_total += matrix[k][j];
        }

        float mean = column_total / rows;
        mean_vector[j] = mean;
    }
}