// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>

namespace pxa_video {
// A valid AVC VUI bounds the decoded picture buffer, including reordering.
// Keep the codec's conservative allocation when the bound is absent/invalid.
inline int threaded_picture_count(int references,unsigned reorder,unsigned buffering,
    bool restriction,int threads,int maximum=16) {
    int count=maximum+threads;
    if(restriction && references>=0 && unsigned(references)<=buffering &&
        reorder<=buffering && buffering<=unsigned(maximum))
        // The library also retains an output queue independently of its
        // reference lists, plus in-flight workers and returned picture views.
        count=std::min(count,int(buffering+reorder)+threads+2);
    return count;
}
}
