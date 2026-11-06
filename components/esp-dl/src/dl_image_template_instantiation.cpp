#include "image/dl_image.hpp"

namespace dl
{
    namespace image
    {
        template void warp_affine<uint16_t>(dl::Tensor<uint16_t> *input, dl::Tensor<uint16_t> *output, dl::math::Matrix<float> *M_inv);
        template void warp_affine<uint8_t>(dl::Tensor<uint8_t> *input, dl::Tensor<uint8_t> *output, dl::math::Matrix<float> *M_inv);
        template void warp_affine<int16_t>(dl::Tensor<int16_t> *input, dl::Tensor<int16_t> *output, dl::math::Matrix<float> *M_inv);
        
        template void warp_affine<uint16_t>(uint16_t *input, std::vector<int> shape, dl::Tensor<uint16_t> *output, dl::math::Matrix<float> *M_inv);
        template void warp_affine<uint8_t>(uint16_t *input, std::vector<int> shape, dl::Tensor<uint8_t> *output, dl::math::Matrix<float> *M_inv);
        template void warp_affine<int16_t>(uint16_t *input, std::vector<int> shape, dl::Tensor<int16_t> *output, dl::math::Matrix<float> *M_inv);
    }
}
