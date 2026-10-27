// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hypercine { namespace detail { namespace {
int16_t Narrow(int v) {
    return static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
int Floor8(int v) { return v >= 0 ? v/8 : -((-v+7)/8); }
// Six defined taps, including the independent left/right boundary formulas.
int HighScalar(const int16_t* p, std::ptrdiff_t step, int pair, int pairs) {
    const int a=p[0], b=p[step];
    if (pair==0)
        return Floor8(5*a-11*b+4*p[2*step]+4*p[3*step]-p[4*step]-p[5*step]+4);
    if (pair==pairs-1)
        return Floor8(11*a-5*b-4*p[-step]-4*p[-2*step]+p[-3*step]+p[-4*step]+4);
    return a-b+Floor8(-p[-2*step]-p[-step]+p[2*step]+p[3*step]+4);
}
template<class V> void Horizontal(const int16_t* src, int w, int h, int ss,
    int16_t* low, int ls, int16_t* high, int hs) {
    const int pairs=w/2;
    for (int y=0;y<h;++y) {
        const auto* p=src+static_cast<std::ptrdiff_t>(y)*ss;
        auto* l=low+static_cast<std::ptrdiff_t>(y)*ls;
        auto* q=high+static_cast<std::ptrdiff_t>(y)*hs;
        l[0]=Narrow(int(p[0])+p[1]);
        q[0]=Narrow(HighScalar(p,1,0,pairs));
        int c=1;
        // The last vector's next pair must end inside the logical row.
        for (;c+V::lanes<=pairs-1;c+=V::lanes) {
            auto center=V::Pairs(p+2*c);
            auto before=V::Pairs(p+2*c-2);
            auto after=V::Pairs(p+2*c+2);
            V::Store(l+c,center);
            V::Store(q+c,V::Add(V::Differences(p+2*c),
                V::Shift3(V::Add(V::Sub(after,before),V::Set(4)))));
        }
        for (;c<pairs;++c) {
            l[c]=Narrow(int(p[2*c])+p[2*c+1]);
            q[c]=Narrow(HighScalar(p+2*c,1,c,pairs));
        }
    }
}
template<class V> void Vertical(const int16_t* src, int w, int h, int ss,
    int16_t* low, int ls, int16_t* high, int hs) {
    const int pairs=h/2;
    for (int r=0;r<pairs;++r) {
        const auto* p=src+static_cast<std::ptrdiff_t>(2*r)*ss;
        auto* l=low+static_cast<std::ptrdiff_t>(r)*ls;
        auto* q=high+static_cast<std::ptrdiff_t>(r)*hs;
        int x=0;
        for (;x+V::lanes<=w;x+=V::lanes) {
            const auto* v=p+x;
            auto a=V::Load(v), b=V::Load(v+ss);
            V::Store(l+x,V::Add(a,b));
            auto sum=V::Set(4);
            if (r==0) {
                sum=V::Add(sum,V::Sub(V::Mul(a,5),V::Mul(b,11)));
                sum=V::Add(sum,V::Mul(V::Add(V::Load(v+2*ss),V::Load(v+3*ss)),4));
                sum=V::Sub(sum,V::Add(V::Load(v+4*ss),V::Load(v+5*ss)));
                sum=V::Shift3(sum);
            } else if (r==pairs-1) {
                sum=V::Add(sum,V::Sub(V::Mul(a,11),V::Mul(b,5)));
                sum=V::Sub(sum,V::Mul(V::Add(V::Load(v-ss),V::Load(v-2*ss)),4));
                sum=V::Add(sum,V::Add(V::Load(v-3*ss),V::Load(v-4*ss)));
                sum=V::Shift3(sum);
            } else {
                sum=V::Sub(sum,V::Add(V::Load(v-2*ss),V::Load(v-ss)));
                sum=V::Add(sum,V::Add(V::Load(v+2*ss),V::Load(v+3*ss)));
                sum=V::Add(V::Shift3(sum),V::Sub(a,b));
            }
            V::Store(q+x,sum);
        }
        for (;x<w;++x) {
            l[x]=Narrow(int(p[x])+p[ss+x]);
            q[x]=Narrow(HighScalar(p+x,ss,r,pairs));
        }
    }
}
}}} // namespace hypercine::detail::anonymous
