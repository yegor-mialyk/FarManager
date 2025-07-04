﻿// Based on Zoltan Csizmadia's TaskManagerEx source, zoltan_csizmadia@yahoo.com

#include <algorithm>
#include <memory>
#include <mutex>
#include <string_view>

#include "Proclist.hpp"
#include "perfthread.hpp" // fot GetProcessData
#include "Proclng.hpp"

#include <lmcons.h>
#include <sddl.h>

#include <algorithm.hpp>
#include <smart_ptr.hpp>
#include <string_utils.hpp>

using namespace std::literals;

struct SYSTEM_HANDLE_TABLE_ENTRY_INFO
{
	USHORT UniqueProcessId;
	USHORT CreatorBackTraceIndex;
	UCHAR ObjectTypeIndex;
	UCHAR HandleAttributes;
	USHORT HandleValue;
	PVOID Object;
	ULONG GrantedAccess;
};

// Defined in GCC headers with different field names :(
struct PROCLIST_SYSTEM_HANDLE_INFORMATION
{
	ULONG NumberOfHandles;
	SYSTEM_HANDLE_TABLE_ENTRY_INFO Handles[1];
};

struct THREAD_BASIC_INFORMATION
{
	LONG       ExitStatus;
	PVOID      TebBaseAddress;
	CLIENT_ID  ClientId;
	ULONG_PTR  AffinityMask;
	LONG       Priority;
	LONG       BasePriority;
};

#ifdef _MSC_VER
inline constexpr auto ObjectNameInformation = static_cast<OBJECT_INFORMATION_CLASS>(1);
#endif

static bool GetProcessId(HANDLE Handle, DWORD& Pid)
{
	PROCESS_BASIC_INFORMATION pi{};

	if (!NT_SUCCESS(pNtQueryInformationProcess(Handle, ProcessBasicInformation, &pi, sizeof(pi), {})))
		return false;

	Pid = static_cast<DWORD>(pi.UniqueProcessId);
	return true;
}

static bool GetThreadId(HANDLE Handle, DWORD& Tid)
{
	THREAD_BASIC_INFORMATION ti;
	if (!NT_SUCCESS(pNtQueryInformationThread(Handle, 0, &ti, sizeof(ti), {})))
		return false;

	Tid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(ti.ClientId.UniqueThread));
	return true;
}

static std::wstring to_string(const UNICODE_STRING& Str)
{
	return { Str.Buffer, Str.Length / sizeof(wchar_t) };
}

static std::unique_ptr<char[]> query_object(HANDLE Handle, OBJECT_INFORMATION_CLASS const Class)
{
	ULONG Size = 8192;
	for (;;)
	{
		auto Buffer = std::make_unique<char[]>(Size);

		const auto Status = pNtQueryObject(Handle, Class, Buffer.get(), Size, &Size);
		if (NT_SUCCESS(Status))
			return Buffer;

		if (Status == STATUS_INFO_LENGTH_MISMATCH)
			continue;

		return {};
	}
}

static std::wstring GetFileName(HANDLE Handle)
{
	using pair = std::pair<HANDLE, DWORD>;
	pair HandleAndFileType{ Handle, FILE_TYPE_UNKNOWN };

	// Check if it's possible to get the file name info
	struct test
	{
		static DWORD WINAPI GetFileNameThread(PVOID Param)
		{
			auto& [Handle, FileType] = *static_cast<pair*>(Param);
			FileType = GetFileType(Handle);
			if (FileType != FILE_TYPE_DISK)
			{
				IO_STATUS_BLOCK iob;
				FILE_BASIC_INFO info;
				const auto FileBasicInformation = 4;
				pNtQueryInformationFile(Handle, &iob, &info, sizeof(info), FileBasicInformation);
			}
			return 0;
		}
	};

	const handle Thread(CreateThread({}, 0, test::GetFileNameThread, &HandleAndFileType, 0, {}));

	// Wait for finishing the thread
	if (WaitForSingleObject(Thread.get(), 100) == WAIT_TIMEOUT)
	{
		TerminateThread(Thread.get(), 0);
		switch (HandleAndFileType.second)
		{
		case FILE_TYPE_PIPE:     return L"<pipe>"s;
		case FILE_TYPE_CHAR:     return L"<char>"s;
		case FILE_TYPE_REMOTE:   return L"<remote>"s;
		case FILE_TYPE_UNKNOWN:  return L"<unknown>"s;
		default:                 return far::format(L"<unknown> ({})"sv, HandleAndFileType.second);
		}
	}

	const auto Data = query_object(Handle, ObjectNameInformation);
	if (!Data)
		return {};

	return to_string(*reinterpret_cast<const UNICODE_STRING*>(Data.get()));
}

static std::wstring GetTypeToken(HANDLE Handle)
{
	const auto Data = query_object(Handle, ObjectTypeInformation);
	if (!Data)
		return {};

	return to_string(*reinterpret_cast<const UNICODE_STRING*>(Data.get()));
}

enum
{
	OB_TYPE_UNKNOWN,
	/*OB_TYPE_TYPE,
	OB_TYPE_DIRECTORY,
	OB_TYPE_SYMBOLIC_LINK,
	OB_TYPE_TOKEN,*/
	OB_TYPE_PROCESS,
	OB_TYPE_THREAD,
	/*OB_TYPE_JOB,
	OB_TYPE_DEBUG_OBJECT,
	OB_TYPE_EVENT,
	OB_TYPE_EVENT_PAIR,
	OB_TYPE_MUTANT,
	OB_TYPE_CALLBACK,
	OB_TYPE_SEMAPHORE,
	OB_TYPE_TIMER,
	OB_TYPE_PROFILE,
	OB_TYPE_KEYED_EVENT,
	OB_TYPE_WINDOW_STATION,
	OB_TYPE_DESKTOP,
	OB_TYPE_SECTION,*/
	OB_TYPE_KEY,
	/*OB_TYPE_PORT,
	OB_TYPE_WAITABLE_PORT,
	OB_TYPE_ADAPTER,
	OB_TYPE_CONTROLLER,
	OB_TYPE_DEVICE,
	OB_TYPE_DRIVER,
	OB_TYPE_IOCOMPLETION,*/
	OB_TYPE_FILE,
	/*OB_TYPE_WMI_GUID,*/

	OB_OTHER
};

static wchar_t const* const constStrTypes[]
{
	L"",
	/*L"Type",
	L"Directory",
	L"SymbolicLink",
	L"Token",*/
	L"Process",
	L"Thread",
	/*L"Job",
	L"OB_TYPE_DEBUG_OBJECT",
	L"Event",
	L"EventPair",
	L"Mutant",
	L"Callback",
	L"Semaphore",
	L"Timer",
	L"Profile",
	L"OB_TYPE_KEYED_EVENT",
	L"WindowStation",
	L"Desktop",
	L"Section",*/
	L"Key",
	/*L"Port",
	L"OB_TYPE_WAITABLE_PORT",
	L"Adapter",
	L"Controller",
	L"Device",
	L"Driver",
	L"IoCompletion",*/
	L"File",
	/*L"WmiGuid",*/
};

static_assert(std::size(constStrTypes) == OB_OTHER);

static WORD GetTypeFromTypeToken(const wchar_t* const TypeToken)
{
	const auto It = std::find_if(std::cbegin(constStrTypes), std::cend(constStrTypes), [&](const wchar_t* i)
	{
		return !FSF.LStricmp(i, TypeToken);
	});

	return It == std::cend(constStrTypes)? static_cast<WORD>(OB_OTHER) : static_cast<WORD>(It - std::cbegin(constStrTypes));
}

static const auto& GetUserAccountID()
{
	static const auto UserAccountID = []() -> std::wstring
	{
		wchar_t UserName[UNLEN + 1];
		auto UserNameSize = static_cast<DWORD>(std::size(UserName));
		if (!GetUserName(UserName, &UserNameSize))
		{
			return {};
		}

		SID_NAME_USE eUse;
		DWORD cbSid = 0, cbDomainName = 0;
		LookupAccountName({}, UserName, {}, &cbSid, {}, &cbDomainName, &eUse);
		if (!cbSid)
			return {};

		const auto Sid = make_malloc<void>(cbSid);
		const auto DomainName = std::make_unique<wchar_t[]>(cbDomainName + 1);
		if (!LookupAccountName({}, UserName, Sid.get(), &cbSid, DomainName.get(), &cbDomainName, &eUse))
			return {};

		local_ptr<wchar_t> StrSid;
		if (!ConvertSidToStringSid(Sid.get(), &ptr_setter(StrSid)))
			return {};

		return StrSid.get();
	}();

	return UserAccountID;
}

static std::wstring GetNameByType(HANDLE handle, WORD type, PerfThread* pThread)
{
	switch (type)
	{
	case OB_TYPE_UNKNOWN:
		return {};

	case OB_TYPE_PROCESS:
		if (DWORD dwId = 0; GetProcessId(handle, dwId))
		{
			const std::scoped_lock l(*pThread);
			const auto pd = pThread->GetProcessData(dwId);
			const auto pName = pd? pd->ProcessName : L"<unknown>"sv;
			return far::format(L"{} ({})"sv, pName, dwId);
		}
		return {};

	case OB_TYPE_THREAD:
		{
			std::wstring ThreadName;

			if (DWORD dwId = 0; GetThreadId(handle, dwId))
				ThreadName = far::format(L"TID: {}"sv, dwId);

			if (local_ptr<wchar_t> Buffer; SUCCEEDED(pGetThreadDescription(handle, &ptr_setter(Buffer))))
			{
				if (*Buffer)
					append(ThreadName, L", "sv, Buffer.get());
			}

			return ThreadName;
		}

	case OB_TYPE_FILE:
		return GetFileName(handle);

	default:
		const auto Data = query_object(handle, ObjectNameInformation);
		if (!Data)
			return {};

		const auto& DataStr = *reinterpret_cast<const UNICODE_STRING*>(Data.get());
		if (!DataStr.Length)
			return {};

		const std::wstring_view ws(DataStr.Buffer, DataStr.Length / sizeof(wchar_t));

		const auto
			REGISTRY = L"\\REGISTRY\\"sv,
			USER     = L"USER"sv,
			CLASSES  = L"MACHINE\\SOFTWARE\\CLASSES"sv,
			MACHINE  = L"MACHINE"sv,
			CLASSES_ = L"_CLASSES"sv;

		if (type == OB_TYPE_KEY && ws.starts_with(REGISTRY))
		{
			auto ws1 = ws.substr(REGISTRY.size());
			std::wstring_view s0;

			if (ws1.starts_with(USER))
			{
				ws1.remove_prefix(USER.size());

				if (ws1.starts_with(L"\\"sv))
				{
					s0 = L"HKU\\"sv;
					ws1.remove_prefix(1);

					const auto& Id = GetUserAccountID();

					if (ws1.starts_with(Id))
					{
						s0 = L"HKCU"sv;
						ws1.remove_prefix(Id.size());

						if (ws1.starts_with(CLASSES_))
						{
							s0 = L"HKCU\\Classes"sv;
							ws1.remove_prefix(CLASSES_.size());
						}
					}
				}
				else
				{
					s0 = L"HKU"sv;
				}
			}
			else if (ws1.starts_with(CLASSES)) { s0 = L"HKCR"sv; ws1.remove_prefix(CLASSES.size()); }
			else if (ws1.starts_with(MACHINE)) { s0 = L"HKLM"sv; ws1.remove_prefix(MACHINE.size()); }

			if (!s0.empty())
			{
				return std::wstring(s0).append(ws1);
			}
		}

		return std::wstring(ws);
	}
}

static handle duplicate_handle(HANDLE h, DWORD dwPID)
{
	DebugToken token;
	const handle RemoteProcess(OpenProcessForced(&token, PROCESS_DUP_HANDLE, dwPID, TRUE));
	if (!RemoteProcess)
		return {};

	handle DuplicatedHandle;
	if (!DuplicateHandle(RemoteProcess.get(), h, GetCurrentProcess(), &ptr_setter(DuplicatedHandle), 0, 0, DUPLICATE_SAME_ACCESS))
		return {};

	return DuplicatedHandle;
}

bool PrintHandleInfo(DWORD dwPID, HANDLE file, bool bIncludeUnnamed, PerfThread* pThread)
{
	block_ptr<PROCLIST_SYSTEM_HANDLE_INFORMATION> Info(sizeof(*Info));

	for (;;)
	{
		const auto SystemHandleInformation = 16;

		ULONG ReturnSize{};
		const auto Result = pNtQuerySystemInformation(SystemHandleInformation, Info.data(), static_cast<ULONG>(Info.size()), &ReturnSize);
		if (NT_SUCCESS(Result))
			break;

		if (any_of(Result, STATUS_INFO_LENGTH_MISMATCH, STATUS_BUFFER_OVERFLOW, STATUS_BUFFER_TOO_SMALL))
		{
			Info.reset(ReturnSize? ReturnSize : grow_exp(Info.size(), {}));
			continue;
		}

		return false;
	}

	WriteToFile(file, far::format(L"\n{}:\n{:6} {:8} {:15} {}\n"sv,
		GetMsg(MHandles),
		GetMsg(MHandlesHandle),
		GetMsg(MHandlesAccess),
		GetMsg(MHandlesType),
		GetMsg(MHandlesName))
	);

	// Iterating through the objects
	for (DWORD i = 0; i < Info->NumberOfHandles; i++)
	{
		// ProcessId filtering check
		if (Info->Handles[i].UniqueProcessId != dwPID && dwPID != static_cast<DWORD>(-1))
			continue;

		auto Handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(Info->Handles[i].HandleValue));
		handle DuplicatedHandle;
		if (dwPID != GetCurrentProcessId())
		{
			DuplicatedHandle = duplicate_handle(Handle, dwPID);
			Handle = DuplicatedHandle.get();
		}

		const auto TypeToken = GetTypeToken(Handle);
		const auto type = GetTypeFromTypeToken(TypeToken.c_str());
		const auto Name = GetNameByType(Handle, type, pThread);

		if (!bIncludeUnnamed && Name.empty())
			continue;

		WriteToFile(file, far::format(L"{:6X} {:08X} {:15} {}\n"sv,
			Info->Handles[i].HandleValue,
			Info->Handles[i].GrantedAccess,
			TypeToken,
			Name));
	}

	return true;
}
