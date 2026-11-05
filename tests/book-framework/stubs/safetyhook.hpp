#pragma once

// Exercise the production call's explicit argument types against a native
// function pointer without installing a detour or loading Skyrim.
struct SafetyHookInline
{
	void* original = nullptr;
	explicit operator bool() const { return original != nullptr; }

	template <class Return, class... Args>
	Return call(Args... args) const
	{
		return reinterpret_cast<Return (*)(Args...)>(original)(args...);
	}
};

namespace safetyhook
{
	inline void* installedThunk = nullptr;
	inline SafetyHookInline create_inline(void* target, void* thunk)
	{
		installedThunk = thunk;
		return { target };
	}
}
