#pragma once

// The one place the version is written. It reaches the log, the startup banner and the release
// artifacts from here. It used to be spelled out separately in the banner as well, which is how a
// build stamped 1.1.0 in its log could still tell the player on screen that it was 1.0.0.
#define TT_VERSION_STRING "1.1.0"
