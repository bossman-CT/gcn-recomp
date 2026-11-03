#pragma once

#include <cstdint>

namespace gx {

// A texture coordinate's scale comes from its slot's SU_SSIZE/SU_TSIZE register (size minus
// one in the low 16 bits), not from the texture bound to the stage. The SDK sets it to the
// image's size, so for most draws the two agree; a draw that submits pixel coordinates
// (a font atlas) sets 0, a one-texel scale, and scaling by the image instead sends its
// coordinates far outside the texture.
constexpr float texcoord_scale(uint32_t su_size) noexcept {
    return static_cast<float>((su_size & 0xFFFFu) + 1u);
}

}  // namespace gx
