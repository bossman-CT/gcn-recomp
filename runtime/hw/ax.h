#pragma once
#include <cstdint>

// ucode_hash is dsp_hle.cpp's fingerprint of the booted ucode; it picks the PB format.
void ax_init(uint32_t ucode_hash);
void ax_process_cmdlist(uint32_t addr, uint16_t size);
// What the AI DMA should play for the block at addr instead of guest memory, big endian
// like it: the enhanced mix (Audio > Quality), where the AX output wrote one there and the
// guest has not changed the block since. Null otherwise: play what memory holds.
const int16_t* ax_host_block(uint32_t addr, uint32_t frames);
