// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Optional read-only device query. Does not create frame buffers or run kernels.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(void) {
    @autoreleasepool {
        id<MTLDevice> d=MTLCreateSystemDefaultDevice();
        if (!d) { fprintf(stderr,"Metal device unavailable\n"); return 1; }
        printf("{\"device\":\"%s\",\"max_buffer_length\":%llu,"
               "\"recommended_working_set_bytes\":%llu,\"current_allocated_bytes\":%llu,"
               "\"physical_memory_bytes\":%llu}\n",
               d.name.UTF8String,(unsigned long long)d.maxBufferLength,
               (unsigned long long)d.recommendedMaxWorkingSetSize,
               (unsigned long long)d.currentAllocatedSize,
               (unsigned long long)NSProcessInfo.processInfo.physicalMemory);
    }
}
