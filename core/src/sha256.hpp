// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
 *
 * Portable SHA-256 implementation — no external dependencies.
 * Shared header used by processor.cpp and video_processor.cpp.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>

namespace sha256 {

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

inline uint32_t rotr(uint32_t x,uint32_t n){return(x>>n)|(x<<(32-n));}
inline uint32_t ch (uint32_t e,uint32_t f,uint32_t g){return(e&f)^(~e&g);}
inline uint32_t maj(uint32_t a,uint32_t b,uint32_t c){return(a&b)^(a&c)^(b&c);}
inline uint32_t S0(uint32_t a){return rotr(a,2)^rotr(a,13)^rotr(a,22);}
inline uint32_t S1(uint32_t e){return rotr(e,6)^rotr(e,11)^rotr(e,25);}
inline uint32_t s0(uint32_t w){return rotr(w,7)^rotr(w,18)^(w>>3);}
inline uint32_t s1(uint32_t w){return rotr(w,17)^rotr(w,19)^(w>>10);}

/**
 * @brief Streaming SHA-256 hasher.
 *
 * Allows hashing large files in chunks without loading the entire
 * file into memory.
 */
class Hasher {
public:
    Hasher() { reset(); }

    void update(const uint8_t* data, size_t len) {
        size_t offset = bit_len_ % 512 / 8;
        bit_len_ += static_cast<uint64_t>(len) * 8;

        // If we have partial data from previous update, complete a block
        if (offset > 0 && len + offset >= 64) {
            std::memcpy(buf_ + offset, data, 64 - offset);
            transform(buf_);
            data += 64 - offset;
            len -= 64 - offset;
            offset = 0;
        }

        // Process full 64-byte blocks
        while (len >= 64) {
            transform(data);
            data += 64;
            len -= 64;
        }

        // Store remaining bytes
        if (len > 0) {
            std::memcpy(buf_ + offset, data, len);
        }
    }

    void update(const void* data, size_t len) {
        update(static_cast<const uint8_t*>(data), len);
    }

    std::string finalize() {
        // Pad message
        uint64_t bit_len = bit_len_;
        size_t offset = bit_len % 512 / 8;
        buf_[offset++] = 0x80;

        // If not enough room for 8-byte length, pad and process
        if (offset > 56) {
            std::memset(buf_ + offset, 0, 64 - offset);
            transform(buf_);
            offset = 0;
        }

        std::memset(buf_ + offset, 0, 56 - offset);
        // Write bit length in big-endian
        for (int i = 7; i >= 0; --i)
            buf_[56 + (7 - i)] = static_cast<uint8_t>(bit_len >> (i * 8));

        transform(buf_);

        // Build hex string
        std::ostringstream ss;
        for (int i = 0; i < 8; ++i)
            ss << std::hex << std::setw(8) << std::setfill('0') << h_[i];
        return ss.str();
    }

    /**
     * @brief Hash an entire file using streaming reads (64KB chunks).
     */
    static std::string hash_file(const std::string& path) {
        std::ifstream raw(path, std::ios::binary);
        if (!raw) return "";

        Hasher hasher;
        char buf[64 * 1024];
        while (raw.read(buf, sizeof(buf)) || raw.gcount() > 0) {
            hasher.update(buf, static_cast<size_t>(raw.gcount()));
            if (raw.eof()) break;
        }
        return hasher.finalize();
    }

    /**
     * @brief Hash a memory buffer (legacy convenience function).
     */
    static std::string hash(const uint8_t* data, size_t len) {
        Hasher hasher;
        hasher.update(data, len);
        return hasher.finalize();
    }

private:
    uint32_t h_[8];
    uint64_t bit_len_;
    uint8_t  buf_[64];

    void reset() {
        h_[0]=0x6a09e667; h_[1]=0xbb67ae85; h_[2]=0x3c6ef372; h_[3]=0xa54ff53a;
        h_[4]=0x510e527f; h_[5]=0x9b05688c; h_[6]=0x1f83d9ab; h_[7]=0x5be0cd19;
        bit_len_ = 0;
        std::memset(buf_, 0, sizeof(buf_));
    }

    void transform(const uint8_t* block) {
        uint32_t w[64];
        for (int j = 0; j < 16; ++j)
            w[j] = (static_cast<uint32_t>(block[j*4]) << 24) |
                   (static_cast<uint32_t>(block[j*4+1]) << 16) |
                   (static_cast<uint32_t>(block[j*4+2]) << 8) |
                    static_cast<uint32_t>(block[j*4+3]);
        for (int j = 16; j < 64; ++j)
            w[j] = s1(w[j-2]) + w[j-7] + s0(w[j-15]) + w[j-16];

        uint32_t a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],hh=h_[7];
        for (int j = 0; j < 64; ++j) {
            uint32_t t1 = hh + S1(e) + ch(e,f,g) + K[j] + w[j];
            uint32_t t2 = S0(a) + maj(a,b,c);
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d;
        h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=hh;
    }
};

} // namespace sha256
