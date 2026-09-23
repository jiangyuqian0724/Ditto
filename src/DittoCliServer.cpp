#include "stdafx.h"
#include "DittoCliServer.h"
#include "..\\Shared\\DittoCliProtocol.h"
#include "Misc.h"

#include <algorithm>
#include <cstdint>
#include <new>
#include <vector>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

namespace
{
	bool ReadExact(HANDLE handle, void* buffer, DWORD bytes)
	{
		auto* out = static_cast<unsigned char*>(buffer);
		DWORD offset = 0;
		while (offset < bytes)
		{
			DWORD read = 0;
			if (!::ReadFile(handle, out + offset, bytes - offset, &read, nullptr) || read == 0)
			{
				return false;
			}
			offset += read;
		}
		return true;
	}

	bool WriteExact(HANDLE handle, const void* buffer, DWORD bytes)
	{
		const auto* in = static_cast<const unsigned char*>(buffer);
		DWORD offset = 0;
		while (offset < bytes)
		{
			DWORD written = 0;
			if (!::WriteFile(handle, in + offset, bytes - offset, &written, nullptr) || written == 0)
			{
				return false;
			}
			offset += written;
		}
		return true;
	}

	bool Utf8ToWide(const std::string& input, std::wstring& output)
	{
		output.clear();
		if (input.empty())
		{
			return true;
		}

		if (input.size() > static_cast<size_t>(INT_MAX))
		{
			return false;
		}

		const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
			static_cast<int>(input.size()), nullptr, 0);
		if (needed <= 0)
		{
			return false;
		}

		output.resize(static_cast<size_t>(needed));
		return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
			static_cast<int>(input.size()), output.data(), needed) == needed;
	}

	bool WideToUtf8(const wchar_t* input, int length, std::string& output)
	{
		output.clear();
		if (input == nullptr || length <= 0)
		{
			return true;
		}

		const int needed = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, length,
			nullptr, 0, nullptr, nullptr);
		if (needed <= 0)
		{
			return false;
		}

		output.resize(static_cast<size_t>(needed));
		return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, length,
			output.data(), needed, nullptr, nullptr) == needed;
	}

	std::string WindowsError(const char* where)
	{
		return std::string(where) + " failed, error=" + std::to_string(::GetLastError());
	}

	void SendResponse(HANDLE pipe, DittoCliProtocol::Status status, const std::string& payload)
	{
		DittoCliProtocol::ResponseHeader response{};
		response.magic = DittoCliProtocol::Magic;
		response.version = DittoCliProtocol::Version;
		response.status = static_cast<std::int32_t>(status);
		response.payloadBytes = static_cast<std::uint32_t>(payload.size());

		if (!WriteExact(pipe, &response, sizeof(response)))
		{
			return;
		}

		if (!payload.empty())
		{
			WriteExact(pipe, payload.data(), static_cast<DWORD>(payload.size()));
		}
	}
}

CDittoCliServer::CDittoCliServer() :
	m_notifyWindow(nullptr),
	m_stop(false)
{
}

CDittoCliServer::~CDittoCliServer()
{
	Stop();
}

bool CDittoCliServer::Start(HWND notifyWindow)
{
	if (m_thread.joinable())
	{
		return true;
	}

	m_notifyWindow = notifyWindow;
	m_stop = false;

	try
	{
		m_thread = std::thread(&CDittoCliServer::Run, this);
	}
	catch (...)
	{
		Log(_T("Ditto CLI server failed to create worker thread"));
		return false;
	}

	return true;
}

void CDittoCliServer::Stop()
{
	m_stop = true;

	if (!m_thread.joinable())
	{
		return;
	}

	// Cancel a blocking ConnectNamedPipe/ReadFile first. The dummy connection is a
	// second wake-up path for Windows versions where cancellation races with accept.
	::CancelSynchronousIo(static_cast<HANDLE>(m_thread.native_handle()));

	HANDLE wake = ::CreateFileW(DittoCliProtocol::PipeName, GENERIC_READ | GENERIC_WRITE,
		0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (wake != INVALID_HANDLE_VALUE)
	{
		::CloseHandle(wake);
	}

	m_thread.join();
	m_notifyWindow = nullptr;
}

void CDittoCliServer::Run()
{
	Log(_T("Ditto CLI named-pipe server starting"));

	while (!m_stop)
	{
		HANDLE pipe = ::CreateNamedPipeW(
			DittoCliProtocol::PipeName,
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1,
			64 * 1024,
			64 * 1024,
			0,
			nullptr);

		if (pipe == INVALID_HANDLE_VALUE)
		{
			Log(StrF(_T("Ditto CLI CreateNamedPipe failed, error: %d"), ::GetLastError()));
			break;
		}

		BOOL connected = ::ConnectNamedPipe(pipe, nullptr);
		if (!connected)
		{
			const DWORD error = ::GetLastError();
			connected = error == ERROR_PIPE_CONNECTED;
			if (!connected && error != ERROR_OPERATION_ABORTED && !m_stop)
			{
				Log(StrF(_T("Ditto CLI ConnectNamedPipe failed, error: %d"), error));
			}
		}

		if (connected && !m_stop)
		{
			HandleClient(pipe);
		}

		::FlushFileBuffers(pipe);
		::DisconnectNamedPipe(pipe);
		::CloseHandle(pipe);
	}

	Log(_T("Ditto CLI named-pipe server stopped"));
}

void CDittoCliServer::HandleClient(HANDLE pipe)
{
	DittoCliProtocol::RequestHeader request{};
	if (!ReadExact(pipe, &request, sizeof(request)))
	{
		return;
	}

	if (request.magic != DittoCliProtocol::Magic ||
		request.version != DittoCliProtocol::Version ||
		request.payloadBytes > DittoCliProtocol::MaxPayloadBytes)
	{
		SendResponse(pipe, DittoCliProtocol::Status::InvalidRequest, "invalid Ditto CLI request");
		return;
	}

	std::string payload(request.payloadBytes, '\0');
	if (request.payloadBytes > 0 &&
		!ReadExact(pipe, payload.data(), request.payloadBytes))
	{
		return;
	}

	const auto command = static_cast<DittoCliProtocol::Command>(request.command);
	if (command == DittoCliProtocol::Command::Ping)
	{
		SendResponse(pipe, DittoCliProtocol::Status::Ok, "OK");
		return;
	}

	if (command == DittoCliProtocol::Command::SetText)
	{
		std::string error;
		if (!SetClipboardText(payload, error))
		{
			SendResponse(pipe, DittoCliProtocol::Status::ClipboardUnavailable, error);
			return;
		}

		std::wstring wide;
		if (Utf8ToWide(payload, wide))
		{
			PostReceivedNotification(wide);
		}

		SendResponse(pipe, DittoCliProtocol::Status::Ok, std::string());
		return;
	}

	if (command == DittoCliProtocol::Command::GetText)
	{
		if (!payload.empty())
		{
			SendResponse(pipe, DittoCliProtocol::Status::InvalidRequest, "get does not accept a payload");
			return;
		}

		std::string text;
		std::string error;
		if (!GetClipboardText(text, error))
		{
			SendResponse(pipe, DittoCliProtocol::Status::UnsupportedClipboardFormat, error);
			return;
		}

		if (text.size() > DittoCliProtocol::MaxPayloadBytes)
		{
			SendResponse(pipe, DittoCliProtocol::Status::TooLarge, "clipboard text exceeds CLI payload limit");
			return;
		}

		SendResponse(pipe, DittoCliProtocol::Status::Ok, text);
		return;
	}

	SendResponse(pipe, DittoCliProtocol::Status::InvalidRequest, "unknown Ditto CLI command");
}

bool CDittoCliServer::OpenClipboardWithRetry() const
{
	for (int attempt = 0; attempt < 50 && !m_stop; ++attempt)
	{
		if (::OpenClipboard(m_notifyWindow))
		{
			return true;
		}
		::Sleep(10);
	}
	return false;
}

bool CDittoCliServer::SetClipboardText(const std::string& utf8Text, std::string& error)
{
	std::wstring wide;
	if (!Utf8ToWide(utf8Text, wide))
	{
		error = "input is not valid UTF-8";
		return false;
	}

	if (!OpenClipboardWithRetry())
	{
		error = WindowsError("OpenClipboard");
		return false;
	}

	if (!::EmptyClipboard())
	{
		error = WindowsError("EmptyClipboard");
		::CloseClipboard();
		return false;
	}

	const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
	HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (memory == nullptr)
	{
		error = WindowsError("GlobalAlloc");
		::CloseClipboard();
		return false;
	}

	void* data = ::GlobalLock(memory);
	if (data == nullptr)
	{
		error = WindowsError("GlobalLock");
		::GlobalFree(memory);
		::CloseClipboard();
		return false;
	}

	if (!wide.empty())
	{
		::memcpy(data, wide.data(), wide.size() * sizeof(wchar_t));
	}
	static_cast<wchar_t*>(data)[wide.size()] = L'\0';
	::GlobalUnlock(memory);

	if (::SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
	{
		error = WindowsError("SetClipboardData");
		::GlobalFree(memory);
		::CloseClipboard();
		return false;
	}

	// Ownership of memory transfers to the system after SetClipboardData succeeds.
	::CloseClipboard();
	return true;
}

bool CDittoCliServer::GetClipboardText(std::string& utf8Text, std::string& error)
{
	utf8Text.clear();

	if (!OpenClipboardWithRetry())
	{
		error = WindowsError("OpenClipboard");
		return false;
	}

	HANDLE dataHandle = ::GetClipboardData(CF_UNICODETEXT);
	if (dataHandle != nullptr)
	{
		const wchar_t* data = static_cast<const wchar_t*>(::GlobalLock(dataHandle));
		if (data == nullptr)
		{
			error = WindowsError("GlobalLock");
			::CloseClipboard();
			return false;
		}

		const size_t length = ::wcslen(data);
		const bool converted = length <= static_cast<size_t>(INT_MAX) &&
			WideToUtf8(data, static_cast<int>(length), utf8Text);
		::GlobalUnlock(dataHandle);
		::CloseClipboard();

		if (!converted)
		{
			error = "failed to encode clipboard text as UTF-8";
			return false;
		}
		return true;
	}

	dataHandle = ::GetClipboardData(CF_TEXT);
	if (dataHandle != nullptr)
	{
		const char* data = static_cast<const char*>(::GlobalLock(dataHandle));
		if (data == nullptr)
		{
			error = WindowsError("GlobalLock");
			::CloseClipboard();
			return false;
		}

		const int sourceLength = static_cast<int>(std::min<size_t>(::strlen(data), static_cast<size_t>(INT_MAX)));
		const int wideLength = ::MultiByteToWideChar(CP_ACP, 0, data, sourceLength, nullptr, 0);
		std::wstring wide;
		if (wideLength > 0)
		{
			wide.resize(static_cast<size_t>(wideLength));
			::MultiByteToWideChar(CP_ACP, 0, data, sourceLength, wide.data(), wideLength);
		}

		::GlobalUnlock(dataHandle);
		::CloseClipboard();

		if (wideLength < 0 || !WideToUtf8(wide.data(), wideLength, utf8Text))
		{
			error = "failed to convert ANSI clipboard text";
			return false;
		}
		return true;
	}

	::CloseClipboard();
	error = "clipboard does not contain text";
	return false;
}

void CDittoCliServer::PostReceivedNotification(const std::wstring& text) const
{
	if (m_notifyWindow == nullptr)
	{
		return;
	}

	auto* copy = new (std::nothrow) std::wstring(text);
	if (copy == nullptr)
	{
		return;
	}

	if (!::PostMessageW(m_notifyWindow, WM_DITTO_CLI_NOTIFY, 0, reinterpret_cast<LPARAM>(copy)))
	{
		delete copy;
	}
}
