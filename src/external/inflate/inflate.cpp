#include "inflate.h"
#include <vector>

namespace QC {
namespace {

struct BitReader {
    const uint8_t* p;
    size_t len;
    size_t pos = 0;   // byte index
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool error = false;

    BitReader(const uint8_t* d, size_t l) : p(d), len(l) {}

    int getbit() {
        if (bitcnt == 0) {
            if (pos >= len) { error = true; return 0; }
            bitbuf = p[pos++];
            bitcnt = 8;
        }
        int b = bitbuf & 1;
        bitbuf >>= 1;
        --bitcnt;
        return b;
    }

    // Read n bits, LSB first.
    uint32_t getbits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint32_t>(getbit()) << i;
        return v;
    }

    void align_to_byte() { bitcnt = 0; }
};

// Canonical Huffman table: decode by walking code lengths.
struct Huffman {
    int counts[16] = {0};   // counts[len] = number of codes of that length
    std::vector<int> symbols;

    void build(const std::vector<int>& lengths) {
        for (int i = 0; i < 16; ++i) counts[i] = 0;
        for (int len : lengths) counts[len]++;
        counts[0] = 0;
        symbols.assign(lengths.size(), 0);
        int offsets[16];
        offsets[0] = 0; offsets[1] = 0;
        for (int i = 1; i < 15; ++i) offsets[i + 1] = offsets[i] + counts[i];
        for (size_t sym = 0; sym < lengths.size(); ++sym) {
            if (lengths[sym] != 0) symbols[offsets[lengths[sym]]++] = static_cast<int>(sym);
        }
    }

    // Decode one symbol from the bit stream.
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= br.getbit();
            int count = counts[len];
            if (code - first < count) return symbols[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;  // invalid
    }
};

// Length/distance base tables (RFC 1951).
const int kLenBase[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const int kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const int kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const int kDistExtra[30]= {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

bool inflate_block_data(BitReader& br, const Huffman& lit, const Huffman& dist, ByteBuffer& out) {
    for (;;) {
        int sym = lit.decode(br);
        if (br.error || sym < 0) return false;
        if (sym == 256) return true;            // end of block
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int length = kLenBase[sym] + static_cast<int>(br.getbits(kLenExtra[sym]));
            int dsym = dist.decode(br);
            if (br.error || dsym < 0 || dsym >= 30) return false;
            int distance = kDistBase[dsym] + static_cast<int>(br.getbits(kDistExtra[dsym]));
            if (static_cast<size_t>(distance) > out.size()) return false;
            size_t start = out.size() - distance;
            for (int i = 0; i < length; ++i) out.push_back(out[start + i]);
        }
    }
}

bool build_fixed(Huffman& lit, Huffman& dist) {
    std::vector<int> ll(288);
    for (int i = 0; i < 144; ++i) ll[i] = 8;
    for (int i = 144; i < 256; ++i) ll[i] = 9;
    for (int i = 256; i < 280; ++i) ll[i] = 7;
    for (int i = 280; i < 288; ++i) ll[i] = 8;
    lit.build(ll);
    std::vector<int> dl(30, 5);
    dist.build(dl);
    return true;
}

bool build_dynamic(BitReader& br, Huffman& lit, Huffman& dist) {
    int hlit = static_cast<int>(br.getbits(5)) + 257;
    int hdist = static_cast<int>(br.getbits(5)) + 1;
    int hclen = static_cast<int>(br.getbits(4)) + 4;
    static const int kOrder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    std::vector<int> cl(19, 0);
    for (int i = 0; i < hclen; ++i) cl[kOrder[i]] = static_cast<int>(br.getbits(3));
    Huffman clh; clh.build(cl);

    std::vector<int> lengths;
    lengths.reserve(hlit + hdist);
    while (static_cast<int>(lengths.size()) < hlit + hdist) {
        int sym = clh.decode(br);
        if (br.error || sym < 0) return false;
        if (sym < 16) {
            lengths.push_back(sym);
        } else if (sym == 16) {
            if (lengths.empty()) return false;
            int rep = 3 + static_cast<int>(br.getbits(2));
            int prev = lengths.back();
            for (int i = 0; i < rep; ++i) lengths.push_back(prev);
        } else if (sym == 17) {
            int rep = 3 + static_cast<int>(br.getbits(3));
            for (int i = 0; i < rep; ++i) lengths.push_back(0);
        } else { // 18
            int rep = 11 + static_cast<int>(br.getbits(7));
            for (int i = 0; i < rep; ++i) lengths.push_back(0);
        }
    }
    if (static_cast<int>(lengths.size()) != hlit + hdist) return false;
    std::vector<int> ll(lengths.begin(), lengths.begin() + hlit);
    std::vector<int> dl(lengths.begin() + hlit, lengths.end());
    lit.build(ll);
    dist.build(dl);
    return true;
}

} // namespace

bool raw_inflate(const uint8_t* data, size_t len, ByteBuffer& out) {
    out.clear();
    BitReader br(data, len);
    for (;;) {
        int bfinal = br.getbit();
        int btype = static_cast<int>(br.getbits(2));
        if (br.error) return false;
        if (btype == 0) {
            br.align_to_byte();
            if (br.pos + 4 > br.len) return false;
            uint16_t l = static_cast<uint16_t>(br.p[br.pos]) | (static_cast<uint16_t>(br.p[br.pos + 1]) << 8);
            br.pos += 4;  // skip LEN + NLEN
            if (br.pos + l > br.len) return false;
            out.insert(out.end(), br.p + br.pos, br.p + br.pos + l);
            br.pos += l;
        } else if (btype == 1) {
            Huffman lit, dist; build_fixed(lit, dist);
            if (!inflate_block_data(br, lit, dist, out)) return false;
        } else if (btype == 2) {
            Huffman lit, dist;
            if (!build_dynamic(br, lit, dist)) return false;
            if (!inflate_block_data(br, lit, dist, out)) return false;
        } else {
            return false;  // reserved
        }
        if (bfinal) break;
    }
    return true;
}

bool zlib_inflate(const uint8_t* data, size_t len, ByteBuffer& out) {
    if (len < 2) return false;
    // RFC 1950: CMF, FLG. Low nibble of CMF must be 8 (deflate). Skip 2-byte header
    // (and a 4-byte preset dictionary if FDICT set). Trailing adler32 is ignored.
    if ((data[0] & 0x0F) != 8) return false;
    size_t offset = 2;
    if (data[1] & 0x20) offset += 4;  // FDICT
    if (offset >= len) return false;
    return raw_inflate(data + offset, len - offset, out);
}

} // namespace QC
