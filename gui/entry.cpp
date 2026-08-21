#ifdef _WIN32

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

extern int logipro_app_main(int argc, char** argv);

std::string utf8_argument(const wchar_t* value) {
    if (value == nullptr || value[0] == L'\0') return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), length, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int wide_argc = 0;
    LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (wide_argv == nullptr) return 1;
    std::vector<std::string> arguments;
    std::vector<char*> argv;
    arguments.reserve(static_cast<std::size_t>(wide_argc));
    argv.reserve(static_cast<std::size_t>(wide_argc) + 1);
    for (int index = 0; index < wide_argc; ++index) arguments.push_back(utf8_argument(wide_argv[index]));
    for (std::string& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const int status = logipro_app_main(wide_argc, argv.data());
    LocalFree(wide_argv);
    return status;
}

#else

extern int logipro_app_main(int argc, char** argv);

int main(int argc, char** argv) {
    return logipro_app_main(argc, argv);
}

#endif
