#include "DynaSpy.h"

#include <fstream>
#include <functional>
#include <numeric>
#include <string>

#include "args.hxx"

/** A Windows version of strerror(3).
 *
 * @param errorID The error number from which to find a
 * string.
 * @return a pointer to a null-terminated C string.
 */
LPSTR win_strerror(DWORD errorID) {
  LPSTR error_str = nullptr;
  size_t error_strlen = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL, errorID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      (LPSTR)&error_str, 0, NULL);

  return error_str;
}

/** Get a filename from a handle. This operation is not
 * guaranteed to succeed. According to MSDN, in fact, it
 * is not a conversion that is even "likely" to succeed:
 * https://docs.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlea/
 *
 * @param handle the handle from which to get a filename.
 * @return pointer to a null-terminated C string which
 * contains the filename associated with the handle. The
 * result will be nullptr if the conversion fails.
 */
TCHAR *filename_from_handle(HANDLE handle) {
  TCHAR filename[MAX_PATH] = {
      0,
  };
  DWORD result =
      GetFinalPathNameByHandle(handle, filename, MAX_PATH, VOLUME_NAME_NT);
  // See documentation. These are the two ways that GetFinalPathNameByHandle
  // signals failure.
  if (result == 0 || result > MAX_PATH) {
    return nullptr;
  }
  TCHAR *result_filename = (TCHAR *)calloc(sizeof(TCHAR), result + 1);
  // Very defensive. calloc() could return 0.
  if (result_filename == nullptr) {
    return nullptr;
  }
  strncat(result_filename, filename, result);
  return result_filename;
}

#define zero_declare(type, variable) \
  type variable;                     \
  memset(&variable, 0, sizeof(type));

[[noreturn]] void usage(char *cmd) {
  std::cout << "Usage: " << cmd << " <executable> [arguments ...]\n";
  exit(1);
}

int main(int argc, char *argv[]) {
  args::ArgumentParser parser(
      "Log the DLLs that are dynamically loaded at runtime.");
  args::HelpFlag help(parser, "help", "Display this help menu", {'h', "help"});
  args::Flag debug(parser, "debug", "Enable debugging.", {"d", "debug"});
  args::ValueFlag<std::string> outfilename(
      parser, "outfile", "Store output to a file", {"o", "outfile"});
  args::Positional<std::string> mark_name(parser, "mark_name",
                                          "The mark program to execute.",
                                          args::Options::Required);
  args::PositionalList<std::string> mark_commandline(
      parser, "arguments", "The arguments for the mark program.");
  try {
    parser.ParseCLI(argc, argv);
  } catch (args::Help) {
    std::cout << parser;
    return 0;
  } catch (args::ParseError e) {
    std::cerr << e.what() << std::endl;
    std::cerr << parser;
    return 1;
  } catch (args::ValidationError e) {
    std::cerr << e.what() << std::endl;
    std::cerr << parser;
    return 1;
  }

  std::ofstream outfile;
  if (outfilename.Get() != "") {
    outfile.open(outfilename.Get(), std::ios_base::trunc);
    if (!outfile.is_open()) {
      std::cerr << "Could not open output file named " << outfilename.Get()
                << "!\n";
      exit(1);
    }
  }
  std::ostream &outputter = outfile.is_open() ? outfile : std::cout;

  zero_declare(PROCESS_INFORMATION, mark_processinformation);
  zero_declare(STARTUPINFO, mark_processstartupinfo);
  HANDLE mark_imagehandle = nullptr;

  // Instead of an array, CreateProcessA wants a space-separated list
  // of command-line arguments.
  std::string complete_commandline = std::accumulate(
      mark_commandline.Get().begin(), mark_commandline.Get().end(),
      "\"" + mark_name.Get() + "\"",
      [](std::string a, std::string b) { return a + " " + b; });

  // DEBUG_ONLY_THIS_PROCESS implies that we are not going to debug
  // any processes that this process creates.
  // TODO: This could allow a process to perform actions that
  // escape our view.
  bool create_result = CreateProcessA(
      mark_name.Get().c_str(), const_cast<char *>(complete_commandline.c_str()),
      NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL,
      (LPSTARTUPINFOA)(&mark_processstartupinfo), &mark_processinformation);

  if (!create_result) {
    std::cout << "Did not launch mark process with path " << mark_name
              << " because " << win_strerror(GetLastError()) << "\n";
    if (outfile.is_open()) {
      outfile.close();
    }
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

  // Closing these "duplicate" handles is required
  // by the system in order to keep reference counts
  // done properly.
  CloseHandle(mark_processinformation.hProcess);
  CloseHandle(mark_processinformation.hThread);

  for (;;) {
    zero_declare(DEBUG_EVENT, debugEvent);

    WaitForDebugEvent(&debugEvent, INFINITE);

    // We will "continue" with _continue_type_ which will
    // impact how our debugger interacts with other debuggers
    // that are currently executing.
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
        if (debug) {
          std::cout << "Process being created!\n";
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
        TCHAR *dll_filename = filename_from_handle(debugEvent.u.LoadDll.hFile);

        if (dll_filename != nullptr) {
          outputter << mark_name.Get() << " loaded a DLL named " << dll_filename
                    << "\n";
          free(dll_filename);
        } else {
          outputter << mark_name.Get()
                    << " loaded a DLL but could not decipher its filename!\n";
        }
        // Again, even though we watched this DLL be loaded,
        // we don't want to keep a reference to it and potentially
        // keep it open longer than we want!
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

  // If we have attached to a process upon which to spy,
  // we are going to close our handle to that process now.
  if (mark_imagehandle) {
    CloseHandle(mark_imagehandle);
  }

  // In case we are outputing to a file, do a flush!
  outputter << std::flush;
  return 0;
}
