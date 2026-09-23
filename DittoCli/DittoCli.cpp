#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "..\\Shared\\DittoCliProtocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	[[noreturn]] void Fail(const std::string& message)
	{
		throw std::runtime_error(message);
	}

	std::string WideToUtf8(const wchar_t* input)
	{
		if (input == nullptr || *input == L'\0')
		{
			return {};
		}

		const int length = static_cast<int>(wcslen(input));
		const int needed = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, length,
			nullptr, 0, nullptr, nullptr);
		if (needed <= 0)
		{
			Fail("failed to convert command line to UTF-8");
		}

		std::string output(static_cast<size_t>(needed), '\0');
		if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, length,
			output.data(), needed, nullptr, nullptr) != needed)
		{
			Fail("failed to convert command line to UTF-8");
		}
		return output;
	}

	std::string ReadStdin()
	{
		if (_setmode(_fileno(stdin), _O_BINARY) == -1)
		{
			Fail("failed to set stdin to binary mode");
		}

		std::string output;
		char buffer[8192];
		while (std::cin.good())
		{
			std::cin.read(buffer, sizeof(buffer));
			const std::streamsize count = std::cin.gcount();
			if (count > 0)
			{
				output.append(buffer, static_cast<size_t>(count));
			}
		}
		return output;
	}

	std::string JoinArgs(const std::vector<std::string>& args, size_t start)
	{
		std::string output;
		for (size_t i = start; i < args.size(); ++i)
		{
			if (i != start)
			{
				output.push_back(' ');
			}
			output += args[i];
		}
		return output;
	}

	bool ReadExact(HANDLE handle, void* buffer, DWORD bytes)
	{
		auto* output = static_cast<unsigned char*>(buffer);
		DWORD offset = 0;
		while (offset < bytes)
		{
			DWORD read = 0;
			if (!::ReadFile(handle, output + offset, bytes - offset, &read, nullptr) || read == 0)
			{
				return false;
			}
			offset += read;
		}
		return true;
	}

	bool WriteExact(HANDLE handle, const void* buffer, DWORD bytes)
	{
		const auto* input = static_cast<const unsigned char*>(buffer);
		DWORD offset = 0;
		while (offset < bytes)
		{
			DWORD written = 0;
			if (!::WriteFile(handle, input + offset, bytes - offset, &written, nullptr) || written == 0)
			{
				return false;
			}
			offset += written;
		}
		return true;
	}

	HANDLE ConnectPipe()
	{
		for (int attempt = 0; attempt < 40; ++attempt)
		{
			HANDLE pipe = ::CreateFileW(DittoCliProtocol::PipeName,
				GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			if (pipe != INVALID_HANDLE_VALUE)
			{
				return pipe;
			}

			const DWORD error = ::GetLastError();
			if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY)
			{
				Fail("failed to connect to Ditto CLI pipe, error=" + std::to_string(error));
			}

			if (error == ERROR_PIPE_BUSY)
			{
				::WaitNamedPipeW(DittoCliProtocol::PipeName, 100);
			}
			else
			{
				::Sleep(50);
			}
		}

		Fail("Ditto is not running, or this build does not contain the CLI server");
	}

	std::string Request(DittoCliProtocol::Command command, const std::string& payload)
	{
		if (payload.size() > DittoCliProtocol::MaxPayloadBytes)
		{
			Fail("payload exceeds the 16 MiB CLI limit");
		}

		HANDLE pipe = ConnectPipe();
		try
		{
			DittoCliProtocol::RequestHeader request{};
			request.magic = DittoCliProtocol::Magic;
			request.version = DittoCliProtocol::Version;
			request.command = static_cast<std::uint32_t>(command);
			request.payloadBytes = static_cast<std::uint32_t>(payload.size());

			if (!WriteExact(pipe, &request, sizeof(request)) ||
				(!payload.empty() && !WriteExact(pipe, payload.data(), static_cast<DWORD>(payload.size()))))
			{
				Fail("failed to send request to Ditto");
			}

			DittoCliProtocol::ResponseHeader response{};
			if (!ReadExact(pipe, &response, sizeof(response)))
			{
				Fail("Ditto closed the CLI connection without a response");
			}

			if (response.magic != DittoCliProtocol::Magic ||
				response.version != DittoCliProtocol::Version ||
				response.payloadBytes > DittoCliProtocol::MaxPayloadBytes)
			{
				Fail("Ditto returned an invalid CLI response");
			}

			std::string responsePayload(response.payloadBytes, '\0');
			if (response.payloadBytes > 0 &&
				!ReadExact(pipe, responsePayload.data(), response.payloadBytes))
			{
				Fail("failed to read Ditto CLI response");
			}

			::CloseHandle(pipe);
			pipe = INVALID_HANDLE_VALUE;

			if (response.status != static_cast<std::int32_t>(DittoCliProtocol::Status::Ok))
			{
				if (responsePayload.empty())
				responsePayload = "Ditto CLI request failed";
				Fail(responsePayload);
			}

			return responsePayload;
		}
		catch (...)
		{
			if (pipe != INVALID_HANDLE_VALUE)
				::CloseHandle(pipe);
			throw;
		}
	}

	void WriteStdoutUtf8(const std::string& value)
	{
		if (_setmode(_fileno(stdout), _O_BINARY) == -1)
		{
			Fail("failed to set stdout to binary mode");
		}

		if (!value.empty() && std::fwrite(value.data(), 1, value.size(), stdout) != value.size())
		{
			Fail("failed to write stdout");
		}
		std::fflush(stdout);
	}

	void Usage(const wchar_t* program)
	{
		std::fwprintf(stderr,
			L"Usage:\n"
			L"  %ls set TEXT...\n"
			L"  %ls set -        # read UTF-8 text from stdin\n"
			L"  %ls get          # write current Windows clipboard text as UTF-8\n"
			L"  %ls ping\n",
			program, program, program, program);
	}
}

int wmain(int argc, wchar_t** argv)
{
	try
	{
		if (argc < 2)
		{
			Usage(argv[0]);
			return 2;
		}

		std::vector<std::string> args;
		args.reserve(static_cast<size_t>(argc));
		for (int i = 0; i < argc; ++i)
		{
			args.push_back(WideToUtf8(argv[i]));
		}

		if (args[1] == "get" && argc == 2)
		{
			WriteStdoutUtf8(Request(DittoCliProtocol::Command::GetText, {}));
			return 0;
		}

		if (args[1] == "ping" && argc == 2)
		{
			const std::string response = Request(DittoCliProtocol::Command::Ping, {});
			WriteStdoutUtf8(response + "\n");
			return 0;
		}

		if (args[1] == "set" && argc >= 3)
		{
			const std::string text = (argc == 3 && args[2] == "-") ? ReadStdin() : JoinArgs(args, 2);
			Request(DittoCliProtocol::Command::SetText, text);
			return 0;
		}

		Usage(argv[0]);
		return 2;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "DittoCli: %s\n", e.what());
		return 1;
	}
}
