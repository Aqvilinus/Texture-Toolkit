#pragma once

// The one place a version number is written in the sources. The log line, the on-screen banner
// and the release tag drifted apart when each was edited on its own.
#define TT_VERSION "1.1.1"

// Which build a log came from. A 32-bit game needs the x86 one, and the usual first question about
// a report that says nothing happened is which of the two was in the folder.
#if defined(_WIN64)
#define TT_ARCH "x64"
#else
#define TT_ARCH "x86"
#endif
