#include "DynaSpy.h"
#include <WinNT.h>
LPSTR win_strerror(DWORD errorID) {
  LPSTR error_str;
  size_t error_strlen = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL, errorID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      (LPSTR)&error_str, 0, NULL);

  return error_str;
}

#define declare(type, variable)                                                \
  type variable;                                                               \
  memset(&variable, 0, sizeof(type));

[[noreturn]] void usage(char *cmd) {
  std::cout << "Usage: " << cmd << " <executable> [arguments ...]\n";
  exit(1);
}

int main(int argc, char *argv[]) {

  bool debug = true;

  if (argc < 2) {
    usage(argv[0]);
  }

  int child_process_count = 0;

  LPCSTR mark_name = argv[1];
  // LPCSTR mark_name = "./HelloWorld.exe";
  LPSTR mark_commandline = argv[2];
  // LPSTR mark_commandline = nullptr;

  declare(PROCESS_INFORMATION, mark_processinformation);
  declare(STARTUPINFO, mark_processstartupinfo);
  HANDLE mark_imagehandle = nullptr;

  bool create_result = CreateProcessA(
      mark_name, mark_commandline, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS,
      NULL, NULL, (LPSTARTUPINFOA)(&mark_processstartupinfo),
      &mark_processinformation);

  if (!create_result) {
    std::cout << "Did not launch mark process with path " << mark_name
              << " because " << win_strerror(GetLastError()) << "\n";
    return 1;
  }
  DWORD mark_pid = mark_processinformation.dwProcessId;
  if (debug) {
    std::cout << "Successfully launched " << mark_name;
    if (mark_commandline) {
      std::cout << " with arguments " << mark_commandline << " ";
    }
    std::cout << "(PID: " << mark_pid << ").\n";
  }

  CloseHandle(mark_processinformation.hProcess);
  CloseHandle(mark_processinformation.hThread);

  for (;;) {
    declare(DEBUG_EVENT, debugEvent);

    WaitForDebugEvent(&debugEvent, INFINITE);

    DWORD continue_type = DBG_CONTINUE;

    switch (debugEvent.dwDebugEventCode) {
    case EXCEPTION_DEBUG_EVENT: {
      if (debug) {
        std::cout << "Got an unhandled exception debug event ("
                  << debugEvent.u.Exception.ExceptionRecord.ExceptionCode
                  << "), but we don't care.\n";
      }
      continue_type = DBG_EXCEPTION_NOT_HANDLED;
      break;
    }
    case CREATE_PROCESS_DEBUG_EVENT: {
      child_process_count++;
      if (debug) {
        std::cout << "Process being created (" << child_process_count << ")!\n";
      }

      // Store the handle because we will want to close it when we are done
      // debugging. I am not 100% sure why we can't close it here!
      mark_imagehandle = debugEvent.u.CreateProcessInfo.hFile;
      break;
    }
    case CREATE_THREAD_DEBUG_EVENT: {
      if (debug) {
        std::cout << "Thread being created!\n";
      }
      break;
    }
    case EXIT_PROCESS_DEBUG_EVENT: {
      if (debug) {
        std::cout << "Process exiting!\n";
      }
      // We only want the debugger to stop if the process exiting
      // is that of the immediate mark.
      DWORD exiting_pid = debugEvent.dwProcessId;
      if (exiting_pid == mark_pid) {
        std::cout << "Mark finished...\n";
        exit(0);
      }
      break;
    }
    case LOAD_DLL_DEBUG_EVENT: {
      if (debug) {
        std::cout << "Loading a dll!\n";
      }

      TCHAR dll_name[MAX_PATH] = {
          0,
      };
      // TODO: This may not work. Check the return value before moving on!
      GetFinalPathNameByHandle(debugEvent.u.LoadDll.hFile, dll_name, MAX_PATH,
                               VOLUME_NAME_NT);
      std::cout << "Loaded a DLL named: " << dll_name << "\n";
      CloseHandle(debugEvent.u.LoadDll.hFile);
      break;
    }
    default: {
      if (debug) {
        std::cout << "Got an unhandled debug event ("
                  << debugEvent.dwDebugEventCode << "), but we don't care.\n";
      }
      break;
    }
    }
    if (!ContinueDebugEvent(debugEvent.dwProcessId, debugEvent.dwThreadId,
                            continue_type)) {
      LPSTR error_string = win_strerror(GetLastError());
      std::cout << "Error: ContinueDebugEvent: " << error_string << "\n";
      std::cout << "Error: Exiting ... \n";
      LocalFree(error_string);
      break;
    }
  }
  if (mark_imagehandle) {
    CloseHandle(mark_imagehandle);
  }
  return 0;
}
