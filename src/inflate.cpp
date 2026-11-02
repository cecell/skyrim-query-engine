#include "inflate.h"

#include <cstring>

namespace spf {
namespace {

constexpr int kMaxBits = 15;

struct Huffman {
    short count[kMaxBits + 1];
    short symbol[288];
};

struct State {
    const uint8_t* in;
    size_t inLen;
    size_t inPos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    std::vector<uint8_t>* out;
    size_t outMax;
    bool error = false;

    int Bits(int need) {
        uint32_t val = bitBuf;
        while (bitCnt < need) {
            if (inPos >= inLen) {
                error = true;
                return 0;
            }
            val |= static_cast<uint32_t>(in[inPos++]) << bitCnt;
            bitCnt += 8;
        }
        bitBuf = val >> need;
        bitCnt -= need;
        return static_cast<int>(val & ((1u << need) - 1));
    }

    int Decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= Bits(1);
            if (error) return -1;
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

// Builds a canonical Huffman decoder; returns false for an over-subscribed set.
bool Construct(Huffman* h, const short* length, int n) {
    for (int len = 0; len <= kMaxBits; ++len) h->count[len] = 0;
    for (int s = 0; s < n; ++s) h->count[length[s]]++;
    if (h->count[0] == n) return true;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return false;
    }
    short offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<short>(offs[len] + h->count[len]);
    for (int s = 0; s < n; ++s) {
        if (length[s] != 0) h->symbol[offs[length[s]]++] = static_cast<short>(s);
    }
    return true;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                             257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(State& s, const Huffman& lencode, const Huffman& distcode) {
    for (;;) {
        int symbol = s.Decode(lencode);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (s.out->size() >= s.outMax) return false;
            s.out->push_back(static_cast<uint8_t>(symbol));
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29) return false;
            int len = kLenBase[symbol] + s.Bits(kLenExtra[symbol]);
            int dsym = s.Decode(distcode);
            if (dsym < 0 || dsym >= 30) return false;
            size_t dist = static_cast<size_t>(kDistBase[dsym] + s.Bits(kDistExtra[dsym]));
            if (s.error || dist > s.out->size() || s.out->size() + len > s.outMax) return false;
            size_t from = s.out->size() - dist;
            for (int k = 0; k < len; ++k) s.out->push_back((*s.out)[from + k]);
        }
    }
}

bool Stored(State& s) {
    s.bitBuf = 0;
    s.bitCnt = 0;
    if (s.inPos + 4 > s.inLen) return false;
    unsigned len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
    unsigned nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
    s.inPos += 4;
    if (len != (~nlen & 0xFFFF)) return false;
    if (s.inPos + len > s.inLen || s.out->size() + len > s.outMax) return false;
    s.out->insert(s.out->end(), s.in + s.inPos, s.in + s.inPos + len);
    s.inPos += len;
    return true;
}

bool Fixed(State& s) {
    static Huffman lencode, distcode;
    static bool built = [] {
        short lengths[288];
        int sym = 0;
        for (; sym < 144; ++sym) lengths[sym] = 8;
        for (; sym < 256; ++sym) lengths[sym] = 9;
        for (; sym < 280; ++sym) lengths[sym] = 7;
        for (; sym < 288; ++sym) lengths[sym] = 8;
        Construct(&lencode, lengths, 288);
        for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
        Construct(&distcode, lengths, 30);
        return true;
    }();
    (void)built;
    return Codes(s, lencode, distcode);
}

bool Dynamic(State& s) {
    static const short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    int nlen = s.Bits(5) + 257;
    int ndist = s.Bits(5) + 1;
    int ncode = s.Bits(4) + 4;
    if (s.error || nlen > 286 || ndist > 30) return false;
    int index = 0;
    for (; index < ncode; ++index) lengths[kOrder[index]] = static_cast<short>(s.Bits(3));
    for (; index < 19; ++index) lengths[kOrder[index]] = 0;
    Huffman lencode, distcode;
    if (s.error || !Construct(&lencode, lengths, 19)) return false;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = s.Decode(lencode);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[index++] = static_cast<short>(symbol);
        } else {
            short len = 0;
            int repeat;
            if (symbol == 16) {
                if (index == 0) return false;
                len = lengths[index - 1];
                repeat = 3 + s.Bits(2);
            } else if (symbol == 17) {
                repeat = 3 + s.Bits(3);
            } else {
                repeat = 11 + s.Bits(7);
            }
            if (s.error || index + repeat > nlen + ndist) return false;
            while (repeat--) lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) return false;
    if (!Construct(&lencode, lengths, nlen)) return false;
    if (!Construct(&distcode, lengths + nlen, ndist)) return false;
    return Codes(s, lencode, distcode);
}

}  // namespace

bool ZlibInflate(const uint8_t* src, size_t srcLen, size_t expectedSize, std::vector<uint8_t>* out) {
    out->clear();
    if (srcLen < 2 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 0x20)) return false;
    out->reserve(expectedSize);
    State s{src + 2, srcLen - 2};
    s.out = out;
    s.outMax = expectedSize;
    int last;
    do {
        last = s.Bits(1);
        int type = s.Bits(2);
        if (s.error) return false;
        bool ok = type == 0 ? Stored(s) : type == 1 ? Fixed(s) : type == 2 ? Dynamic(s) : false;
        if (!ok || s.error) return false;
    } while (!last);
    return out->size() == expectedSize;
}

}  // namespace spf
