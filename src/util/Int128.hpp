#pragma once

// GCC and Clang have it; __extension__ keeps -Wpedantic quiet (as camera/FramePhaseLock.hpp does).
namespace apertar {
__extension__ typedef __int128 Int128;
}
