// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Isolated original-source replay; never linked into HyperCine libraries.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "original_descale.inc"

struct AlignedDelete {
    void operator()(unsigned char* p) const { ::operator delete(p, std::align_val_t(32)); }
};
struct Buffer {
    size_t size;
    std::unique_ptr<unsigned char, AlignedDelete> bytes;
    explicit Buffer(size_t n) : size(n), bytes(static_cast<unsigned char*>(
        ::operator new(n + 64, std::align_val_t(32)))) {
        std::memset(bytes.get(), 0xa5, n + 64);
    }
    PIXEL* data() { return reinterpret_cast<PIXEL*>(bytes.get() + 32); }
    bool guards() const {
        return std::all_of(bytes.get(), bytes.get()+32, [](unsigned char x){return x==0xa5;}) &&
            std::all_of(bytes.get()+32+size, bytes.get()+64+size, [](unsigned char x){return x==0xa5;});
    }
};
int main() {
    try {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
#endif
        uint32_t shape[2];
        if (std::fread(shape, sizeof(shape), 1, stdin)!=1) return 1;
        const auto w=shape[0], h=shape[1];
        if (!((w==240 && h==270)||(w==480 && h==270)||
              (w==480 && h==540)||(w==960 && h==540))) return 1;
        const size_t n=size_t(w)*h*2;
        std::array<Buffer,4> inputs{Buffer(n),Buffer(n),Buffer(n),Buffer(n)};
        std::array<std::vector<unsigned char>,4> before;
        for (size_t i=0;i<4;++i) {
            if (std::fread(inputs[i].data(),1,n,stdin)!=n) return 1;
            auto p=reinterpret_cast<unsigned char*>(inputs[i].data());
            before[i].assign(p,p+n);
        }
        if (std::fgetc(stdin)!=EOF) return 1;
        Buffer output(n*4), scratch(size_t(w)*2*9);
        int q[4]={1,1,1,1};
        InvertSpatialQuantDescale16s(inputs[0].data(),w*2, inputs[1].data(),w*2,
            inputs[2].data(),w*2, inputs[3].data(),w*2, output.data(),w*4,
            ROI{int(w),int(h)}, scratch.data(),scratch.size,2,q);
        for (size_t i=0;i<4;++i)
            if (!inputs[i].guards() || std::memcmp(inputs[i].data(),before[i].data(),n)) return 1;
        if (!output.guards() || !scratch.guards()) return 1;
        return std::fwrite(output.data(),1,n*4,stdout)==n*4 ? 0 : 1;
    } catch (...) { return 1; }
}
