// microReticulum, by Chad Attermann, Apache-2.0 (lib/microReticulum/LICENSE).
// Changed for RTNode (https://github.com/jrl290/RTNode-HeltecV4); the changes
// are in RTNode's git history.

#pragma once

#include "Type.h"
#include <memory>

namespace RNS {
	// Stub — channel support removed. Retained for LinkData.h compatibility.
	class Channel {
	public:
		Channel(Type::NoneConstructor) {}
		Channel(const Channel&) = default;
		Channel& operator=(const Channel&) = default;
		operator bool() const { return false; }
		bool operator<(const Channel&) const { return false; }
		void _shutdown() {}
	};
	class ChannelAdvertisement {};
}
