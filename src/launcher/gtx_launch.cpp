// gtx-launch: start a game with GTX shim DLLs preloaded (M1 loader, first step).
//
//   gtx-launch [--shims <dir>] [--wait-ms N] <game.exe> [game args...]
//
// The process is created suspended, each shim DLL found in <dir> (default: this exe's folder)
// is loaded into it via a remote LoadLibraryW, then the main thread resumes. Because the Windows
// loader returns an already-loaded module with the same base name, later LoadLibrary calls by
// the game (e.g. the GDK static lib's LoadLibraryExW("xgameruntime.dll", SYSTEM32)) get our shim.
// Environment (incl. GTX_LOG) is inherited. Exit code = the game's exit code.
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

// Shims the launcher knows how to preload. Order matters if shims depend on each other.
const wchar_t* const kShimNames[] = {L"xgameruntime.dll"};

std::wstring dir_of(const std::wstring& path) {
  size_t p = path.find_last_of(L"\\/");
  return p == std::wstring::npos ? L"." : path.substr(0, p);
}

std::wstring self_dir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return dir_of(buf);
}

std::wstring quote(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
    } else if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out += c;
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      out += c;
      backslashes = 0;
    }
  }
  out.append(backslashes * 2, L'\\');
  return out + L"\"";
}

bool inject(HANDLE process, const std::wstring& dll) {
  size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote) return false;
  bool ok = false;
  if (WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) {
    auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
    HANDLE t = CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr);
    if (t) {
      WaitForSingleObject(t, 30000);
      DWORD module_low = 0;
      ok = GetExitCodeThread(t, &module_low) && module_low != 0;
      CloseHandle(t);
    }
  }
  VirtualFreeEx(process, remote, 0, MEM_RELEASE);
  return ok;
}

int usage() {
  std::fwprintf(stderr, L"usage: gtx-launch [--shims <dir>] [--wait-ms N] <game.exe> [args...]\n");
  return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring shim_dir = self_dir();
  DWORD wait_ms = INFINITE;
  int i = 1;
  for (; i < argc && argv[i][0] == L'-'; ++i) {
    if (!std::wcscmp(argv[i], L"--shims") && i + 1 < argc) {
      shim_dir = argv[++i];
    } else if (!std::wcscmp(argv[i], L"--wait-ms") && i + 1 < argc) {
      wait_ms = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
    } else {
      return usage();
    }
  }
  if (i >= argc) return usage();

  wchar_t exe_full[MAX_PATH];
  if (!GetFullPathNameW(argv[i], MAX_PATH, exe_full, nullptr)) return usage();
  std::wstring cmd = quote(exe_full);
  for (int k = i + 1; k < argc; ++k) cmd += L" " + quote(argv[k]);
  std::wstring work_dir = dir_of(exe_full);

  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
  cmd_buf.push_back(L'\0');
  if (!CreateProcessW(exe_full, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                      work_dir.c_str(), &si, &pi)) {
    std::fwprintf(stderr, L"gtx-launch: CreateProcess failed (%lu): %ls\n", GetLastError(), exe_full);
    return 1;
  }

  int injected = 0;
  for (const wchar_t* name : kShimNames) {
    std::wstring dll = shim_dir + L"\\" + name;
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    if (inject(pi.hProcess, dll)) {
      ++injected;
      std::fwprintf(stderr, L"gtx-launch: preloaded %ls\n", dll.c_str());
    } else {
      std::fwprintf(stderr, L"gtx-launch: FAILED to preload %ls (%lu)\n", dll.c_str(), GetLastError());
    }
  }

  ResumeThread(pi.hThread);
  DWORD exit_code = 0;
  if (WaitForSingleObject(pi.hProcess, wait_ms) == WAIT_TIMEOUT) {
    std::fwprintf(stderr, L"gtx-launch: --wait-ms elapsed, terminating game\n");
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return injected == 0 ? 3 : static_cast<int>(exit_code);
}
