#include "disc_alignment.h"
#include <cassert>

int main() {
    using WiiCompiled::Disc::AlignmentPaddingLength;
    constexpr auto noNext = UINT64_MAX;
    assert(AlignmentPaddingLength(65, 65, noNext, 32, 32) == 31);
    assert(AlignmentPaddingLength(66, 65, noNext, 32, 32) == 30);
    assert(AlignmentPaddingLength(65, 65, noNext, 2, 32) == 2);
    assert(AlignmentPaddingLength(65, 65, 70, 32, 32) == 5);
    assert(AlignmentPaddingLength(70, 65, 70, 32, 32) == 0);
    assert(AlignmentPaddingLength(64, 64, noNext, 32, 32) == 0);
    assert(AlignmentPaddingLength(96, 65, noNext, 32, 32) == 0);
    assert(AlignmentPaddingLength(64, 65, noNext, 32, 32) == 0);
    assert(AlignmentPaddingLength(65, 65, noNext, 32, 4) == 3);
    assert(AlignmentPaddingLength(68, 65, noNext, 32, 4) == 0);
    assert(AlignmentPaddingLength(noNext, noNext, noNext, 32, 32) == 0);
    assert(AlignmentPaddingLength(65, 65, noNext, 32, 0) == 0);
    assert(AlignmentPaddingLength(65, 65, noNext, 32, 3) == 0);
}
