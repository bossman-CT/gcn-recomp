#pragma once
#include <cstdint>

// ucode_hash is dsp_hle.cpp's fingerprint of the booted ucode; it picks the PB format.
void ax_init(uint32_t ucode_hash);
void ax_process_cmdlist(uint32_t addr, uint16_t size);
