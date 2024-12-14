//  Copyright 2024 James Forshaw. All Rights Reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include <Windows.h>
#include <winternl.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <sstream>
#include <memory>
#include <chrono>

using namespace std;
using namespace std::chrono;

#pragma comment(lib, "ntdll.lib")

extern "C" {
    enum EVENT_TYPE {
        NotificationEvent,
        SynchronizationEvent
    };

    NTSYSAPI NTSTATUS NtCreateEvent(
        PHANDLE            EventHandle,
        ACCESS_MASK        DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes,
        EVENT_TYPE         EventType,
        BOOLEAN            InitialState
    );

    NTSYSAPI NTSTATUS NtOpenEvent(
        PHANDLE            EventHandle,
        ACCESS_MASK        DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes
    );

    NTSYSAPI NTSTATUS NtCreateDirectoryObjectEx(PHANDLE Handle,
        ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, 
        HANDLE ShadowDirectory, ULONG Flags);

    NTSYSAPI NTSTATUS NtOpenDirectoryObject(PHANDLE Handle,
        ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);

    NTSYSAPI NTSTATUS NtCreateSymbolicLinkObject(
        PHANDLE LinkHandle,
        ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes,
        PUNICODE_STRING DestinationName
    );

    typedef struct _OBJECT_NAME_INFORMATION
    {
        UNICODE_STRING Name;
    } OBJECT_NAME_INFORMATION, * POBJECT_NAME_INFORMATION;
}

class NtException {
public:
    explicit NtException(NTSTATUS status)
        : m_status(status) {}
    NTSTATUS status() const {
        return m_status;
    }
private:
    NTSTATUS m_status;
};

static void Check(NTSTATUS status) {
    if (NT_ERROR(status)) {
        throw NtException(status);
    }
}

static wstring GetName(HANDLE handle) {
    DWORD size = USHRT_MAX + sizeof(OBJECT_NAME_INFORMATION);
    auto buffer = make_unique<char[]>(size);

    ULONG ret_length = 0;
    Check(NtQueryObject(handle, static_cast<OBJECT_INFORMATION_CLASS>(1), buffer.get(), size, &ret_length));
    POBJECT_NAME_INFORMATION name = reinterpret_cast<POBJECT_NAME_INFORMATION>(buffer.get());
    return wstring(name->Name.Buffer, name->Name.Length / sizeof(WCHAR));
}

class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : m_handle(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept {
        m_handle = other.m_handle;
        other.m_handle = nullptr;
    }
    const ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle() {
        ::CloseHandle(m_handle);
    }

    HANDLE get() const {
        return m_handle;
    }

    HANDLE* ptr() {
        return &m_handle;
    }

    wstring name() const {
        return GetName(m_handle);
    }
private:
    HANDLE m_handle;
};

class Timer {
public:
    Timer() {
        m_start = high_resolution_clock::now();
    }
    double GetTime(int iterations) const {
        auto stop = high_resolution_clock::now();
        auto result = duration_cast<microseconds>(stop - m_start);

        return (double)result.count() / (double)iterations;
    }
private:
    high_resolution_clock::time_point m_start;
};

struct UnicodeString : public UNICODE_STRING {
    explicit UnicodeString(const wstring& str) : m_str(str) {
        MaximumLength = Length = (USHORT)(m_str.size() * sizeof(wchar_t));
        Buffer = const_cast<wchar_t*>(m_str.c_str());
    }
private:
    wstring m_str;
};

struct ObjectAttributes : public OBJECT_ATTRIBUTES {
    explicit ObjectAttributes(const wstring& str, HANDLE root = nullptr, ULONG attributes = 0) 
        : m_str(str) {
        InitializeObjectAttributes(this, &m_str, attributes, root, nullptr);
    }
private:
    UnicodeString m_str;
};

static ScopedHandle CreateDirectory(const wstring& name, HANDLE root = nullptr, HANDLE shadow_dir = nullptr) {
    ObjectAttributes obja(name, root);
    ScopedHandle handle;
    Check(NtCreateDirectoryObjectEx(handle.ptr(), MAXIMUM_ALLOWED, &obja, shadow_dir, 0));
    return handle;
}

static ScopedHandle OpenDirectory(const wstring& name, HANDLE root = nullptr) {
    ObjectAttributes obja(name, root);
    ScopedHandle handle;
    Check(NtOpenDirectoryObject(handle.ptr(), MAXIMUM_ALLOWED, &obja));
    return handle;
}

static ScopedHandle CreateLink(const wstring& name, HANDLE root, const wstring& target) {
    ObjectAttributes obja(name, root);
    UnicodeString target_ustr(target);
    ScopedHandle handle;
    Check(NtCreateSymbolicLinkObject(handle.ptr(), MAXIMUM_ALLOWED, &obja, &target_ustr));
    return handle;
}

static ScopedHandle CreateEvent(const wstring& name, HANDLE root = nullptr) {
    ObjectAttributes obja(name, root);
    ScopedHandle handle;
    Check(NtCreateEvent(handle.ptr(), MAXIMUM_ALLOWED, &obja, NotificationEvent, FALSE));
    return handle;
}

static wstring IntToString(int i) {
    wstringstream ss;
    ss << i;
    return ss.str();
}

static double RunTest(const wstring name, int iterations, wstring create_name = L"", HANDLE root = nullptr)
{
    if (create_name.empty()) {
        create_name = name;
    }
    ScopedHandle event_handle = CreateEvent(create_name, root);
    ObjectAttributes obja(name);
    vector<ScopedHandle> handles;
    Timer timer;
    for (int i = 0; i < iterations; ++i) {
        HANDLE open_handle;
        Check(NtOpenEvent(&open_handle, MAXIMUM_ALLOWED, &obja));
        handles.emplace_back(open_handle);
    }
    return timer.GetTime(iterations);
}

static int GetArg(const vector<string>& args, int index, int def_value) {
    if (args.size() > index && args[index] != "_") {
        return atoi(args[index].c_str());
    }
    return def_value;
}

static wstring MakeNullString(int count) {
    wstring ret;
    for (int i = 0; i < count; ++i) {
        ret.push_back(0);
    }
    return ret;
}

static wstring MakeCollisionName(int count) {
    return MakeNullString(count) + L"A";
}

static void Test1(const vector<string>& args)
{
    int iterations = GetArg(args, 0, 1000);

    auto test1 = RunTest(L"\\BaseNamedObjects\\{2F2C4C1D-FD52-47CA-BF97-CA72B6CA55F8}", iterations);
    printf("%.2fus for %d iterations.\n", test1, iterations);
}

static void Test2(const vector<string>& args)
{
    int iterations = GetArg(args, 0, 1000);

    wstring path;
    while (path.size() <= 32000) {
        auto result = RunTest(L"\\BaseNamedObjects\\A" + path, iterations);
        printf("%zu,%f\n", path.size(), result);
        path += wstring(500, 'A');
    }
}

static void Test3(const vector<string>& args)
{
    int iterations = GetArg(args, 0, 1000);
    int dir_count = GetArg(args, 1, 16000);

    ScopedHandle base_dir = OpenDirectory(L"\\BaseNamedObjects");
    HANDLE last_dir = base_dir.get();
    vector<ScopedHandle> dirs;
    for (int i = 0; i < dir_count; i++) {
        dirs.emplace_back(CreateDirectory(L"A", last_dir));
        last_dir = dirs.back().get();
        if ((i % 500) == 0)
        {
            auto result = RunTest(GetName(last_dir) + L"\\X", iterations);
            printf("%d,%f\n", i + 1, result);
        }
    }
}

static void Test4(const vector<string>& args)
{
    int iterations = GetArg(args, 0, 10);
    int dir_count = GetArg(args, 1, 16000);
    int symlink_count = GetArg(args, 2, 63);

    ScopedHandle base_dir = OpenDirectory(L"\\BaseNamedObjects");
    HANDLE last_dir = base_dir.get();
    vector<ScopedHandle> dirs;
    for (int i = 0; i < dir_count; i++) {
        dirs.emplace_back(CreateDirectory(L"A", last_dir));
        last_dir = dirs.back().get();
    }
    vector<ScopedHandle> links;
    wstring last_dir_name = GetName(last_dir);
    for (int i = 0; i < symlink_count; ++i) {
        links.emplace_back(CreateLink(IntToString(i), last_dir, last_dir_name + L"\\" + IntToString(i + 1)));
    }
    printf("%f\n", RunTest(links.front().name(), iterations, IntToString(symlink_count), last_dir));
}

static void Test5(const vector<string>& args)
{
    int iterations = GetArg(args, 0, 1000);
    int collision_count = GetArg(args, 1, 32000);

    ScopedHandle base_dir = CreateDirectory(L"\\BaseNamedObjects\\A");
    vector<ScopedHandle> dirs;
    wstring base_dir_name = MakeCollisionName(collision_count);
    for (int i = 0; i < collision_count; i++) {
        wstring name = MakeCollisionName(collision_count - i);
        dirs.emplace_back(CreateDirectory(name, base_dir.get()));
        if ((i % 500) == 0) {
            Timer timer;
            for (int j = 0; j < iterations; ++j) {
                OpenDirectory(base_dir_name, base_dir.get());
            }
            printf("%d,%f\n", i, timer.GetTime(iterations));
        }
    }
}

static void Test6(const vector<string>& args)
{
    int collision_count = GetArg(args, 0, 32000);

    vector<wstring> names;
    for (int i = 0; i < collision_count; i++) {
        names.push_back(MakeCollisionName(collision_count - i));
    }

    ScopedHandle base_dir = CreateDirectory(L"\\BaseNamedObjects\\A");
    vector<ScopedHandle> dirs;
    Timer timer;
    for (auto& name : names) {
        dirs.emplace_back(CreateDirectory(name, base_dir.get()));
    }
    printf("%f\n", timer.GetTime(1));
}

static void Test7(const vector<string>& args)
{
    int iterations = GetArg(args, 0, 1000);
    int dir_count = GetArg(args, 1, 16000);

    wstring dir_name = L"\\BaseNamedObjects\\A";
    ScopedHandle shadow_dir = CreateDirectory(dir_name);
    ScopedHandle target_dir = CreateDirectory(L"A", shadow_dir.get(), shadow_dir.get());
    for (int i = 0; i < dir_count; i += 500) {
        wstring open_name = dir_name;
        for (int j = 0; j < i; j++) {
            open_name += L"\\A";
        }
        open_name += L"\\X";
        printf("%d,%f\n", i, RunTest(open_name, iterations, L"X", shadow_dir.get()));
    }
}

static void Test8(const vector<string>& args)
{
    int iterations = GetArg(args, 0, 1000);
    int dir_count = GetArg(args, 1, 16000);
    int symlink_count = GetArg(args, 2, 1);
    int collision_count = GetArg(args, 3, 16000);

    wstring dir_name = L"\\BaseNamedObjects\\A";
    ScopedHandle shadow_dir = CreateDirectory(dir_name);
    ScopedHandle target_dir = CreateDirectory(L"A", shadow_dir.get(), shadow_dir.get());
    vector<ScopedHandle> dirs;
    for (int i = 0; i < collision_count - 1; ++i) {
        dirs.emplace_back(CreateDirectory(MakeCollisionName(collision_count - i), shadow_dir.get()));
    }

    wstring last_dir_name = dir_name;

    for (int i = 0; i < dir_count; i++) {
        last_dir_name += L"\\A";
    }

    printf("Created directories\n");
    vector<ScopedHandle> links;
    for (int i = 0; i < symlink_count; ++i) {
        links.emplace_back(CreateLink(IntToString(i), shadow_dir.get(), last_dir_name + L"\\" + IntToString(i + 1)));
    }

    printf("%f\n", RunTest(last_dir_name + L"\\0", 1, IntToString(symlink_count), shadow_dir.get()));
}

static void PrintHelp() {
    printf("Specify test:\n");
    printf("1 = Simple open.\n");
    printf("2 = Incrementing length name string.\n");
    printf("3 = Recursive directories.\n");
    printf("4 = Recursive symlinks.\n");
    printf("5 = Name collisions.\n");
    printf("6 = Collision insertion time.\n");
    printf("7 = Shadow directories.\n");
    printf("8 = Full test.\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintHelp();
        return 1;
    }

    try {
        int test_no = atoi(argv[1]);
        vector<string> args(&argv[2], &argv[argc]);

        switch (test_no) {
        case 1:
            Test1(args);
            break;
        case 2:
            Test2(args);
            break;
        case 3:
            Test3(args);
            break;
        case 4:
            Test4(args);
            break;
        case 5:
            Test5(args);
            break;
        case 6:
            Test6(args);
            break;
        case 7:
            Test7(args);
            break;
        case 8:
            Test8(args);
            break;
        default:
            printf("Unknown test: %d.\n", test_no);
            PrintHelp();
            return 1;
        }
    }
    catch (const NtException& ex) {
        printf("Error in program: %08X\n", ex.status());
    }

    return 0;
}
