#pragma once

#include <atomic>
#include <string>
#include <thread>

constexpr UINT WM_DITTO_CLI_NOTIFY = WM_APP + 230;

class CDittoCliServer
{
public:
	CDittoCliServer();
	~CDittoCliServer();

	bool Start(HWND notifyWindow);
	void Stop();

private:
	void Run();
	void HandleClient(HANDLE pipe);
	bool SetClipboardText(const std::string& utf8Text, std::string& error);
	bool GetClipboardText(std::string& utf8Text, std::string& error);
	bool OpenClipboardWithRetry() const;
	void PostReceivedNotification(const std::wstring& text) const;

	HWND m_notifyWindow;
	std::atomic<bool> m_stop;
	std::thread m_thread;
};
