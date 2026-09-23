#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace RE
{
	using FormID = std::uint32_t;
	struct BSString
	{
		using size_type = std::uint16_t;
		inline static bool failReplacement = false;
		inline static int defaultConstructions = 0;
		inline static int valueConstructions = 0;
		inline static int moveAssignments = 0;
		inline static int liveInstances = 0;
		std::string text;
		BSString()
		{
			++defaultConstructions;
			++liveInstances;
		}
		explicit BSString(std::string_view value) : text(value)
		{
			++valueConstructions;
			if (failReplacement) {
				throw std::runtime_error("replacement allocation failed");
			}
			++liveInstances;
		}
		BSString& operator=(BSString&& other)
		{
			++moveAssignments;
			text = std::move(other.text);
			return *this;
		}
		~BSString() { --liveInstances; }
	};
	struct BSFixedString
	{
		std::string text;
		explicit BSFixedString(const char* value) : text(value) {}
		const char* c_str() const { return text.c_str(); }
	};
	struct ExtraDataList
	{};
	struct TESObjectREFR
	{};
	struct TESObjectBOOK
	{
		FormID GetFormID() const { return 0x42; }
		const char* GetName() const { return "Test book"; }
	};
	struct NiPoint3
	{};
	struct NiMatrix3
	{};
	struct NiAVObject
	{};
	struct StaticFunctionTag
	{};
	namespace BSScript
	{
		struct IVirtualMachine
		{
			template <class Fn>
			void RegisterFunction(const char*, const char*, Fn)
			{}
		};
	}
}

namespace REL
{
	inline bool vr = false;
	inline std::uintptr_t target = 0;
	struct Module
	{
		static bool IsVR() { return vr; }
	};
	struct RelocationID
	{
		RelocationID(int, int) {}
	};
	template <class T>
	struct Relocation
	{
		explicit Relocation(RelocationID) {}
		T address() const { return static_cast<T>(target); }
	};
}

namespace spdlog
{
	inline bool failDebug = false;
	namespace level
	{
		enum level_enum
		{
			debug
		};
	}
	struct logger
	{
		bool should_log(level::level_enum) const { return failDebug; }
	};
	inline logger* default_logger_raw()
	{
		static logger instance;
		return &instance;
	}
	template <class... Args>
	void debug(Args...)
	{
		if (failDebug) {
			throw std::runtime_error("debug logging failed");
		}
	}
	template <class... Args>
	void info(Args...)
	{}
	template <class... Args>
	void warn(Args...)
	{}
	template <class... Args>
	void error(Args...)
	{}
}

// Compile the production implementation; only its game and detour services
// are replaced so both native signatures can run in a standalone process.
#include "../../src/Core/BookFramework.cpp"

namespace
{
	using FlatOpen = void (*)(const RE::BSString&, const RE::ExtraDataList*, RE::TESObjectREFR*,
		RE::TESObjectBOOK*, const RE::NiPoint3&, const RE::NiMatrix3&, float, bool);
	using VrOpen = void (*)(const RE::BSString&, const RE::ExtraDataList*, RE::TESObjectREFR*,
		RE::TESObjectBOOK*, const RE::NiPoint3&, const RE::NiMatrix3&, float, bool, RE::NiAVObject*);
	using namespace Lodestone::Core::BookFramework;
	static_assert(std::is_same_v<decltype(&OpenBookMenuHook::thunk<>), FlatOpen>);
	static_assert(std::is_same_v<decltype(&OpenBookMenuHook::thunk<RE::NiAVObject*>), VrOpen>);

	RE::ExtraDataList extra;
	RE::TESObjectREFR reference;
	RE::TESObjectBOOK book;
	RE::NiPoint3 position;
	RE::NiMatrix3 rotation;
	RE::NiAVObject scene;
	const RE::BSString description{ "original" };
	const RE::ExtraDataList* expectedExtra = nullptr;
	RE::TESObjectREFR* expectedReference = nullptr;
	RE::TESObjectBOOK* expectedBook = nullptr;
	RE::NiAVObject* expectedScene = nullptr;
	std::string expectedText;
	bool expectedDefaultPosition = false;
	bool expectOriginalDescription = true;
	int expectedLiveInstances = 0;
	int calls = 0;
	int cases = 0;

	enum class TextCase
	{
		Passthrough,
		Replacement,
		EmptyReplacement,
		ReplacementFailure,
		NullBook,
		LoggingFailure,
		MaximumLength,
		TerminatorOverflow,
		LengthOverflow
	};

	std::string StoredText(TextCase mode)
	{
		switch (mode) {
		case TextCase::EmptyReplacement:
			return "";
		case TextCase::MaximumLength:
			return std::string(65534, 'a');
		case TextCase::TerminatorOverflow:
			return std::string(65535, 'a');
		case TextCase::LengthOverflow:
			return std::string(65536, 'a');
		default:
			return "replacement";
		}
	}

	void Require(bool condition, const char* message)
	{
		if (!condition) {
			throw std::runtime_error(message);
		}
	}

	void CheckCommon(const RE::BSString& text, const RE::ExtraDataList* extras,
		RE::TESObjectREFR* ref, RE::TESObjectBOOK* form, const RE::NiPoint3& pos,
		const RE::NiMatrix3& rot, float scale, bool defaultPosition)
	{
		++calls;
		Require(text.text == expectedText, "book text changed unexpectedly");
		Require((&text == &description) == expectOriginalDescription, "description identity changed");
		Require(extras == expectedExtra && ref == expectedReference && form == expectedBook, "form arguments changed");
		Require(&pos == &position && &rot == &rotation, "transform references changed");
		Require(scale == 0.4375f && defaultPosition == expectedDefaultPosition, "scale or bool changed");
		Require(RE::BSString::liveInstances == expectedLiveInstances, "replacement lifetime changed during original call");
	}

#if defined(_MSC_VER)
#	define BOOK_TEST_NOINLINE __declspec(noinline)
#else
#	define BOOK_TEST_NOINLINE __attribute__((noinline))
#endif
	BOOK_TEST_NOINLINE void FlatOriginal(const RE::BSString& text, const RE::ExtraDataList* extras,
		RE::TESObjectREFR* ref, RE::TESObjectBOOK* form, const RE::NiPoint3& pos,
		const RE::NiMatrix3& rot, float scale, bool defaultPosition)
	{
		CheckCommon(text, extras, ref, form, pos, rot, scale, defaultPosition);
	}

	BOOK_TEST_NOINLINE void VrOriginal(const RE::BSString& text, const RE::ExtraDataList* extras,
		RE::TESObjectREFR* ref, RE::TESObjectBOOK* form, const RE::NiPoint3& pos,
		const RE::NiMatrix3& rot, float scale, bool defaultPosition, RE::NiAVObject* source)
	{
		CheckCommon(text, extras, ref, form, pos, rot, scale, defaultPosition);
		Require(source == expectedScene, "ninth VR argument was lost");
	}

	void Run(bool vr, RE::NiAVObject* source, bool worldReference, bool defaultPosition, TextCase mode)
	{
		REL::vr = vr;
		REL::target = vr ? reinterpret_cast<std::uintptr_t>(&VrOriginal) : reinterpret_cast<std::uintptr_t>(&FlatOriginal);
		Install();
		Require(safetyhook::installedThunk == (vr ? reinterpret_cast<void*>(&OpenBookMenuHook::thunk<RE::NiAVObject*>) : reinterpret_cast<void*>(&OpenBookMenuHook::thunk<>)), "wrong installed signature");
		g_bookText.clear();
		if (mode != TextCase::Passthrough) {
			g_bookText[book.GetFormID()] = StoredText(mode);
		}
		RE::BSString::failReplacement = mode == TextCase::ReplacementFailure;
		RE::BSString::defaultConstructions = 0;
		RE::BSString::valueConstructions = 0;
		RE::BSString::moveAssignments = 0;
		const auto initialLiveInstances = RE::BSString::liveInstances;
		spdlog::failDebug = mode == TextCase::LoggingFailure;
		expectedExtra = worldReference ? &extra : nullptr;
		expectedReference = worldReference ? &reference : nullptr;
		expectedBook = mode == TextCase::NullBook ? nullptr : &book;
		expectedScene = source;
		expectedDefaultPosition = defaultPosition;
		expectOriginalDescription = mode != TextCase::Replacement && mode != TextCase::EmptyReplacement && mode != TextCase::MaximumLength;
		expectedText = expectOriginalDescription ? "original" : StoredText(mode);
		const bool constructsReplacement = mode != TextCase::Passthrough && mode != TextCase::NullBook &&
		                                   mode != TextCase::TerminatorOverflow && mode != TextCase::LengthOverflow;
		expectedLiveInstances = initialLiveInstances + (constructsReplacement && mode != TextCase::ReplacementFailure ? 1 : 0);
		calls = 0;
		if (vr) {
			reinterpret_cast<VrOpen>(safetyhook::installedThunk)(description, expectedExtra, expectedReference, expectedBook, position, rotation, 0.4375f, defaultPosition, source);
		} else {
			reinterpret_cast<FlatOpen>(safetyhook::installedThunk)(description, expectedExtra, expectedReference, expectedBook, position, rotation, 0.4375f, defaultPosition);
		}
		Require(calls == 1, "original was not called exactly once");
		Require(RE::BSString::defaultConstructions == 0, "unnecessary default game-string allocation");
		Require(RE::BSString::valueConstructions == (constructsReplacement ? 1 : 0), "unexpected replacement construction count");
		Require(RE::BSString::moveAssignments == 0, "replacement used leaking BSString move assignment");
		Require(RE::BSString::liveInstances == initialLiveInstances, "replacement survived beyond the original call");
		++cases;
	}
}

int main()
{
	try {
		for (bool defaultPosition : { false, true }) {
			for (bool worldReference : { false, true }) {
				for (auto mode : { TextCase::Passthrough, TextCase::Replacement, TextCase::EmptyReplacement,
						 TextCase::ReplacementFailure, TextCase::NullBook, TextCase::LoggingFailure,
						 TextCase::MaximumLength, TextCase::TerminatorOverflow, TextCase::LengthOverflow }) {
					Run(false, nullptr, worldReference, defaultPosition, mode);
					Run(true, nullptr, worldReference, defaultPosition, mode);
					Run(true, &scene, worldReference, defaultPosition, mode);
				}
			}
		}
		std::cout << cases << " book forwarding cases passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
