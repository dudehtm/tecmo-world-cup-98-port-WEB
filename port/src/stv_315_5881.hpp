// Sega 315-5881 decryption / decompression device for ST-V (Tecmo World Cup '98).
// Ported cleanly to zero-heap portable C++17 from MAME 0.289:
//   src/mame/sega/315-5881_crypt.cpp & src/mame/sega/stv.cpp
// Original MAME license: BSD-3-Clause
// Copyright-holders: Andreas Naive, Olivier Galibert, David Haywood
#pragma once

#include <cstddef>
#include <cstdint>

namespace twc98 {

struct Stv3155881State {
    static constexpr uint32_t kBufferSize = 32768u;
    static constexpr uint32_t kLineSize = 512u;
    static constexpr uint32_t kDefaultKeyTwcup98 = 0x05200913u;
    static constexpr uint32_t kFlagCompressed = 0x00020000u;

    // Cart ROM pointer supplied by host/machine at call time (non-owning).
    uint32_t key = kDefaultKeyTwcup98;
    uint16_t subkey = 0u;
    uint32_t prot_cur_address = 0u;

    uint32_t dec_header = 0u;
    uint16_t dec_hist = 0u;
    bool enc_ready = false;
    bool first_read = false;

    int32_t buffer_pos = 0;
    int32_t line_buffer_pos = 0;
    int32_t line_buffer_size = 0;
    int32_t buffer_bit = 7;
    int32_t buffer_bit2 = 15;
    int32_t block_pos = 0;
    int32_t block_size = 0;
    int32_t block_numlines = 0;
    int32_t done_compression = 0;

    uint16_t buffer2a = 0u;
    uint8_t buffer2[2]{};
    uint8_t line_cur = 0u;
    uint8_t buffer[kBufferSize]{};
    uint8_t line_buf[2][kLineSize]{};

    // ST-V A-Bus protection wrapper registers (0x04FFFFF0..0x04FFFFFF)
    uint32_t a_bus[4]{};
    uint32_t abus_protenable = 0u;
    uint32_t abus_protkey = 0u;

    void reset(uint32_t game_key = kDefaultKeyTwcup98);

    void set_addr_low(uint16_t data);
    void set_addr_high(uint16_t data);
    void set_subkey(uint16_t data);

    uint16_t do_decrypt(const uint8_t* cart_rom, size_t cart_size);

    // Big-endian SH-2 bus interface matching ST-V 0x04FFFFF0..0x04FFFFFF (offset 0..3)
    uint32_t abus_read(uint32_t ea, uint32_t width, const uint8_t* cart_rom, size_t cart_size);
    void abus_write(uint32_t ea, uint32_t width, uint32_t val);

private:
    uint16_t read_cart_word_swapped(const uint8_t* cart_rom, size_t cart_size, uint32_t word_addr) const;
    uint16_t get_decrypted_16(const uint8_t* cart_rom, size_t cart_size);
    void enc_start(const uint8_t* cart_rom, size_t cart_size);
    void enc_fill(const uint8_t* cart_rom, size_t cart_size);
    int get_compressed_bit(const uint8_t* cart_rom, size_t cart_size);
    void line_fill(const uint8_t* cart_rom, size_t cart_size);

    static uint16_t block_decrypt(uint32_t game_key, uint16_t seq_key, uint16_t counter, uint16_t data);
};

}  // namespace twc98
