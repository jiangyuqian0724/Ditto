#pragma once

#include <cstdint>

namespace DittoCliProtocol
{
	static const wchar_t PipeName[] = L"\\\\.\\pipe\\DittoCli-v1";
	constexpr std::uint32_t Magic = 0x4F545444; // "DTTO" in little-endian memory
	constexpr std::uint32_t Version = 1;
	constexpr std::uint32_t MaxPayloadBytes = 16u * 1024u * 1024u;

	enum class Command : std::uint32_t
	{
		SetText = 1,
		GetText = 2,
		Ping = 3,
	};

	enum class Status : std::int32_t
	{
		Ok = 0,
		InvalidRequest = 1,
		ClipboardUnavailable = 2,
		UnsupportedClipboardFormat = 3,
		TooLarge = 4,
		InternalError = 5,
	};

#pragma pack(push, 1)
	struct RequestHeader
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t command;
		std::uint32_t payloadBytes;
	};

	struct ResponseHeader
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::int32_t status;
		std::uint32_t payloadBytes;
	};
#pragma pack(pop)

	static_assert(sizeof(RequestHeader) == 16, "Unexpected Ditto CLI request header size");
	static_assert(sizeof(ResponseHeader) == 16, "Unexpected Ditto CLI response header size");
}
