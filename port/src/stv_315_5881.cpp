// Sega 315-5881 decryption / decompression device for ST-V (Tecmo World Cup '98).
// Ported cleanly to zero-heap portable C++17 from MAME 0.289:
//   src/mame/sega/315-5881_crypt.cpp & src/mame/sega/stv.cpp
// Original MAME license: BSD-3-Clause
// Copyright-holders: Andreas Naive, Olivier Galibert, David Haywood
#include "stv_315_5881.hpp"

#include <cstring>

namespace twc98 {
namespace {

struct CryptSbox {
    uint8_t table[64];
    int inputs[6];
    int outputs[2];
};

constexpr int kFn1Gk = 38;
constexpr int kFn2Gk = 32;

static inline uint32_t bit_at(uint32_t x, int n) {
    return (x >> n) & 1u;
}

template <int B15, int B14, int B13, int B12, int B11, int B10, int B9, int B8,
          int B7,  int B6,  int B5,  int B4,  int B3,  int B2,  int B1, int B0>
static inline uint16_t bitswap16(uint16_t v) {
    return static_cast<uint16_t>(
        (bit_at(v, B15) << 15) | (bit_at(v, B14) << 14) | (bit_at(v, B13) << 13) | (bit_at(v, B12) << 12) |
        (bit_at(v, B11) << 11) | (bit_at(v, B10) << 10) | (bit_at(v, B9)  << 9)  | (bit_at(v, B8)  << 8)  |
        (bit_at(v, B7)  << 7)  | (bit_at(v, B6)  << 6)  | (bit_at(v, B5)  << 5)  | (bit_at(v, B4)  << 4)  |
        (bit_at(v, B3)  << 3)  | (bit_at(v, B2)  << 2)  | (bit_at(v, B1)  << 1)  | (bit_at(v, B0)  << 0));
}

const CryptSbox kFn1Sboxes[4][4] = {
    {   // 1st round
        {
            {
                0,3,2,2,1,3,1,2,3,2,1,2,1,2,3,1,3,2,2,0,2,1,3,0,0,3,2,3,2,1,2,0,
                2,3,1,1,2,2,1,1,1,0,2,3,3,0,2,1,1,1,1,1,3,0,3,2,1,0,1,2,0,3,1,3,
            },
            {3,4,5,7,-1,-1},
            {0,4}
        },
        {
            {
                2,2,2,0,3,3,0,1,2,2,3,2,3,0,2,2,1,1,0,3,3,2,0,2,0,1,0,1,2,3,1,1,
                0,1,3,3,1,3,3,1,2,3,2,0,0,0,2,2,0,3,1,3,0,3,2,2,0,3,0,3,1,1,0,2,
            },
            {0,1,2,5,6,7},
            {1,6}
        },
        {
            {
                0,1,3,0,3,1,1,1,1,2,3,1,3,0,2,3,3,2,0,2,1,1,2,1,1,3,1,0,0,2,0,1,
                1,3,1,0,0,3,2,3,2,0,3,3,0,0,0,0,1,2,3,3,2,0,3,2,1,0,0,0,2,2,3,3,
            },
            {0,2,5,6,7,-1},
            {2,3}
        },
        {
            {
                3,2,1,2,1,2,3,2,0,3,2,2,3,1,3,3,0,2,3,0,3,3,2,1,1,1,2,0,2,2,0,1,
                1,3,3,0,0,3,0,3,0,2,1,3,2,1,0,0,0,1,1,2,0,1,0,0,0,1,3,3,2,0,3,3,
            },
            {1,2,3,4,6,7},
            {5,7}
        },
    },
    {   // 2nd round
        {
            {
                3,3,1,2,0,0,2,2,2,1,2,1,3,1,1,3,3,0,0,3,0,3,3,2,1,1,3,2,3,2,1,3,
                2,3,0,1,3,2,0,1,2,1,3,1,2,2,3,3,3,1,2,2,0,3,1,2,2,1,3,0,3,0,1,3,
            },
            {0,1,3,4,5,7},
            {0,4}
        },
        {
            {
                2,0,1,0,0,3,2,0,3,3,1,2,1,3,0,2,0,2,0,0,0,2,3,1,3,1,1,2,3,0,3,0,
                3,0,2,0,0,2,2,1,0,2,3,3,1,3,1,0,1,3,3,0,0,1,3,1,0,2,0,3,2,1,0,1,
            },
            {0,1,3,4,6,-1},
            {1,5}
        },
        {
            {
                2,2,2,3,1,1,0,1,3,3,1,1,2,2,2,0,0,3,2,3,3,0,2,1,2,2,3,0,1,3,0,0,
                3,2,0,3,2,0,1,0,0,1,2,2,3,3,0,2,2,1,3,1,1,1,1,2,0,3,1,0,0,2,3,2,
            },
            {1,2,5,6,7,6},
            {2,7}
        },
        {
            {
                0,1,3,3,3,1,3,3,1,0,2,0,2,0,0,3,1,2,1,3,1,2,3,2,2,0,1,3,0,3,3,3,
                0,0,0,2,1,1,2,3,2,2,3,1,1,2,0,2,0,2,1,3,1,1,3,3,1,1,3,0,2,3,0,0,
            },
            {2,3,4,5,6,7},
            {3,6}
        },
    },
    {   // 3rd round
        {
            {
                0,0,1,0,1,0,0,3,2,0,0,3,0,1,0,2,0,3,0,0,2,0,3,2,2,1,3,2,2,1,1,2,
                0,0,0,3,0,1,1,0,0,2,1,0,3,1,2,2,2,0,3,1,3,0,1,2,2,1,1,1,0,2,3,1,
            },
            {1,2,3,4,5,7},
            {0,5}
        },
        {
            {
                1,2,1,0,3,1,1,2,0,0,2,3,2,3,1,3,2,0,3,2,2,3,1,1,1,1,0,3,2,0,0,1,
                1,0,0,1,3,1,2,3,0,0,2,3,3,0,1,0,0,2,3,0,1,2,0,1,3,3,3,1,2,0,2,1,
            },
            {0,2,4,5,6,7},
            {1,6}
        },
        {
            {
                0,3,0,2,1,2,0,0,1,1,0,0,3,1,1,0,0,3,0,0,2,3,3,2,3,1,2,0,0,2,3,0,
                255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
                255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
            },
            {0,2,4,6,7,-1},
            {2,3}
        },
        {
            {
                0,0,1,0,0,1,0,2,3,3,0,3,3,2,3,0,2,2,2,0,3,2,0,3,1,0,0,3,3,0,0,0,
                2,2,1,0,2,0,3,2,0,0,3,1,3,3,0,0,2,1,1,2,1,0,1,1,0,3,1,2,0,2,0,3,
            },
            {0,1,2,3,6,-1},
            {4,7}
        },
    },
    {   // 4th round
        {
            {
                0,3,3,3,3,3,2,0,0,1,2,0,2,2,2,2,1,1,0,2,2,1,3,2,3,2,0,1,2,3,2,1,
                3,2,2,3,1,0,1,0,0,2,0,1,2,1,2,3,1,2,1,1,2,2,1,0,1,3,2,3,2,0,3,1,
            },
            {0,1,3,4,5,6},
            {0,5}
        },
        {
            {
                0,3,0,0,2,0,3,1,1,1,2,2,2,1,3,1,2,2,1,3,2,2,3,3,0,3,1,0,3,2,0,1,
                3,0,2,0,1,0,2,1,3,3,1,2,2,0,2,3,3,2,3,0,1,1,3,3,0,2,1,3,0,2,2,3,
            },
            {0,1,2,3,5,7},
            {1,7}
        },
        {
            {
                0,1,2,3,3,3,3,1,2,0,2,3,2,1,0,1,2,2,1,2,0,3,2,0,1,1,0,1,3,1,3,1,
                3,1,0,0,1,0,0,0,0,1,2,2,1,1,3,3,1,2,3,3,3,2,3,0,2,2,1,3,3,0,2,0,
            },
            {2,3,4,5,6,7},
            {2,3}
        },
        {
            {
                0,2,1,1,3,2,0,3,1,0,1,0,3,2,1,1,2,2,0,3,1,0,1,2,2,2,3,3,0,0,0,0,
                1,2,1,0,2,1,2,2,2,3,2,3,0,1,3,0,0,1,3,0,0,1,1,0,1,0,0,0,0,2,0,1,
            },
            {0,1,2,4,6,7},
            {4,6}
        },
    },
};

const CryptSbox kFn2Sboxes[4][4] = {
    {   // 1st round
        {
            {
                3,3,0,1,0,1,0,0,0,3,0,0,1,3,1,2,0,3,3,3,2,1,0,1,1,1,2,2,2,3,2,2,
                2,1,3,3,1,3,1,1,0,0,1,2,0,2,2,1,1,2,3,1,2,1,3,1,2,2,0,1,3,0,2,2,
            },
            {1,3,4,5,6,7},
            {0,7}
        },
        {
            {
                0,1,3,0,1,1,2,3,2,0,0,3,2,1,3,1,3,3,0,0,1,0,0,3,0,3,3,2,3,2,0,1,
                3,2,3,2,2,1,3,1,1,1,0,3,3,2,2,1,1,2,0,2,0,1,1,0,1,0,1,1,2,0,3,0,
            },
            {0,3,5,6,5,0},
            {1,2}
        },
        {
            {
                0,2,2,1,0,1,2,1,2,0,1,2,3,3,0,1,3,1,1,2,1,2,1,3,3,2,3,3,2,1,0,1,
                0,1,0,2,0,1,1,3,2,0,3,2,1,1,1,3,2,3,0,2,3,0,2,2,1,3,0,1,1,2,2,2,
            },
            {0,2,3,4,7,-1},
            {3,4}
        },
        {
            {
                2,3,1,3,2,0,1,2,0,0,3,3,3,3,3,1,2,0,2,1,2,3,0,2,0,1,0,3,0,2,1,0,
                2,3,0,1,3,0,3,2,3,1,2,0,3,1,1,2,0,3,0,0,2,0,2,1,2,2,3,2,1,2,3,1,
            },
            {1,2,5,6,-1,-1},
            {5,6}
        },
    },
    {   // 2nd round
        {
            {
                2,3,1,3,1,0,3,3,3,2,3,3,2,0,0,3,2,3,0,3,1,1,2,3,1,1,2,2,0,1,0,0,
                2,1,0,1,2,0,1,2,0,3,1,1,2,3,1,2,0,2,0,1,3,0,1,0,2,2,3,0,3,2,3,0,
            },
            {0,1,4,5,6,7},
            {0,7}
        },
        {
            {
                0,2,2,0,2,2,0,3,2,3,2,1,3,2,3,3,1,1,0,0,3,0,2,1,1,3,3,2,3,2,0,1,
                1,2,3,0,1,0,3,0,3,1,0,2,1,2,0,3,2,3,1,2,2,0,3,2,3,0,0,1,2,3,3,3,
            },
            {0,2,3,6,7,-1},
            {1,5}
        },
        {
            {
                1,0,3,0,0,1,2,1,0,0,1,0,0,0,2,3,2,2,0,2,0,1,3,0,2,0,1,3,2,3,0,1,
                1,2,2,2,1,3,0,3,0,1,1,0,3,2,3,3,2,0,0,3,1,2,1,3,3,2,1,0,2,1,2,3,
            },
            {2,3,4,6,7,2},
            {2,3}
        },
        {
            {
                2,3,1,3,1,1,2,3,3,1,1,0,1,0,2,3,2,1,0,0,2,2,0,1,0,2,2,2,0,2,1,0,
                3,1,2,3,1,3,0,2,1,0,1,0,0,1,2,2,3,2,3,1,3,2,1,1,2,0,2,1,3,3,1,0,
            },
            {1,2,3,4,5,6},
            {4,6}
        },
    },
    {   // 3rd round
        {
            {
                0,3,0,1,3,0,0,2,1,0,1,3,2,2,2,0,3,3,3,0,2,2,0,3,0,0,2,3,0,3,2,1,
                3,3,0,3,0,2,3,3,1,1,1,0,2,2,1,1,3,0,3,1,2,0,2,0,0,0,3,2,1,1,0,0,
            },
            {1,4,5,6,7,5},
            {0,5}
        },
        {
            {
                0,3,0,1,3,0,3,1,3,2,2,2,3,0,3,2,2,1,2,2,0,3,2,2,0,0,2,1,1,3,2,3,
                2,3,3,1,2,0,1,2,2,1,0,0,0,0,2,3,1,2,0,3,1,3,1,2,3,2,1,0,3,0,0,2,
            },
            {0,2,3,4,6,7},
            {1,7}
        },
        {
            {
                2,2,0,3,0,3,1,0,1,1,2,3,2,3,1,0,0,0,3,2,2,0,2,3,1,3,2,0,3,3,1,3,
                255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
                255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
            },
            {1,2,4,7,2,-1},
            {2,4}
        },
        {
            {
                0,2,3,1,3,1,1,0,0,1,3,0,2,1,3,3,2,0,2,1,1,2,3,3,0,0,0,2,0,2,3,0,
                3,3,3,3,2,3,3,2,3,0,1,0,2,3,3,2,0,1,3,1,0,1,2,3,3,0,2,0,3,0,3,3,
            },
            {0,1,2,3,5,7},
            {3,6}
        },
    },
    {   // 4th round
        {
            {
                0,1,1,0,0,1,0,2,3,3,0,1,2,3,0,2,1,0,3,3,2,0,3,0,0,2,1,0,1,0,1,3,
                0,3,3,1,2,0,3,0,1,3,2,0,3,3,1,3,0,2,3,3,2,1,1,2,2,1,2,1,2,0,1,1,
            },
            {0,1,2,4,7,-1},
            {0,5}
        },
        {
            {
                2,0,0,2,3,0,2,3,3,1,1,1,2,1,1,0,0,2,1,0,0,3,1,0,0,3,3,0,1,0,1,2,
                0,2,0,2,0,1,2,3,2,1,1,0,3,3,3,3,3,3,1,0,3,0,0,2,0,3,2,0,2,2,0,1,
            },
            {0,1,3,5,6,-1},
            {1,3}
        },
        {
            {
                0,1,1,2,1,3,1,1,0,0,3,1,1,1,2,0,3,2,0,1,1,2,3,3,3,0,3,0,0,2,0,3,
                3,2,0,0,3,2,3,1,2,3,0,3,2,0,1,2,2,2,0,2,0,1,2,2,3,1,2,2,1,1,1,1,
            },
            {0,2,3,4,5,7},
            {2,7}
        },
        {
            {
                0,1,2,0,3,3,0,3,2,1,3,3,0,3,1,1,3,2,3,2,3,0,0,0,3,0,2,2,3,2,2,3,
                2,2,3,1,2,3,1,2,0,3,0,2,3,1,0,0,3,2,1,2,1,2,1,3,1,0,2,3,3,1,3,2,
            },
            {2,3,4,5,6,7},
            {4,6}
        },
    },
};

constexpr int kFn1GameKeySched[kFn1Gk][2] = {
    {1,29},  {1,71},  {2,4},   {2,54},  {3,8},   {4,56},  {4,73},  {5,11},
    {6,51},  {7,92},  {8,89},  {9,9},   {9,39},  {9,58},  {10,90}, {11,6},
    {12,64}, {13,49}, {14,44}, {15,40}, {16,69}, {17,15}, {18,23}, {18,43},
    {19,82}, {20,81}, {21,32}, {22,5},  {23,66}, {24,13}, {24,45}, {25,12},
    {25,35}, {26,61}, {27,10}, {27,59}, {28,25}, {29,86}
};

constexpr int kFn2GameKeySched[kFn2Gk][2] = {
    {0,0},   {1,3},   {2,11},  {3,20},  {4,22},  {5,23},  {6,29},  {7,38},
    {8,39},  {9,55},  {9,86},  {9,87},  {10,50}, {11,57}, {12,59}, {13,61},
    {14,63}, {15,67}, {16,72}, {17,83}, {18,88}, {19,94}, {20,35}, {21,17},
    {22,6},  {23,85}, {24,16}, {25,25}, {26,92}, {27,47}, {28,28}, {29,90}
};

constexpr int kFn1SeqKeySched[20][2] = {
    {0,52},  {1,34},  {2,17},  {3,36}, {4,84},  {4,88},  {5,57},  {6,48},
    {6,68},  {7,76},  {8,83},  {9,30}, {10,22}, {10,41}, {11,38}, {12,55},
    {13,74}, {14,19}, {14,80}, {15,26}
};

constexpr int kFn2SeqKeySched[16] = {77,34,8,42,36,27,69,66,13,9,79,31,49,7,24,64};
constexpr int kFn2MidResSched[16] = {1,10,44,68,74,78,81,95,2,4,30,40,41,51,53,58};

constexpr uint8_t kTrees[9][2][32] = {
    {
        {0x01,0x10,0x0f,0x05,0xc4,0x13,0x87,0x0a,0xcc,0x81,0xce,0x0c,0x86,0x0e,0x84,0xc2,
         0x11,0xc1,0xc3,0xcf,0x15,0xc8,0xcd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
        {0xc7,0x02,0x03,0x04,0x80,0x06,0x07,0x08,0x09,0xc9,0x0b,0x0d,0x82,0x83,0x85,0xc0,
         0x12,0xc6,0xc5,0x14,0x16,0xca,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    },
    {
        {0x02,0x80,0x05,0x04,0x81,0x10,0x15,0x82,0x09,0x83,0x0b,0x0c,0x0d,0xdc,0x0f,0xde,
         0x1c,0xcf,0xc5,0xdd,0x86,0x16,0x87,0x18,0x19,0x1a,0xda,0xca,0xc9,0x1e,0xce,0xff},
        {0x01,0x17,0x03,0x0a,0x08,0x06,0x07,0xc2,0xd9,0xc4,0xd8,0xc8,0x0e,0x84,0xcb,0x85,
         0x11,0x12,0x13,0x14,0xcd,0x1b,0xdb,0xc7,0xc0,0xc1,0x1d,0xdf,0xc3,0xc6,0xcc,0xff},
    },
    {
        {0xc6,0x80,0x03,0x0b,0x05,0x07,0x82,0x08,0x15,0xdc,0xdd,0x0c,0xd9,0xc2,0x14,0x10,
         0x85,0x86,0x18,0x16,0xc5,0xc4,0xc8,0xc9,0xc0,0xcc,0xff,0xff,0xff,0xff,0xff,0xff},
        {0x01,0x02,0x12,0x04,0x81,0x06,0x83,0xc3,0x09,0x0a,0x84,0x11,0x0d,0x0e,0x0f,0x19,
         0xca,0xc1,0x13,0xd8,0xda,0xdb,0x17,0xde,0xcd,0xcb,0xff,0xff,0xff,0xff,0xff,0xff},
    },
    {
        {0x01,0x80,0x0d,0x04,0x05,0x15,0x83,0x08,0xd9,0x10,0x0b,0x0c,0x84,0x0e,0xc0,0x14,
         0x12,0xcb,0x13,0xca,0xc8,0xc2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
        {0xc5,0x02,0x03,0x07,0x81,0x06,0x82,0xcc,0x09,0x0a,0xc9,0x11,0xc4,0x0f,0x85,0xd8,
         0xda,0xdb,0xc3,0xdc,0xdd,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    },
    {
        {0x01,0x80,0x06,0x0c,0x05,0x81,0xd8,0x84,0x09,0xdc,0x0b,0x0f,0x0d,0x0e,0x10,0xdb,
         0x11,0xca,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
        {0xc4,0x02,0x03,0x04,0xcb,0x0a,0x07,0x08,0xd9,0x82,0xc8,0x83,0xc0,0xc1,0xda,0xc2,
         0xc9,0xc3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    },
    {
        {0x01,0x02,0x06,0x0a,0x83,0x0b,0x07,0x08,0x09,0x82,0xd8,0x0c,0xd9,0xda,0xff,0xff,
         0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
        {0xc3,0x80,0x03,0x04,0x05,0x81,0xca,0xc8,0xdb,0xc9,0xc0,0xc1,0x0d,0xc2,0xff,0xff,
         0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    },
    {
        {0x01,0x02,0x03,0x04,0x81,0x07,0x08,0xd8,0xda,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,
         0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
        {0xc2,0x80,0x05,0xc9,0xc8,0x06,0x82,0xc0,0x09,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,
         0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    },
    {
        {0x01,0x80,0x04,0xc8,0xc0,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
         0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
        {0xc1,0x02,0x03,0x81,0x05,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
         0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    },
    {
        {0x01,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
         0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
        {0xc0,0x80,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
         0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    },
};

int feistel_function(int input, const CryptSbox* sboxes, uint32_t subkeys) {
    int result = 0;
    for (int m = 0; m < 4; ++m) {
        uint32_t aux = 0;
        for (int k = 0; k < 6; ++k) {
            if (sboxes[m].inputs[k] != -1) {
                aux |= bit_at(static_cast<uint32_t>(input), sboxes[m].inputs[k]) << k;
            }
        }
        const uint32_t val = sboxes[m].table[(aux ^ subkeys) & 0x3fu];
        for (int k = 0; k < 2; ++k) {
            result |= static_cast<int>(bit_at(val, k) << sboxes[m].outputs[k]);
        }
        subkeys >>= 6;
    }
    return result;
}

}  // namespace

void Stv3155881State::reset(uint32_t game_key) {
    key = game_key;
    subkey = 0u;
    prot_cur_address = 0u;
    dec_header = 0u;
    dec_hist = 0u;
    enc_ready = false;
    first_read = false;

    buffer_pos = 0;
    line_buffer_pos = 0;
    line_buffer_size = 0;
    buffer_bit = 7;
    buffer_bit2 = 15;
    block_pos = 0;
    block_size = 0;
    block_numlines = 0;
    done_compression = 0;

    buffer2a = 0u;
    buffer2[0] = 0u;
    buffer2[1] = 0u;
    line_cur = 0u;
    std::memset(buffer, 0, sizeof(buffer));
    std::memset(line_buf, 0, sizeof(line_buf));
    std::memset(a_bus, 0, sizeof(a_bus));
    abus_protenable = 0u;
    abus_protkey = 0u;
}

void Stv3155881State::set_addr_low(uint16_t data) {
    prot_cur_address = (prot_cur_address & 0xffff0000u) | data;
    enc_ready = false;
}

void Stv3155881State::set_addr_high(uint16_t data) {
    prot_cur_address = (prot_cur_address & 0x0000ffffu) | (static_cast<uint32_t>(data) << 16);
    enc_ready = false;
    buffer_bit = 7;
    buffer_bit2 = 15;
}

void Stv3155881State::set_subkey(uint16_t data) {
    subkey = data;
    enc_ready = false;
}

uint16_t Stv3155881State::read_cart_word_swapped(const uint8_t* cart_rom, size_t cart_size, uint32_t word_addr) const {
    const size_t byte_off = static_cast<size_t>(word_addr) * 2u;
    if (!cart_rom || byte_off + 1u >= cart_size) return 0u;
    // Matches MAME stv.cpp crypt_read_callback: BE read at 0x02000000 + 2*addr, byte-swapped.
    return static_cast<uint16_t>((static_cast<uint16_t>(cart_rom[byte_off + 1u]) << 8) | cart_rom[byte_off]);
}

uint16_t Stv3155881State::block_decrypt(uint32_t game_key, uint16_t seq_key, uint16_t counter, uint16_t data) {
    uint32_t fn1_subkeys[4]{};
    uint32_t fn2_subkeys[4]{};

    for (int j = 0; j < kFn1Gk; ++j) {
        if (bit_at(game_key, kFn1GameKeySched[j][0]) != 0u) {
            const int aux = kFn1GameKeySched[j][1] % 24;
            const int aux2 = kFn1GameKeySched[j][1] / 24;
            fn1_subkeys[aux2] ^= (1u << aux);
        }
    }
    for (int j = 0; j < kFn2Gk; ++j) {
        if (bit_at(game_key, kFn2GameKeySched[j][0]) != 0u) {
            const int aux = kFn2GameKeySched[j][1] % 24;
            const int aux2 = kFn2GameKeySched[j][1] / 24;
            fn2_subkeys[aux2] ^= (1u << aux);
        }
    }
    for (int j = 0; j < 20; ++j) {
        if (bit_at(seq_key, kFn1SeqKeySched[j][0]) != 0u) {
            const int aux = kFn1SeqKeySched[j][1] % 24;
            const int aux2 = kFn1SeqKeySched[j][1] / 24;
            fn1_subkeys[aux2] ^= (1u << aux);
        }
    }
    for (int j = 0; j < 16; ++j) {
        if (bit_at(seq_key, j) != 0u) {
            const int aux = kFn2SeqKeySched[j] % 24;
            const int aux2 = kFn2SeqKeySched[j] / 24;
            fn2_subkeys[aux2] ^= (1u << aux);
        }
    }

    int aux = bitswap16<5, 12, 14, 13, 9, 3, 6, 4, 8, 1, 15, 11, 0, 7, 10, 2>(counter);
    int B = aux >> 8;
    int A = (aux & 0xff) ^ feistel_function(B, kFn1Sboxes[0], fn1_subkeys[0]);
    B ^= feistel_function(A, kFn1Sboxes[1], fn1_subkeys[1]);
    A ^= feistel_function(B, kFn1Sboxes[2], fn1_subkeys[2]);
    B ^= feistel_function(A, kFn1Sboxes[3], fn1_subkeys[3]);
    const uint16_t middle_result = static_cast<uint16_t>(((B & 0xff) << 8) | (A & 0xff));

    for (int j = 0; j < 16; ++j) {
        if (bit_at(middle_result, j) != 0u) {
            const int bit_idx = kFn2MidResSched[j] % 24;
            const int word_idx = kFn2MidResSched[j] / 24;
            fn2_subkeys[word_idx] ^= (1u << bit_idx);
        }
    }

    aux = bitswap16<14, 3, 8, 12, 13, 7, 15, 4, 6, 2, 9, 5, 11, 0, 1, 10>(data);
    B = aux >> 8;
    A = (aux & 0xff) ^ feistel_function(B, kFn2Sboxes[0], fn2_subkeys[0]);
    B ^= feistel_function(A, kFn2Sboxes[1], fn2_subkeys[1]);
    A ^= feistel_function(B, kFn2Sboxes[2], fn2_subkeys[2]);
    B ^= feistel_function(A, kFn2Sboxes[3], fn2_subkeys[3]);
    aux = ((B & 0xff) << 8) | (A & 0xff);

    return bitswap16<15, 7, 6, 14, 13, 12, 5, 4, 3, 2, 11, 10, 9, 1, 0, 8>(static_cast<uint16_t>(aux));
}

uint16_t Stv3155881State::get_decrypted_16(const uint8_t* cart_rom, size_t cart_size) {
    const uint16_t enc = read_cart_word_swapped(cart_rom, cart_size, prot_cur_address);
    const uint16_t dec = block_decrypt(key, subkey, static_cast<uint16_t>(prot_cur_address & 0xffffu), enc);
    const uint16_t res = static_cast<uint16_t>((dec & 3u) | (dec_hist & 0xfffcu));
    dec_hist = dec;
    ++prot_cur_address;
    return res;
}

void Stv3155881State::enc_start(const uint8_t* cart_rom, size_t cart_size) {
    block_pos = 0;
    done_compression = 0;
    buffer_pos = static_cast<int32_t>(kBufferSize);

    if (buffer_bit2 < 14) {
        dec_header = static_cast<uint32_t>(buffer2a & 0x0003u) << 16;
    } else {
        dec_hist = 0u;
        dec_header = static_cast<uint32_t>(get_decrypted_16(cart_rom, cart_size)) << 16;
    }
    dec_header |= get_decrypted_16(cart_rom, cart_size);

    block_numlines = static_cast<int32_t>((dec_header & 0x000000ffu) + 1u);
    const int32_t blocky = static_cast<int32_t>(((dec_header & 0x0001ff00u) >> 8) + 1u);
    block_size = block_numlines * blocky;

    if ((dec_header & kFlagCompressed) != 0u) {
        line_buffer_size = (blocky <= static_cast<int32_t>(kLineSize)) ? blocky : static_cast<int32_t>(kLineSize);
        line_buffer_pos = line_buffer_size;
        buffer_bit = 7;
        buffer_bit2 = 15;
    }
    enc_ready = true;
}

void Stv3155881State::enc_fill(const uint8_t* cart_rom, size_t cart_size) {
    for (uint32_t i = 0; i < kBufferSize; i += 2u) {
        const uint16_t val = get_decrypted_16(cart_rom, cart_size);
        buffer[i] = static_cast<uint8_t>(val & 0xffu);
        buffer[i + 1u] = static_cast<uint8_t>(val >> 8);
        block_pos += 2;
        if ((dec_header & kFlagCompressed) == 0u) {
            if (block_pos == block_size) {
                enc_start(cart_rom, cart_size);
            }
        }
    }
    buffer_pos = 0;
}

int Stv3155881State::get_compressed_bit(const uint8_t* cart_rom, size_t cart_size) {
    if (buffer_bit2 == 15) {
        buffer_bit2 = 0;
        buffer2a = get_decrypted_16(cart_rom, cart_size);
        buffer2[0] = static_cast<uint8_t>(buffer2a & 0xffu);
        buffer2[1] = static_cast<uint8_t>(buffer2a >> 8);
        buffer_pos = 0;
    } else {
        ++buffer_bit2;
    }

    const int res = (buffer2[(buffer_pos & 1) ^ 1] >> buffer_bit) & 1;
    --buffer_bit;
    if (buffer_bit == -1) {
        buffer_bit = 7;
        ++buffer_pos;
    }
    return res;
}

void Stv3155881State::line_fill(const uint8_t* cart_rom, size_t cart_size) {
    const uint8_t* lp = line_buf[line_cur];
    line_cur ^= 1u;
    uint8_t* lc = line_buf[line_cur];
    line_buffer_pos = 0;

    for (int32_t i = 0; i < line_buffer_size;) {
        const int slot = (i != 0) ? ((i < line_buffer_size - 7) ? 1 : ((i & 7) + 1)) : 0;
        uint32_t tmp = 0u;
        while ((tmp & 0x80u) == 0u) {
            tmp = get_compressed_bit(cart_rom, cart_size) ? kTrees[slot][1][tmp] : kTrees[slot][0][tmp];
        }
        if (tmp != 0xffu) {
            const int32_t count = static_cast<int32_t>((tmp & 7u) + 1u);
            if ((tmp & 0x40u) != 0u) {
                static constexpr int kOffsets[4] = {0, 1, 0, -1};
                const int offset = kOffsets[(tmp & 0x18u) >> 3];
                for (int32_t j = 0; j < count && i < line_buffer_size; ++j) {
                    int idx = (i + offset) % line_buffer_size;
                    if (idx < 0) idx += line_buffer_size;
                    lc[static_cast<uint32_t>(i) ^ 1u] = lp[static_cast<uint32_t>(idx) ^ 1u];
                    ++i;
                }
            } else {
                uint8_t byte_val = 0u;
                for (int b = 0; b < 8; ++b) {
                    byte_val = static_cast<uint8_t>((byte_val << 1) | get_compressed_bit(cart_rom, cart_size));
                }
                for (int32_t j = 0; j < count && i < line_buffer_size; ++j) {
                    lc[static_cast<uint32_t>(i) ^ 1u] = byte_val;
                    ++i;
                }
            }
        }
    }

    ++block_pos;
    if (block_numlines == block_pos) {
        done_compression = 1;
    }
}

uint16_t Stv3155881State::do_decrypt(const uint8_t* cart_rom, size_t cart_size) {
    if (!enc_ready) {
        enc_start(cart_rom, cart_size);
    }
    const uint8_t* base = nullptr;
    if ((dec_header & kFlagCompressed) != 0u) {
        if (line_buffer_pos >= line_buffer_size) {
            if (done_compression == 1) {
                enc_start(cart_rom, cart_size);
            }
            line_fill(cart_rom, cart_size);
        }
        base = line_buf[line_cur] + line_buffer_pos;
        line_buffer_pos += 2;
    } else {
        if (buffer_pos >= static_cast<int32_t>(kBufferSize)) {
            enc_fill(cart_rom, cart_size);
        }
        base = buffer + buffer_pos;
        buffer_pos += 2;
    }
    return static_cast<uint16_t>((static_cast<uint16_t>(base[0]) << 8) | base[1]);
}

uint32_t Stv3155881State::abus_read(uint32_t ea, uint32_t width, const uint8_t* cart_rom, size_t cart_size) {
    const uint32_t offset = (ea & 0x0fu) >> 2;
    uint32_t word32 = 0u;
    if ((abus_protenable & 0x00010000u) != 0u) {
        if (offset == 3u) {
            uint16_t res = do_decrypt(cart_rom, cart_size);
            uint16_t res2 = do_decrypt(cart_rom, cart_size);
            res = static_cast<uint16_t>(((res & 0xff00u) >> 8) | ((res & 0x00ffu) << 8));
            res2 = static_cast<uint16_t>(((res2 & 0xff00u) >> 8) | ((res2 & 0x00ffu) << 8));
            word32 = res2 | (static_cast<uint32_t>(res) << 16);
        } else {
            word32 = a_bus[offset];
        }
    } else {
        word32 = a_bus[offset];
    }

    const uint32_t lane = ea & 3u;
    if (width == 4u) return word32;
    if (width == 2u) return (lane == 0u) ? (word32 >> 16) : (word32 & 0xffffu);
    return (word32 >> (24u - lane * 8u)) & 0xffu;
}

void Stv3155881State::abus_write(uint32_t ea, uint32_t width, uint32_t val) {
    const uint32_t offset = (ea & 0x0fu) >> 2;
    const uint32_t lane = ea & 3u;

    uint32_t mask = 0xffffffffu;
    uint32_t shifted = val;
    if (width == 1u) {
        const uint32_t shift = 24u - lane * 8u;
        mask = 0xffu << shift;
        shifted = (val & 0xffu) << shift;
    } else if (width == 2u) {
        const uint32_t shift = (lane == 0u) ? 16u : 0u;
        mask = 0xffffu << shift;
        shifted = (val & 0xffffu) << shift;
    }

    a_bus[offset] = (a_bus[offset] & ~mask) | (shifted & mask);

    if (offset == 0u) {
        abus_protenable = (abus_protenable & ~mask) | (shifted & mask);
    } else if (offset == 2u) {
        if ((mask & 0xffff0000u) != 0u) {
            set_addr_low(static_cast<uint16_t>(a_bus[2] >> 16));
        }
        if ((mask & 0x0000ffffu) != 0u) {
            set_addr_high(static_cast<uint16_t>(a_bus[2] & 0xffffu));
        }
    } else if (offset == 3u) {
        abus_protkey = (abus_protkey & ~mask) | (shifted & mask);
        set_subkey(static_cast<uint16_t>(abus_protkey >> 16));
    }
}

}  // namespace twc98
