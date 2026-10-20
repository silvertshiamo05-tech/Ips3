// The few settings the adapted kernel code reads. UnleashedRecomp has a full config system;
// here they are fixed for now.
#pragma once

#include <cstdint>

enum class ELanguage : uint32_t
{
	English = 1,
	Japanese = 2,
	German = 3,
	French = 4,
	Spanish = 5,
	Italian = 6,
};

template <typename T>
struct ConfigValue
{
	T Value;
	operator T() const { return Value; }
	bool operator==(T other) const { return Value == other; }
};

namespace Config
{
	inline ConfigValue<ELanguage> Language{ELanguage::English};
	inline ConfigValue<bool> Vibration{true};
}
