#pragma once

#include "httplib.h"
#include "Options.h"
#include "sqlite/CppSQLite3.h"
#include "../Shared/TextConvert.h"

#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class CDittoHttpServer
{
public:
	CDittoHttpServer()
	{
		Start();
	}

	~CDittoHttpServer()
	{
		Stop();
	}

	CDittoHttpServer(const CDittoHttpServer&) = delete;
	CDittoHttpServer& operator=(const CDittoHttpServer&) = delete;

private:
	struct HttpClip
	{
		int id = 0;
		__int64 date = 0;
		std::string text;
	};

	static std::string CStringToUtf8(const CString& value)
	{
		CStringA utf8 = CTextConvert::UnicodeToUTF8(value);
		return std::string(utf8.GetString(), utf8.GetLength());
	}

	static std::string JsonEscape(const std::string& value)
	{
		std::string out;
		out.reserve(value.size() + 16);
		for(unsigned char ch : value)
		{
			switch(ch)
			{
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if(ch < 0x20)
				{
					char buf[7] = {};
					sprintf_s(buf, "\\u%04x", ch);
					out += buf;
				}
				else
				{
					out.push_back(static_cast<char>(ch));
				}
				break;
			}
		}
		return out;
	}

	static std::string HtmlEscape(const std::string& value)
	{
		std::string out;
		out.reserve(value.size() + 32);
		for(char ch : value)
		{
			switch(ch)
			{
			case '&': out += "&amp;"; break;
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			case '"': out += "&quot;"; break;
			case '\'': out += "&#39;"; break;
			default: out.push_back(ch); break;
			}
		}
		return out;
	}

	static std::string ReadClipText(CppSQLite3DB& db, int clipId, const CString& fallback)
	{
		CppSQLite3Query data = db.execQueryEx(
			_T("SELECT strClipBoardFormat, ooData FROM Data ")
			_T("WHERE lParentID = %d AND (strClipBoardFormat = 'CF_UNICODETEXT' OR strClipBoardFormat = 'CF_TEXT') ")
			_T("ORDER BY CASE WHEN strClipBoardFormat = 'CF_UNICODETEXT' THEN 0 ELSE 1 END LIMIT 1"),
			clipId);

		if(!data.eof())
		{
			CString format = data.getStringField(_T("strClipBoardFormat"));
			int length = 0;
			const unsigned char* blob = data.getBlobField(_T("ooData"), length);
			if(blob != nullptr && length > 0)
			{
				if(format.CompareNoCase(_T("CF_UNICODETEXT")) == 0)
				{
					int wcharCount = length / static_cast<int>(sizeof(wchar_t));
					const wchar_t* text = reinterpret_cast<const wchar_t*>(blob);
					while(wcharCount > 0 && text[wcharCount - 1] == L'\0')
						--wcharCount;
					return CStringToUtf8(CString(text, wcharCount));
				}

				int charCount = length;
				const char* text = reinterpret_cast<const char*>(blob);
				while(charCount > 0 && text[charCount - 1] == '\0')
					--charCount;
				return CStringToUtf8(CTextConvert::AnsiToUnicode(CStringA(text, charCount)));
			}
		}

		return CStringToUtf8(fallback);
	}

	static std::vector<HttpClip> LoadRecentClips(int limit)
	{
		std::vector<HttpClip> clips;
		CppSQLite3DB db;
		db.open(CGetSetOptions::GetDBPath());
		db.setBusyTimeout(1000);

		CppSQLite3Query q = db.execQueryEx(
			_T("SELECT lID, lDate, mText FROM Main WHERE bIsGroup = 0 ORDER BY clipOrder DESC LIMIT %d"),
			limit);

		while(!q.eof())
		{
			HttpClip clip;
			clip.id = q.getIntField(_T("lID"));
			clip.date = q.getInt64Field(_T("lDate"));
			clip.text = ReadClipText(db, clip.id, q.getStringField(_T("mText")));
			clips.push_back(std::move(clip));
			q.nextRow();
		}
		return clips;
	}

	static bool SetClipboardText(const std::string& utf8)
	{
		CStringW unicode = CTextConvert::Utf8ToUnicode(CStringA(utf8.data(), static_cast<int>(utf8.size())));
		const SIZE_T bytes = (static_cast<SIZE_T>(unicode.GetLength()) + 1) * sizeof(wchar_t);
		HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
		if(memory == nullptr)
			return false;

		void* dest = GlobalLock(memory);
		if(dest == nullptr)
		{
			GlobalFree(memory);
			return false;
		}

		memcpy(dest, unicode.GetString(), unicode.GetLength() * sizeof(wchar_t));
		reinterpret_cast<wchar_t*>(dest)[unicode.GetLength()] = L'\0';
		GlobalUnlock(memory);

		bool opened = false;
		for(int i = 0; i < 10; ++i)
		{
			if(OpenClipboard(nullptr))
			{
				opened = true;
				break;
			}
			Sleep(10);
		}

		if(!opened)
		{
			GlobalFree(memory);
			return false;
		}

		bool ok = false;
		if(EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory) != nullptr)
		{
			memory = nullptr;
			ok = true;
		}
		CloseClipboard();

		if(memory != nullptr)
			GlobalFree(memory);
		return ok;
	}

	static std::string MakeLatestJson()
	{
		auto clips = LoadRecentClips(1);
		if(clips.empty())
			return "{\"ok\":true,\"data\":null}";

		const HttpClip& clip = clips.front();
		std::ostringstream out;
		out << "{\"ok\":true,\"data\":{\"id\":" << clip.id
			<< ",\"date\":" << clip.date
			<< ",\"text\":\"" << JsonEscape(clip.text) << "\"}}";
		return out.str();
	}

	static std::string MakeHomePage()
	{
		auto clips = LoadRecentClips(5);
		std::ostringstream html;
		html << R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ditto HTTP</title><style>
body{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;max-width:900px;margin:32px auto;padding:0 16px;background:#f6f7f9;color:#111}
h1{font-size:24px}.card{background:#fff;border:1px solid #ddd;border-radius:10px;padding:14px;margin:10px 0;white-space:pre-wrap;word-break:break-word}.meta{font-size:12px;color:#777;margin-bottom:8px}
textarea{width:100%;box-sizing:border-box;min-height:110px;padding:10px}button{margin-top:8px;padding:8px 16px}code{background:#eee;padding:2px 5px;border-radius:4px}
</style></head><body><h1>Ditto 最近 5 条</h1>)HTML";

		if(clips.empty())
			html << "<div class=\"card\">暂无数据</div>";
		else
			for(const auto& clip : clips)
				html << "<div class=\"card\"><div class=\"meta\">ID " << clip.id << " · Unix " << clip.date << "</div>" << HtmlEscape(clip.text) << "</div>";

		html << R"HTML(<h2>写入剪贴板</h2><textarea id="text" placeholder="输入文本"></textarea><br>
<button onclick="sendText()">POST 到 Ditto</button><p id="status"></p>
<p>API：<code>GET /api/latest</code> · <code>POST /api/clipboard</code></p>
<script>async function sendText(){const s=document.getElementById('status'),t=document.getElementById('text').value,r=await fetch('/api/clipboard',{method:'POST',headers:{'Content-Type':'text/plain; charset=utf-8'},body:t}),j=await r.json();s.textContent=j.ok?'已写入剪贴板':'写入失败';if(j.ok)setTimeout(()=>location.reload(),250)}</script>
</body></html>)HTML";
		return html.str();
	}

	void Start()
	{
		if(m_server)
			return;

		m_server = std::make_unique<httplib::Server>();
		m_server->set_payload_max_length(1024 * 1024);

		m_server->Get("/", [](const httplib::Request&, httplib::Response& res)
		{
			try { res.set_content(MakeHomePage(), "text/html; charset=utf-8"); }
			catch(const CppSQLite3Exception&) { res.status = 500; res.set_content("Ditto database error", "text/plain; charset=utf-8"); }
		});

		m_server->Get("/api/latest", [](const httplib::Request&, httplib::Response& res)
		{
			try { res.set_content(MakeLatestJson(), "application/json; charset=utf-8"); }
			catch(const CppSQLite3Exception&) { res.status = 500; res.set_content("{\"ok\":false,\"error\":\"database error\"}", "application/json; charset=utf-8"); }
		});

		m_server->Post("/api/clipboard", [](const httplib::Request& req, httplib::Response& res)
		{
			if(req.body.empty()) { res.status = 400; res.set_content("{\"ok\":false,\"error\":\"empty body\"}", "application/json; charset=utf-8"); return; }
			if(!SetClipboardText(req.body)) { res.status = 500; res.set_content("{\"ok\":false,\"error\":\"clipboard unavailable\"}", "application/json; charset=utf-8"); return; }
			res.set_content("{\"ok\":true}", "application/json; charset=utf-8");
		});

		auto* server = m_server.get();
		m_thread = std::thread([server]() { server->listen("127.0.0.1", 23456); });
	}

	void Stop()
	{
		if(!m_server)
			return;
		m_server->stop();
		if(m_thread.joinable())
			m_thread.join();
		m_server.reset();
	}

	std::unique_ptr<httplib::Server> m_server;
	std::thread m_thread;
};
