// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Diagnostic-only ordinary new counter; never linked into production.
#include "hypercine/standalone.h"
#include <cstdlib>
#include <iostream>
#include <new>
namespace {
thread_local bool counting=false;
thread_local std::size_t allocations=0,allocated_bytes=0;
}
void* operator new(std::size_t n){
    if(counting){++allocations;allocated_bytes+=n;}
    if(void* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
int main(){try{
    using namespace hypercine::standalone;
    std::cout<<"{\"ordinary_vector_allocations\":[";
    for(int dense=0;dense<2;++dense){
        Plane p(960,540);if(dense)std::fill(p.pixels.begin(),p.pixels.end(),int16_t{-32768});
        allocations=allocated_bytes=0;counting=true;
        auto bytes=PackHighpass(p,4*(p.width*p.height+1));
        counting=false;
        if(dense)std::cout<<',';
        std::cout<<"{\"dense\":"<<dense<<",\"allocations\":"<<allocations<<",\"allocated_bytes_sum\":"<<allocated_bytes<<",\"output_bytes\":"<<bytes.size()<<",\"output_capacity\":"<<bytes.capacity()<<"}";
    }
    std::cout<<"]}\n";
}catch(const std::exception& e){counting=false;std::cerr<<e.what()<<'\n';return 1;}}
